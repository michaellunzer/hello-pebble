# Blue Pebble

This project aims to create a pebble watch face from a pixel art earth (inspired by blue marble and now hello, world) that is reactive to sun position and illumination. The idea is a geostationary view of the earth where the pixels are gradually turned dark (black marble) as the day progresses.

## Supported platforms

- Basalt (Pebble Time, 144×168)
- Gabbro (Pebble 2, 260×260)

## How it works

1. **Pixel art assets** — Hand-drawn blue marble (day) and black marble (night) PNGs for each platform, stored as 4-bit palette bitmaps.
2. **Orthographic projection** — Each pixel in the image is mapped to a latitude/longitude coordinate using a simple orthographic projection.
3. **Sunrise/sunset lookup** — For each pixel, `suntools::sunriset()` computes sunrise and sunset times across 12 months at 30-minute resolution, producing a 4D boolean daylight array (month × time slot × row × col).
4. **Compression to limits** — The boolean array is compressed to left/right daylight boundary columns per row (with a secondary left2 column for wrap-around cases), halved vertically via nearest-neighbour downsampling.
5. **Binary resource** — The limits array is written as a raw binary (`limits_{platform}.bin`) bundled into the app resources.
6. **On-watch rendering** — The C watch app loads the day and night bitmaps, combines their palettes into a single 4-bit palette, then for each pixel swaps the palette index to day or night based on the limits for the current UTC month and half-hour slot.

## Project structure

- `blue_pixel/` — Pebble C app (built with `pebble build`)
  - `src/c/blue_pixel.c` — Watch face source
  - `resources/` — Bitmap assets and precomputed limits binaries
- `R/` — R helper functions (loaded via `devtools::load_all()`)
  - `projection_helpers.R` — Orthographic pixel → lat/lon projection
  - `time_helpers.R` — Sunrise/sunset computation via suntools
  - `bin_writer.R` — Write binary file pebble can read
  - `view_helpers.R` — Visualisation/debugging plots
- `generate_daynight_map.Rmd` — Generate the binary needed for pebble to generate the merged images on the fly
- `resize_images.py` — Resize source images for each platform


A view of earth from space inspired by the blue marble and hello world images from Artemis and Apollo missions. Presents a fixed view of earth with a transition from day to night (approximated in hourly increments).

## Globe views

Two views are selectable from the watchface settings (Clay config page):

- **Americas (California)** — centred at 23°N 108°W, default in this fork
- **Oceania & SE Asia** — the original hand-drawn view centred at 3.8°S 134°E

### Generating additional views

This fork adds `generate_view.py`, a Python port of the R pipeline that produces the day/night PNGs and the limits binary for any projection centre. It classifies land/ocean/ice from NASA's [land_shallow_topo](https://eoimages.gsfc.nasa.gov/images/imagerecords/57000/57752/land_shallow_topo_2048.jpg) equirectangular map and samples city lights from the [2012 night lights composite](https://eoimages.gsfc.nasa.gov/images/imagerecords/79000/79765/dnb_land_ocean_ice.2012.3600x1800.jpg), quantised to the same 8-colour palette as the hand-drawn art:

```
uv run python generate_view.py --view americas --lat 23 --lon -108 \
    --land land_shallow_topo_2048.jpg --night night_lights.jpg
```

To wire a new view in, add its bitmaps/limits to `blue_pixel/package.json`, a select option in `src/pkjs/config.js`, and a case in the resource pickers in `src/c/blue_pixel.c`.

Note: the limits binaries store hourly (not half-hour) UTC slots, and bitmaps use `"spaceOptimization": "storage"`, so that two full views fit within the 256KB resource limit per platform.

# Troubleshooting
I was running into issues with qemu getting stuck booting and complete reinstall involves.

- uv tool uninstall
- Delete ~/Library/Application Support/Pebble SDK/
- Reinstall