"""Generate globe view assets (day/night PNGs + limits bin) for a projection centre.

Python port of the R pipeline (generate_daynight_map.Rmd + R/*.R) so new views
of the earth can be produced without the R toolchain. Land/ocean/ice pixels are
classified from NASA's land_shallow_topo equirectangular map and city lights
are sampled from the NASA day-night-band composite, then quantised to the same
8-colour palette as the hand-drawn Oceania art.

Usage:
    uv run python generate_view.py --view americas --lat 23 --lon -108 \
        --land /path/to/land_shallow_topo_2048.jpg --night /path/to/night_lights.jpg
    uv run python generate_view.py --validate  # diff regenerated oceania limits
"""

import argparse
import math
import os

import numpy as np
from PIL import Image

RESOURCES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "blue_pixel", "resources")

# Globe geometry per platform, matching the existing hand-drawn assets:
# basalt/emery globes span the full screen width; gabbro (round) is inset.
PLATFORMS = {
    "basalt": {"size": (144, 168), "radius": 72},
    "emery": {"size": (200, 228), "radius": 100},
    "gabbro": {"size": (260, 260), "radius": 90},
}

DAY_COLOURS = {"ocean": (0, 85, 255, 255), "land": (0, 170, 0, 255), "ice": (255, 255, 255, 255)}
NIGHT_COLOURS = {"ocean": (0, 0, 85, 255), "land": (0, 85, 0, 255), "ice": (170, 170, 170, 255),
                 "lights": (255, 255, 170, 255)}
LIGHTS_FRACTION = 0.065  # fraction of land pixels lit at night, matches original art

N_TIMES = 24  # hourly UTC slots (halved from the original 48 to fit basalt's 256KB resource limit with two views)
N_MONTHS = 12
SUN_ALTITUDE_DEG = -0.833  # horizon crossing incl. refraction, as suntools uses


def inverse_orthographic(width, height, radius, lat_ref, lon_ref):
    """Lat/lon (degrees) for every pixel; NaN outside the globe.

    Mirrors R/projection_helpers.R: 1-based pixel coords, x = col - w/2,
    y = h/2 - row.
    """
    cols = np.arange(1, width + 1, dtype=np.float64)
    rows = np.arange(1, height + 1, dtype=np.float64)
    x = cols[None, :] - width / 2.0
    y = height / 2.0 - rows[:, None]
    psi = np.sqrt(x**2 + y**2)
    inside = psi <= radius

    with np.errstate(invalid="ignore", divide="ignore"):
        c = np.arcsin(np.clip(psi / radius, 0.0, 1.0))
        lat0 = math.radians(lat_ref)
        lon0 = math.radians(lon_ref)
        phi = np.arcsin(np.clip(np.cos(c) * math.sin(lat0) + y * np.sin(c) * math.cos(lat0) / psi, -1.0, 1.0))
        lam = lon0 + np.arctan2(x * np.sin(c), psi * np.cos(c) * math.cos(lat0) - y * np.sin(c) * math.sin(lat0))

    # Centre pixel: psi == 0 gives 0/0
    centre = psi == 0
    phi[centre] = math.radians(lat_ref)
    lam[centre] = math.radians(lon_ref)

    lat = np.degrees(phi)
    lon = (np.degrees(lam) + 180.0) % 360.0 - 180.0
    lat[~inside] = np.nan
    lon[~inside] = np.nan
    return lat, lon, inside


def sample_equirect(img_arr, lat, lon):
    """Nearest-neighbour sample of an equirectangular RGB array at lat/lon."""
    h, w = img_arr.shape[:2]
    u = np.clip(((lon + 180.0) / 360.0 * w).astype(np.int64), 0, w - 1)
    v = np.clip(((90.0 - lat) / 180.0 * h).astype(np.int64), 0, h - 1)
    return img_arr[v, u]


def classify_terrain(land_img_path, lat, lon, inside):
    """Classify each globe pixel as ocean/land/ice from the blue marble map."""
    src = Image.open(land_img_path).convert("RGB")
    # Pre-smooth so each sample represents a neighbourhood (cleaner coastlines)
    src = src.resize((1024, 512), Image.Resampling.LANCZOS)
    arr = np.asarray(src, dtype=np.int16)

    lat_f = np.where(inside, lat, 0.0)
    lon_f = np.where(inside, lon, 0.0)
    rgb = sample_equirect(arr, lat_f, lon_f)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]

    ice = (np.minimum(np.minimum(r, g), b) > 160) & inside
    ocean = (~ice) & (b > r + 20) & (b > g + 10) & inside
    land = inside & ~ice & ~ocean
    return {"ocean": ocean, "land": land, "ice": ice}


def pick_city_lights(night_img_path, lat, lon, land_mask):
    """Select the brightest ~6.5% of land pixels as city lights."""
    src = Image.open(night_img_path).convert("L")
    src = src.resize((1800, 900), Image.Resampling.LANCZOS)
    arr = np.asarray(src, dtype=np.float64)

    lat_f = np.where(land_mask, lat, 0.0)
    lon_f = np.where(land_mask, lon, 0.0)
    brightness = sample_equirect(arr, lat_f, lon_f)
    brightness = np.where(land_mask, brightness, -1.0)

    n_land = int(land_mask.sum())
    n_lights = int(round(n_land * LIGHTS_FRACTION))
    if n_lights == 0:
        return np.zeros_like(land_mask)
    flat = brightness.ravel()
    threshold = np.partition(flat, -n_lights)[-n_lights]
    # Require some genuine brightness so empty wilderness never lights up
    threshold = max(threshold, 25.0)
    return (brightness >= threshold) & land_mask


def render_images(terrain, lights_mask, height, width):
    day = np.zeros((height, width, 4), dtype=np.uint8)
    night = np.zeros((height, width, 4), dtype=np.uint8)
    for kind in ("ocean", "land", "ice"):
        day[terrain[kind]] = DAY_COLOURS[kind]
        night[terrain[kind]] = NIGHT_COLOURS[kind]
    night[lights_mask] = NIGHT_COLOURS["lights"]
    return Image.fromarray(day, "RGBA"), Image.fromarray(night, "RGBA")


# --- Solar position (NOAA spreadsheet formulas) ---------------------------

def julian_day(unix_seconds):
    return unix_seconds / 86400.0 + 2440587.5


def solar_declination_and_eot(unix_seconds):
    """Return (declination radians, equation of time in minutes)."""
    T = (julian_day(unix_seconds) - 2451545.0) / 36525.0
    L0 = math.radians((280.46646 + T * (36000.76983 + 0.0003032 * T)) % 360.0)
    M = math.radians(357.52911 + T * (35999.05029 - 0.0001537 * T))
    e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T)
    C = (math.sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T))
         + math.sin(2 * M) * (0.019993 - 0.000101 * T)
         + math.sin(3 * M) * 0.000289)
    true_long = math.degrees(L0) + C
    omega = math.radians(125.04 - 1934.136 * T)
    lam_app = math.radians(true_long - 0.00569 - 0.00478 * math.sin(omega))
    eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0
    eps = math.radians(eps0 + 0.00256 * math.cos(omega))
    decl = math.asin(math.sin(eps) * math.sin(lam_app))
    y = math.tan(eps / 2.0) ** 2
    eot = 4.0 * math.degrees(
        y * math.sin(2 * L0) - 2 * e * math.sin(M)
        + 4 * e * y * math.sin(M) * math.cos(2 * L0)
        - 0.5 * y * y * math.sin(4 * L0) - 1.25 * e * e * math.sin(2 * M))
    return decl, eot


def month_timestamps():
    """Middle of each month, matching the Rmd: 2026-01-01 12:00 UTC + (k+0.5) avg months."""
    base = 1767268800.0  # 2026-01-01 12:00:00 UTC
    avg_month = 365.25 / 12.0 * 86400.0
    return [base + (k + 0.5) * avg_month for k in range(N_MONTHS)]


def compute_daylight(lat, lon, inside):
    """Boolean array (months, times, rows, cols): sun above horizon."""
    h, w = lat.shape
    phi = np.radians(np.where(inside, lat, 0.0))
    sin_phi, cos_phi = np.sin(phi), np.cos(phi)
    lon_h = np.where(inside, lon, 0.0) / 15.0  # longitude in hours
    sin_min = math.sin(math.radians(SUN_ALTITUDE_DEG))

    out = np.zeros((N_MONTHS, N_TIMES, h, w), dtype=bool)
    for m, ts in enumerate(month_timestamps()):
        decl, eot = solar_declination_and_eot(ts)
        sin_d, cos_d = math.sin(decl), math.cos(decl)
        for t in range(N_TIMES):
            utc_h = t * (24.0 / N_TIMES)
            hour_angle = np.radians((utc_h + eot / 60.0 + lon_h - 12.0) * 15.0)
            sin_elev = sin_phi * sin_d + cos_phi * cos_d * np.cos(hour_angle)
            out[m, t] = (sin_elev >= sin_min) & inside
    return out


# --- Compression to limits (port of R/bin_writer.R) ------------------------

def row_min_max(mask):
    """1-based min/max available column per row; (0, -1) when row is empty."""
    h = mask.shape[0]
    mins = np.zeros(h, dtype=np.int64)
    maxs = np.full(h, -1, dtype=np.int64)
    for i in range(h):
        idx = np.flatnonzero(mask[i])
        if idx.size:
            mins[i] = idx[0] + 1
            maxs[i] = idx[-1] + 1
    return mins, maxs


def fill_small_gaps(vals, max_gap=2):
    """Fill night gaps of <= max_gap pixels between day runs (projection artifacts)."""
    out = vals.copy()
    n = len(vals)
    i = 0
    while i < n:
        if not out[i]:
            j = i
            while j < n and not out[j]:
                j += 1
            if i > 0 and j < n and (j - i) <= max_gap:
                out[i:j] = True
            i = j
        else:
            i += 1
    return out


def compress_to_limits(daylight, mask):
    """(months, times, stored_rows, 3) uint8 limits array."""
    h, w = mask.shape
    mins, maxs = row_min_max(mask)
    stored = list(range(0, h, 2))  # 0-based display rows 0,2,4,... (R rows 1,3,5,...)
    limits = np.zeros((N_MONTHS, N_TIMES, len(stored), 3), dtype=np.uint8)

    for si, r0 in enumerate(stored):
        r = r0
        pair = r0 + 1 if r0 + 1 < h else None
        if maxs[r] < mins[r] or maxs[r] == -1:
            if pair is not None and maxs[pair] >= mins[pair] and maxs[pair] != -1:
                r, pair = pair, None
            else:
                continue  # outside globe: leave zeros
        row_min, row_max = int(mins[r]), int(maxs[r])
        pair_min = int(mins[pair]) if pair is not None and maxs[pair] != -1 else None
        pair_max = int(maxs[pair]) if pair is not None and maxs[pair] != -1 else None

        eff_min = min(row_min, pair_min) if pair_min else row_min
        eff_max = max(row_max, pair_max) if pair_max else row_max
        sentinel = min(eff_max + 1, 255)

        for m in range(N_MONTHS):
            for t in range(N_TIMES):
                vals = np.zeros(eff_max - eff_min + 1, dtype=bool)
                rs, re = row_min - eff_min, row_max - eff_min
                vals[rs:re + 1] = daylight[m, t, r, row_min - 1:row_max]
                if pair_min and eff_min < row_min:
                    vals[:row_min - eff_min] = daylight[m, t, pair, eff_min - 1:row_min - 1]
                if pair_max and eff_max > row_max:
                    vals[re + 1:] = daylight[m, t, pair, row_max:eff_max]

                vals = fill_small_gaps(vals)
                day_cols = np.flatnonzero(vals) + eff_min  # 1-based columns

                if day_cols.size == 0:
                    limits[m, t, si] = (sentinel, 0, sentinel)
                    continue
                gaps = np.diff(day_cols)
                gap_idx = np.flatnonzero(gaps > 1)
                if gap_idx.size == 0:
                    limits[m, t, si] = (min(day_cols[0], 255), min(day_cols[-1], 255), sentinel)
                else:
                    split = gap_idx[np.argmax(gaps[gap_idx])]
                    limits[m, t, si] = (min(day_cols[0], 255),
                                        min(day_cols[split], 255),
                                        min(day_cols[split + 1], 255))
    return limits


def write_limits_bin(limits, path):
    """Sequential blocks: time (outer) x month (inner), each [left|right|left2] x rows."""
    with open(path, "wb") as f:
        for t in range(N_TIMES):
            for m in range(N_MONTHS):
                for lc in range(3):
                    f.write(limits[m, t, :, lc].astype(np.uint8).tobytes())


# --- Entry points -----------------------------------------------------------

def generate_view(view, lat_ref, lon_ref, land_path, night_path):
    for platform, spec in PLATFORMS.items():
        width, height = spec["size"]
        lat, lon, inside = inverse_orthographic(width, height, spec["radius"], lat_ref, lon_ref)
        terrain = classify_terrain(land_path, lat, lon, inside)
        lights = pick_city_lights(night_path, lat, lon, terrain["land"])
        day_img, night_img = render_images(terrain, lights, height, width)

        day_img.save(os.path.join(RESOURCES, f"blue_marble_{view}_{platform}.png"))
        night_img.save(os.path.join(RESOURCES, f"black_marble_{view}_{platform}.png"))

        daylight = compute_daylight(lat, lon, inside)
        limits = compress_to_limits(daylight, inside)
        write_limits_bin(limits, os.path.join(RESOURCES, f"limits_{view}_{platform}.bin"))
        print(f"{platform}: {int(terrain['land'].sum())} land / {int(terrain['ocean'].sum())} ocean / "
              f"{int(terrain['ice'].sum())} ice / {int(lights.sum())} lights; limits written")


def validate_against_oceania():
    """Regenerate oceania limits from the hand-drawn basalt art and diff the repo bin."""
    img = Image.open(os.path.join(RESOURCES, "blue_marble_basalt.png")).convert("RGBA")
    mask = np.asarray(img)[..., 3] > 0
    h, w = mask.shape
    mins, maxs = row_min_max(mask)
    radius = (maxs - mins).max() / 2.0
    lat, lon, _ = inverse_orthographic(w, h, radius, -3.84, 133.96)
    inside = mask & ~np.isnan(lat)

    daylight = compute_daylight(lat, lon, inside)
    limits = compress_to_limits(daylight, mask)

    with open(os.path.join(RESOURCES, "limits_basalt.bin"), "rb") as f:
        ref = np.frombuffer(f.read(), dtype=np.uint8)
    mine = np.zeros_like(ref)
    i = 0
    stored = (h + 1) // 2
    for t in range(N_TIMES):
        for m in range(N_MONTHS):
            for lc in range(3):
                mine[i:i + stored] = limits[m, t, :, lc]
                i += stored
    diff = np.abs(mine.astype(np.int32) - ref.astype(np.int32))
    print(f"bytes: {ref.size}, exact match: {(diff == 0).mean():.1%}, "
          f"|diff|<=2 px: {(diff <= 2).mean():.1%}, mean |diff|: {diff.mean():.2f}, max: {diff.max()}")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--view", default="americas")
    p.add_argument("--lat", type=float, default=23.0)
    p.add_argument("--lon", type=float, default=-108.0)
    p.add_argument("--land", help="path to land_shallow_topo equirect jpg")
    p.add_argument("--night", help="path to night lights equirect jpg")
    p.add_argument("--validate", action="store_true")
    args = p.parse_args()

    if args.validate:
        validate_against_oceania()
    else:
        generate_view(args.view, args.lat, args.lon, args.land, args.night)
