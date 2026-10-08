/*
 * Ergo S1 dongle status screen. Adapts to the OLED height:
 *
 * 128x64 (0.96"):                     128x32 (0.91"):
 *   [USB/BT1]               Base       [kb]  L 85%      R 72%
 *   L [batt] 85%   R [batt] 72%        [kb]  [USB] 42 wpm  Base
 *   +---------------+        42
 *   | pixel keyb.   |       wpm
 *   +---------------+
 *
 * The pixel keyboard's keys tap faster as your WPM rises (idle when you stop).
 *
 * Battery labels: ZMK numbers the halves by connection order, which can change
 * between power-ups, so the screen watches key presses and learns which
 * connection is the left half and which is the right. Until a key has been
 * pressed on each half it assumes the first connection is the left half.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/endpoints.h>
#include <zmk/wpm.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#define NUM_PERIPHERALS 2
#define SIDE_LEFT 0
#define SIDE_RIGHT 1
#define TEXT_X 36

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define TALL_SCREEN (DT_PROP(DISPLAY_NODE, height) >= 64)

static const lv_font_t *font(void) { return &lv_font_montserrat_12; }

/* ------------------------------------------------------------------ */
/* Batteries                                                            */
/* ------------------------------------------------------------------ */

/* Key positions (keymap order) that belong to the left half. */
static bool position_is_left(uint32_t pos) {
    if (pos < 48) {
        return (pos % 12) < 6; /* rows 0-3: 6 left keys then 6 right keys */
    }
    if (pos < 56) {
        return pos < 52; /* bottom row: 4 left, 4 right */
    }
    return pos < 62; /* thumbs: 6 left, 6 right */
}

struct batt_state {
    uint8_t level[NUM_PERIPHERALS]; /* indexed by side; 0 = not connected */
};

static uint8_t slot_level[NUM_PERIPHERALS];
static int8_t slot_side[NUM_PERIPHERALS] = {SIDE_LEFT, SIDE_RIGHT};
static lv_obj_t *side_label[NUM_PERIPHERALS];

static const char *batt_symbol(uint8_t level) {
    if (level > 90) return LV_SYMBOL_BATTERY_FULL;
    if (level > 65) return LV_SYMBOL_BATTERY_3;
    if (level > 35) return LV_SYMBOL_BATTERY_2;
    if (level > 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void batt_update_cb(struct batt_state state) {
    static const char *names[NUM_PERIPHERALS] = {"L", "R"};
    for (int side = 0; side < NUM_PERIPHERALS; side++) {
        if (side_label[side] == NULL) {
            continue;
        }
        uint8_t lvl = state.level[side];
        if (lvl == 0) {
            lv_label_set_text_fmt(side_label[side], "%s --", names[side]);
        } else if (TALL_SCREEN) {
            lv_label_set_text_fmt(side_label[side], "%s %s %u%%", names[side], batt_symbol(lvl), lvl);
        } else {
            lv_label_set_text_fmt(side_label[side], "%s %u%%", names[side], lvl);
        }
    }
}

static struct batt_state batt_get_state(const zmk_event_t *eh) {
    if (eh != NULL) {
        const struct zmk_peripheral_battery_state_changed *bev =
            as_zmk_peripheral_battery_state_changed(eh);
        if (bev != NULL && bev->source < NUM_PERIPHERALS) {
            slot_level[bev->source] = bev->state_of_charge;
        }

        const struct zmk_position_state_changed *pev = as_zmk_position_state_changed(eh);
        if (pev != NULL && pev->source < NUM_PERIPHERALS) {
            int8_t side = position_is_left(pev->position) ? SIDE_LEFT : SIDE_RIGHT;
            if (slot_side[pev->source] != side) {
                slot_side[pev->source] = side;
                slot_side[1 - pev->source] = 1 - side;
            }
        }
    }

    struct batt_state st = {0};
    for (int slot = 0; slot < NUM_PERIPHERALS; slot++) {
        st.level[slot_side[slot]] = slot_level[slot];
    }
    return st;
}

ZMK_DISPLAY_WIDGET_LISTENER(ergo_batt, struct batt_state, batt_update_cb, batt_get_state)
ZMK_SUBSCRIPTION(ergo_batt, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(ergo_batt, zmk_position_state_changed);

/* ------------------------------------------------------------------ */
/* Output: small USB / Bluetooth icon                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *output_label;

struct out_state {
    struct zmk_endpoint_instance ep;
};

static void out_update_cb(struct out_state st) {
    if (output_label == NULL) {
        return;
    }
    if (st.ep.transport == ZMK_TRANSPORT_USB) {
        lv_label_set_text(output_label, LV_SYMBOL_USB);
    } else {
        lv_label_set_text_fmt(output_label, LV_SYMBOL_BLUETOOTH "%i", st.ep.ble.profile_index + 1);
    }
}

static struct out_state out_get_state(const zmk_event_t *eh) {
    return (struct out_state){.ep = zmk_endpoints_selected()};
}

ZMK_DISPLAY_WIDGET_LISTENER(ergo_out, struct out_state, out_update_cb, out_get_state)
ZMK_SUBSCRIPTION(ergo_out, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(ergo_out, zmk_ble_active_profile_changed);
#endif

/* ------------------------------------------------------------------ */
/* WPM text + typing animation                                         */
/* ------------------------------------------------------------------ */

static lv_obj_t *wpm_label;
static lv_obj_t *wpm_unit_label;
static volatile int current_wpm;

struct wpm_state {
    int wpm;
};

static void wpm_update_cb(struct wpm_state st) {
    current_wpm = st.wpm;
    if (wpm_label != NULL) {
        if (wpm_unit_label != NULL) {
            lv_label_set_text_fmt(wpm_label, "%i", st.wpm); /* tall layout: unit on its own line */
        } else {
            lv_label_set_text_fmt(wpm_label, "%i wpm", st.wpm);
        }
    }
}

static struct wpm_state wpm_get_state(const zmk_event_t *eh) {
    const struct zmk_wpm_state_changed *ev = (eh != NULL) ? as_zmk_wpm_state_changed(eh) : NULL;
    return (struct wpm_state){.wpm = (ev != NULL) ? ev->state : zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(ergo_wpm, struct wpm_state, wpm_update_cb, wpm_get_state)
ZMK_SUBSCRIPTION(ergo_wpm, zmk_wpm_state_changed);

/* Tiny keyboard: 3 rows x 5 keys plus a space bar. */
#define KB_ROWS 3
#define KB_COLS 5
#define KB_KEYS (KB_ROWS * KB_COLS + 1) /* last one is the space bar */

static lv_obj_t *kb_key[KB_KEYS];
static int kb_pressed = -1;
static uint32_t kb_elapsed;
static uint32_t kb_seed = 0x2545F491;
static lv_color_t ink;

static void kb_set(int idx, bool down) {
    if (idx < 0) {
        return;
    }
    lv_obj_set_style_bg_opa(kb_key[idx], down ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
}

static void kb_timer_cb(lv_timer_t *t) {
    int wpm = current_wpm;
    kb_elapsed += 40;

    if (wpm <= 0) {
        kb_set(kb_pressed, false); /* idle: hands off the keyboard */
        kb_pressed = -1;
        return;
    }

    /* Faster typing -> faster taps: ~400 ms per tap at 20 wpm, ~90 ms at 120 wpm. */
    uint32_t interval = 1200 / (wpm / 10 + 2);
    if (interval < 80) {
        interval = 80;
    }
    if (kb_elapsed < interval) {
        return;
    }
    kb_elapsed = 0;

    kb_set(kb_pressed, false);
    kb_seed = kb_seed * 1103515245u + 12345u;
    uint32_t r = (kb_seed >> 16) % 6;
    int next = (r == 0) ? KB_KEYS - 1 : (int)((kb_seed >> 8) % (KB_ROWS * KB_COLS));
    if (next == kb_pressed) {
        next = (next + 3) % (KB_ROWS * KB_COLS);
    }
    kb_pressed = next;
    kb_set(kb_pressed, true);
}

static lv_obj_t *kb_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                        lv_coord_t h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, ink, LV_PART_MAIN);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, ink, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void kb_create(lv_obj_t *screen, lv_coord_t ox, lv_coord_t oy, lv_coord_t kw,
                      lv_coord_t kh, lv_coord_t gx, lv_coord_t gy) {
    ink = lv_obj_get_style_text_color(screen, LV_PART_MAIN);

    lv_coord_t inner_w = KB_COLS * kw + (KB_COLS - 1) * gx;
    lv_coord_t case_w = inner_w + 4;
    lv_coord_t space_y = oy + 2 + KB_ROWS * (kh + gy);
    lv_coord_t case_h = (space_y - oy) + kh - 1 + 3;

    kb_box(screen, ox, oy, case_w, case_h); /* keyboard case */
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            kb_key[r * KB_COLS + c] =
                kb_box(screen, ox + 2 + c * (kw + gx), oy + 2 + r * (kh + gy), kw, kh);
        }
    }
    /* space bar spans the middle three key columns */
    kb_key[KB_KEYS - 1] =
        kb_box(screen, ox + 2 + (kw + gx), space_y, 3 * kw + 2 * gx, kh - 1);

    lv_timer_create(kb_timer_cb, 40, NULL);
}

/* ------------------------------------------------------------------ */
/* Screen                                                               */
/* ------------------------------------------------------------------ */

static struct zmk_widget_layer_status layer_status_widget;

static lv_obj_t *text_label(lv_obj_t *screen, lv_align_t align, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(screen);
    lv_obj_set_style_text_font(l, font(), LV_PART_MAIN);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, x, y);
    return l;
}

static void layer_create(lv_obj_t *screen, lv_coord_t width, lv_align_t align) {
    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_t *layer = zmk_widget_layer_status_obj(&layer_status_widget);
    lv_obj_set_style_text_font(layer, font(), LV_PART_MAIN);
    lv_obj_set_width(layer, width);
    lv_label_set_long_mode(layer, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(layer, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(layer, align, 0, 0);
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    if (TALL_SCREEN) {
        /* 128x64: output + layer / batteries / keyboard + WPM */
        output_label = text_label(screen, LV_ALIGN_TOP_LEFT, 0, 0);
        layer_create(screen, 80, LV_ALIGN_TOP_RIGHT);

        side_label[SIDE_LEFT] = text_label(screen, LV_ALIGN_TOP_LEFT, 0, 16);
        side_label[SIDE_RIGHT] = text_label(screen, LV_ALIGN_TOP_RIGHT, 0, 16);

        kb_create(screen, 0, 34, 9, 5, 2, 1); /* 51 x 29 px keyboard */

        wpm_label = lv_label_create(screen);
        lv_obj_set_style_text_font(wpm_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_label_set_text(wpm_label, "");
        lv_obj_align(wpm_label, LV_ALIGN_BOTTOM_RIGHT, 0, -14);
        wpm_unit_label = text_label(screen, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_label_set_text(wpm_unit_label, "wpm");
    } else {
        /* 128x32: keyboard on the left, two text rows on the right */
        kb_create(screen, 0, 6, 4, 3, 2, 1);

        side_label[SIDE_LEFT] = text_label(screen, LV_ALIGN_TOP_LEFT, TEXT_X, 0);
        side_label[SIDE_RIGHT] = text_label(screen, LV_ALIGN_TOP_RIGHT, 0, 0);

        output_label = text_label(screen, LV_ALIGN_BOTTOM_LEFT, TEXT_X, 0);
        wpm_label = text_label(screen, LV_ALIGN_BOTTOM_LEFT, TEXT_X + 20, 0);
        layer_create(screen, 34, LV_ALIGN_BOTTOM_RIGHT);
    }

    ergo_batt_init();
    ergo_out_init();
    ergo_wpm_init();
    return screen;
}
