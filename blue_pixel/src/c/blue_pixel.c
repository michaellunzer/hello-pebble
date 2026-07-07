#include <pebble.h>
#include <stdlib.h>

static Window *s_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static GBitmap *s_bitmap;
static GBitmap *s_day_bitmap;
static GBitmap *s_night_bitmap;
static BitmapLayer *s_bitmap_layer;
static bool s_palettes_combined = false;
static bool s_show_date = true;
static int s_globe_view = 0;

#define PERSIST_KEY_SHOW_DATE 0
#define PERSIST_KEY_GLOBE_VIEW 1

#define GLOBE_VIEW_OCEANIA 0
#define GLOBE_VIEW_AMERICAS 1

static uint32_t day_resource_id(void) {
    return s_globe_view == GLOBE_VIEW_AMERICAS ? RESOURCE_ID_BLUE_MARBLE_AMERICAS : RESOURCE_ID_BLUE_MARBLE;
}

static uint32_t night_resource_id(void) {
    return s_globe_view == GLOBE_VIEW_AMERICAS ? RESOURCE_ID_BLACK_MARBLE_AMERICAS : RESOURCE_ID_BLACK_MARBLE;
}

static uint32_t limits_resource_id(void) {
    return s_globe_view == GLOBE_VIEW_AMERICAS ? RESOURCE_ID_LIMITS_AMERICAS : RESOURCE_ID_LIMITS;
}

static void update_background_image(struct tm *utc_time) {
    // It is my understanding that the palette bitmap types allow you to have x many colours from the palette. So 2BitPalette allows for 4 colours.
    // check if day_bitmap and night_bitmap are already loaded, if not load them
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Updating background image for time: %d:%d", utc_time->tm_hour, utc_time->tm_min);
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Memory START: Free=%lu Used=%lu", 
    //         (unsigned long)heap_bytes_free(), (unsigned long)heap_bytes_used());
    
    if (s_day_bitmap == NULL) {
        s_day_bitmap = gbitmap_create_with_resource(day_resource_id());
        s_palettes_combined = false; // reset this flag whenever we load new bitmaps to ensure palettes are combined correctly
    }
    if (s_night_bitmap == NULL) {
        s_night_bitmap = gbitmap_create_with_resource(night_resource_id());
        s_palettes_combined = false;
    }
    if (!s_day_bitmap || !s_night_bitmap) { return; }

    // Print the palette colours
    int n_colours_per_palette = 8;
    GColor *day_palette = gbitmap_get_palette(s_day_bitmap);

    if (!s_palettes_combined) {
        // Only combine if we are reloading the bitmaps otherwise we garble the palette
        GColor *night_palette = gbitmap_get_palette(s_night_bitmap);
        // combine the two palettes
        for (int i = 0; i < n_colours_per_palette; i++) {
            day_palette[i+n_colours_per_palette] = night_palette[i];
        }

        // logging
        // APP_LOG(APP_LOG_LEVEL_DEBUG, "Day bitmap palette:");
        // for (int i = 0; i < n_colours_per_palette; i++) {
        //     GColor color = day_palette[i];
        //     APP_LOG(APP_LOG_LEVEL_DEBUG, "Color %d: R=%d G=%d B=%d A=%d", i, color.r, color.g, color.b, color.a);
        // }
        // APP_LOG(APP_LOG_LEVEL_DEBUG, "Night bitmap palette:");
        // for (int i = 0; i < n_colours_per_palette; i++) {
        //     GColor color = night_palette[i];
        //     APP_LOG(APP_LOG_LEVEL_DEBUG, "Color %d: R=%d G=%d B=%d A=%d", i, color.r, color.g, color.b, color.a);
        // }
        // APP_LOG(APP_LOG_LEVEL_DEBUG, "Combined bitmap palette:");
        // for (int i = 0; i < n_colours_per_palette*2; i++) {
        //     GColor color = day_palette[i];
        //     APP_LOG(APP_LOG_LEVEL_DEBUG, "Color %d: R=%d G=%d B=%d A=%d", i, color.r, color.g, color.b, color.a);
        // }

        gbitmap_set_palette(s_day_bitmap, day_palette, false);
        gbitmap_set_palette(s_night_bitmap, day_palette, false);
        s_palettes_combined = true;
    }

    // Knitting - combine day and night bitmaps
    // create a copy of day then modify
    uint8_t *night_data = gbitmap_get_data(s_night_bitmap);
    unsigned int bytes_per_row = gbitmap_get_bytes_per_row(s_day_bitmap);
    GRect bounds = gbitmap_get_bounds(s_day_bitmap);
    unsigned int rows = bounds.size.h;
    unsigned int cols = bounds.size.w;

    // Before creating new bitmap, destroy the old one
    if (s_bitmap != NULL) {
        gbitmap_destroy(s_bitmap);
        s_bitmap = NULL;
    }

    // Create the blank bitmap first and use its internal buffer directly to avoid malloc/free issues
    s_bitmap = gbitmap_create_blank(bounds.size, gbitmap_get_format(s_day_bitmap));
    uint8_t *comb_data = gbitmap_get_data(s_bitmap);
    memcpy(comb_data, gbitmap_get_data(s_day_bitmap), bytes_per_row * rows);

    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Bitmap dimensions: %d cols x %d rows, bytes per row: %d", cols, rows, bytes_per_row);

    // Get pregenerated limits array
    ResHandle handle = resource_get_handle(limits_resource_id());
    // Limits binary stores every other row (half the display rows) with 3 limit columns
    unsigned int stored_rows = (rows + 1) / 2;  // ceil(rows/2) = 84 for 168
    size_t block_size_bytes = stored_rows * 3 * sizeof(uint8_t);
    uint8_t *s_buffer = (uint8_t*)malloc(block_size_bytes);
    if (!s_buffer) { return; }

    // time_idx: 0-based hour index (0..23)
    int time_idx = utc_time->tm_hour;
    // month: 0-based (tm_mon is already 0-based in C)
    int month = utc_time->tm_mon;
    int block_index = time_idx * 12 + month;
    int byte_offset = block_index * (int)block_size_bytes;
    resource_load_byte_range(handle, byte_offset, (uint8_t*)s_buffer, block_size_bytes);
    // s_buffer layout: [left × stored_rows] [right × stored_rows] [left2 × stored_rows]

    for (unsigned int ii = 0; ii < rows; ii++) {
        unsigned int row_byte_num = ii * bytes_per_row;
        unsigned int stored_row = ii / 2;  // nearest-neighbor: map display row to stored row
        unsigned int left_limit = s_buffer[stored_row];
        unsigned int right_limit = s_buffer[stored_row + stored_rows];
        unsigned int left2_limit = s_buffer[stored_row + 2 * stored_rows];

        for (unsigned int jj = 0; jj < bytes_per_row; jj++) {
            unsigned int px_left = jj * 2 + 1;   // 1-based, matching R's 1-based limit values
            unsigned int px_right = jj * 2 + 2;  // 1-based

            // Determine if each pixel in this byte is night
            // Day if: (px >= left && px <= right) || (left2 <= max_col && px >= left2)
            // Night otherwise. left > right means all-night row.
            bool px1_night = true;
            bool px2_night = true;

            // Check primary daylight block (left..right)
            if (left_limit <= right_limit) {
                if (px_left >= left_limit && px_left <= right_limit) px1_night = false;
                if (px_right >= left_limit && px_right <= right_limit) px2_night = false;
            }

            // Check secondary daylight block (left2..end of available pixels)
            // left2 > right_limit signals no second block (sentinel)
            // but we also need left2 to be a valid column
            if (left2_limit > left_limit && left2_limit <= right_limit) {
                // left2 is within the primary block - not a valid second block
            } else if (left2_limit > 0 && left2_limit <= cols) {
                // Valid second block from left2 to end of available pixels in row
                if (px_left >= left2_limit) px1_night = false;
                if (px_right >= left2_limit) px2_night = false;
            }

            if (px1_night || px2_night) {
                uint8_t night_byte = night_data[row_byte_num + jj];
                uint8_t day_byte   = comb_data[row_byte_num + jj];  // initialised from day bitmap

                // For each pixel: take from day bitmap by default,
                // replace with shifted night value only if that pixel is night.
                uint8_t pixel1 = (day_byte >> 4) & 0x0F;
                uint8_t pixel2 = day_byte & 0x0F;

                if (px1_night) pixel1 = ((night_byte >> 4) & 0x0F) + n_colours_per_palette;
                if (px2_night) pixel2 = (night_byte & 0x0F) + n_colours_per_palette;

                comb_data[row_byte_num + jj] = (pixel1 << 4) | pixel2;
            }
        }
    }
   
    // Free the buffer after use
    if (s_buffer) {
        free(s_buffer);
    }
        
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Memory merged: Free=%lu Used=%lu", 
    //         (unsigned long)heap_bytes_free(), (unsigned long)heap_bytes_used());

    // set the palette on the already-created bitmap
    gbitmap_set_palette(s_bitmap, day_palette, false);
        
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Memory assigned: Free=%lu Used=%lu", 
    //         (unsigned long)heap_bytes_free(), (unsigned long)heap_bytes_used());
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "I am about to set the bitmap on the layer");
    bitmap_layer_set_bitmap(s_bitmap_layer, s_bitmap);
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "I have set the bitmap on the layer");
    
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Memory END: Free=%lu Used=%lu", 
    //         (unsigned long)heap_bytes_free(), (unsigned long)heap_bytes_used());
}

static void update_layout(void) {
    if (!s_time_layer || !s_date_layer) return;

    Layer *window_layer = window_get_root_layer(s_window);
    GRect bounds = layer_get_bounds(window_layer);

    const int time_h = 55;
    const int date_h = 26;
    const int gap = 2;

    if (s_show_date) {
        int group_h = time_h + gap + date_h;
        int group_top = (bounds.size.h - group_h) / 2;
        layer_set_frame(text_layer_get_layer(s_time_layer),
                        GRect(0, group_top, bounds.size.w, time_h));
        layer_set_frame(text_layer_get_layer(s_date_layer),
                        GRect(0, group_top + time_h + gap, bounds.size.w, date_h));
        layer_set_hidden(text_layer_get_layer(s_date_layer), false);
    } else {
        int time_top = (bounds.size.h - time_h) / 2;
        layer_set_frame(text_layer_get_layer(s_time_layer),
                        GRect(0, time_top, bounds.size.w, time_h));
        layer_set_hidden(text_layer_get_layer(s_date_layer), true);
    }
}

static void update_time(bool force_bgd_update) {
    time_t temp = time(NULL);
    struct tm *tick_time = localtime(&temp);

    static char s_time_buffer[8];
    strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
    text_layer_set_text(s_time_layer, s_time_buffer);

    static char s_date_buffer[12];
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (s_show_date) {
        snprintf(s_date_buffer, sizeof(s_date_buffer), "%d %s", tick_time->tm_mday, months[tick_time->tm_mon]);
        text_layer_set_text(s_date_layer, s_date_buffer);
    }

    // Capture local minute before gmtime() may overwrite the shared tm buffer
    int local_min = tick_time->tm_min;
    struct tm *utc_time = gmtime(&temp);
    if (local_min == 0 || local_min == 30 || force_bgd_update) {
        update_background_image(utc_time);
    }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time(false);
}

static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
    Tuple *show_date_t = dict_find(iterator, MESSAGE_KEY_SHOW_DATE);
    if (show_date_t) {
        s_show_date = (bool)show_date_t->value->int32;
        persist_write_bool(PERSIST_KEY_SHOW_DATE, s_show_date);
        update_layout();
    }

    Tuple *globe_view_t = dict_find(iterator, MESSAGE_KEY_GLOBE_VIEW);
    if (globe_view_t) {
        // Clay sends select values as strings
        int view = globe_view_t->type == TUPLE_CSTRING
            ? atoi(globe_view_t->value->cstring)
            : (int)globe_view_t->value->int32;
        if (view != s_globe_view) {
            s_globe_view = view;
            persist_write_int(PERSIST_KEY_GLOBE_VIEW, s_globe_view);
            // Drop the loaded bitmaps so the new view's resources get loaded
            if (s_day_bitmap) {
                gbitmap_destroy(s_day_bitmap);
                s_day_bitmap = NULL;
            }
            if (s_night_bitmap) {
                gbitmap_destroy(s_night_bitmap);
                s_night_bitmap = NULL;
            }
            s_palettes_combined = false;
            update_time(true);
        }
    }
}

static void prv_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_background_color(window, GColorBlack);

    // Bitmap layer (background) — added first so text layers render on top
    s_bitmap_layer = bitmap_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
    bitmap_layer_set_compositing_mode(s_bitmap_layer, GCompOpSet);
    layer_add_child(window_layer, bitmap_layer_get_layer(s_bitmap_layer));

    // Time layer
    s_time_layer = text_layer_create(GRect(0, 0, bounds.size.w, 55));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorWhite);
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

    // Date layer
    s_date_layer = text_layer_create(GRect(0, 0, bounds.size.w, 26));
    text_layer_set_background_color(s_date_layer, GColorClear);
    text_layer_set_text_color(s_date_layer, GColorWhite);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_ROBOTO_CONDENSED_21));
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
    layer_add_child(window_layer, text_layer_get_layer(s_date_layer));

    update_layout();
    update_time(true);
}

static void prv_window_unload(Window *window) {
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
}

static void prv_init(void) {
    // Load persisted settings; default to showing the date over Australia
    s_show_date = persist_exists(PERSIST_KEY_SHOW_DATE) ? persist_read_bool(PERSIST_KEY_SHOW_DATE) : true;
    s_globe_view = persist_exists(PERSIST_KEY_GLOBE_VIEW) ? persist_read_int(PERSIST_KEY_GLOBE_VIEW) : GLOBE_VIEW_OCEANIA;

    // Entry point: create the main window and set up handlers
    s_window = window_create();
    // window_set_click_config_provider(s_window, prv_click_config_provider);
    window_set_window_handlers(s_window, (WindowHandlers) {
        .load = prv_window_load,
        .unload = prv_window_unload,
    });

    // AppMessage — receive settings from Clay config page
    app_message_register_inbox_received(inbox_received_handler);
    app_message_open(64, 64);

    // Time
    // Register with TickTimerService - MINUTE_UNIT means we'll get a callback every minute
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

    // Show the Window on the watch, with animated=true
    const bool animated = true;
    window_stack_push(s_window, animated);
}

static void prv_deinit(void) {
    // Destroy the Window to free resources
    window_destroy(s_window);
    // Clean up the GBitmap and BitmapLayer resources
    bitmap_layer_destroy(s_bitmap_layer);
    if (s_bitmap) {
        gbitmap_destroy(s_bitmap);
        s_bitmap = NULL;
    }
    if (s_day_bitmap) {
        gbitmap_destroy(s_day_bitmap);
    }
    if (s_night_bitmap) {
        gbitmap_destroy(s_night_bitmap);
    }
}

int main(void) {
    prv_init();

    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p", s_window);

    app_event_loop();
    prv_deinit();
}
