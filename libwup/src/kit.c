/* kit: see include/wup_kit.h. The parts are the menu's (menu/ui.c), sizes and colours the same. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "wup_kit.h"
#include "handoff.h"
#include "wup_tone.h"

#define HUD 20                                       /* the size of everything in the bars, as in the menu */
static kit_screen stack[8];
static int depth, focus_at[8], held_b, quiet;
static void (*deferred)(void);
static lv_obj_t *status, *battery, *clk;
static int16_t click[2 * CLICK];
static int audio_on, ui;

int kit_dark(void) { return (ui & WUP_UI_SET) && (ui & WUP_UI_DARK); }
static lv_color_t ink(void) { return kit_dark() ? lv_color_hex(0xf2f2f2) : lv_color_hex(0x202124); }
static lv_color_t paper(void) { return kit_dark() ? lv_color_hex(0x15171a) : lv_color_white(); }

void kit_click(void) {
    static uint32_t last;
    if (ui & WUP_UI_QUIET) return;
    uint32_t now = wup_us();
    if (now - last < 40000) return;                  /* (one click for one action) */
    last = now;
    if (!audio_on) audio_on = wup_audio_open() == 0;
    if (audio_on) wup_audio_queue(click, CLICK);
}
static void kit_note_sound(int notice) { (void)notice; kit_click(); }   /* an alert or a notice appears: the click too */

/* ---- the status: Wi-Fi, charging, the battery, the time */
static void status_text(void) {
    if (!status) return;
    int charging = 0, p = wup_battery(&charging), on = wup_wifi_rssi() != 0;
#if defined(WUP_KIT_PREVIEW) || defined(CATALOG_PREVIEW)   /* (pictures from wup-emu, which has no Wi-Fi: as the menu's MENU_PREVIEW) */
    on = 1; p = 66; charging = 0;
#endif
    char t[24];
    snprintf(t, sizeof t, "%s%s%s", on ? LV_SYMBOL_WIFI : "", on && charging ? "  " : "", charging ? LV_SYMBOL_CHARGE : "");
    if (strcmp(lv_label_get_text(status), t)) lv_label_set_text(status, t);
    lv_obj_set_width(battery, p < 0 ? 0 : 2 + p * 22 / 100);
    {                                              /* charging: the bolt alone; not: the battery alone */
        lv_obj_t *whole = lv_obj_get_parent(lv_obj_get_parent(battery));
        if (charging) lv_obj_add_flag(whole, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(whole, LV_OBJ_FLAG_HIDDEN);
    }
    char c[16] = "";
    if (wup_time_known()) {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        int h = tm.tm_hour; if (!(ui & WUP_UI_24H)) { h %= 12; if (!h) h = 12; }
        snprintf(c, sizeof c, ui & WUP_UI_24H ? "%02d:%02d" : "%d:%02d", h, tm.tm_min);
    }
    if (clk && strcmp(lv_label_get_text(clk), c)) lv_label_set_text(clk, c);
}

/* ---- the parts of a screen */
static void hint_bar(lv_obj_t *scr, const char *hints) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 44); lv_obj_set_style_pad_hor(bar, 32, 0); lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (const char *p = hints; *p; ) {              /* "B Back    A Select": a round badge, then what it does */
        char key[2] = { *p, 0 }, what[32]; int n = 0;
        p++; while (*p == ' ') p++;
        while (*p && !(p[0] == ' ' && p[1] == ' ') && n < 31) what[n++] = *p++;
        what[n] = 0; while (*p == ' ') p++;
        lv_obj_t *badge = lv_obj_create(bar);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, HUD, HUD); lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, ink(), 0); lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_t *k = lv_label_create(badge);
        lv_label_set_text(k, key); lv_obj_set_style_text_font(k, &inter_hud, 0); lv_obj_set_style_text_color(k, paper(), 0); lv_obj_align(k, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *t = lv_label_create(bar);
        lv_label_set_text(t, what); lv_obj_set_style_text_font(t, &inter_16, 0); lv_obj_set_style_text_opa(t, LV_OPA_70, 0);
        lv_obj_set_style_pad_right(t, 16, 0);
    }
}
lv_obj_t *kit_page(lv_obj_t *scr, const char *title, const char *hints) {
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0); lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 64); lv_obj_set_style_pad_hor(bar, 32, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title); lv_obj_set_style_text_font(t, &inter_32, 0);
    lv_obj_t *st = lv_obj_create(bar);              /* Wi-Fi, charging, the battery and the time, as in the menu */
    lv_obj_remove_style_all(st); lv_obj_set_size(st, LV_SIZE_CONTENT, 28); lv_obj_set_style_pad_column(st, 10, 0);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    status = lv_label_create(st); lv_label_set_text(status, ""); lv_obj_set_style_text_color(status, ink(), 0);
    lv_obj_t *bat = lv_obj_create(st);
    lv_obj_remove_style_all(bat); lv_obj_set_size(bat, 36, HUD - 4);
    lv_obj_t *body = lv_obj_create(bat);
    lv_obj_remove_style_all(body); lv_obj_set_size(body, 32, HUD - 4); lv_obj_set_style_radius(body, 4, 0);
    lv_obj_set_style_border_width(body, 2, 0); lv_obj_set_style_border_color(body, ink(), 0); lv_obj_set_style_pad_all(body, 2, 0);
    battery = lv_obj_create(body);
    lv_obj_remove_style_all(battery); lv_obj_set_height(battery, LV_PCT(100)); lv_obj_set_style_radius(battery, 1, 0);
    lv_obj_set_style_bg_color(battery, ink(), 0); lv_obj_set_style_bg_opa(battery, LV_OPA_COVER, 0);
    lv_obj_t *tip = lv_obj_create(bat);
    lv_obj_remove_style_all(tip); lv_obj_set_size(tip, 3, 7); lv_obj_align(tip, LV_ALIGN_RIGHT_MID, 0, 0); lv_obj_set_style_radius(tip, 1, 0);
    lv_obj_set_style_bg_color(tip, ink(), 0); lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    clk = lv_label_create(st); lv_label_set_text(clk, ""); lv_obj_set_style_text_color(clk, ink(), 0);
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_width(list, LV_PCT(100)); lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_radius(list, 0, 0); lv_obj_set_style_border_width(list, 0, 0); lv_obj_set_style_pad_hor(list, 24, 0);
    hint_bar(scr, hints);
    status_text();
    return list;
}
lv_obj_t *kit_row(lv_obj_t *list, const void *icon, const char *text, const char *value, lv_event_cb_t cb, void *arg) {
    lv_obj_t *b = lv_list_add_button(list, icon, text);
    lv_obj_set_style_pad_ver(b, 16, 0);
    if (icon && lv_image_src_get_type(icon) == LV_IMAGE_SRC_SYMBOL) {   /* symbols differ in width: one column, texts in line */
        lv_obj_t *im = lv_obj_get_child(b, 0);
        lv_obj_set_width(im, 28); lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);
    }
    else if (icon) {                                                     /* an app's icon (a file): as big as the menu's */
        lv_obj_t *im = lv_obj_get_child(b, 0);
        lv_obj_set_size(im, 64, 64); lv_image_set_inner_align(im, LV_IMAGE_ALIGN_STRETCH);
    }
    if (value) { lv_obj_t *v = lv_label_create(b); lv_label_set_text(v, value); lv_obj_set_style_text_opa(v, LV_OPA_60, 0); lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0); }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg); else { lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_group_remove_obj(b); }
    return b;
}
void kit_note(lv_obj_t *list, const char *text) {
    lv_obj_t *l = lv_label_create(list);
    lv_label_set_text(l, text); lv_obj_set_width(l, LV_PCT(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &inter_16, 0); lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(l, 16, 0); lv_obj_set_style_pad_top(l, 8, 0); lv_obj_set_style_pad_bottom(l, 20, 0);
}
static void spin(void *o, int32_t v) { lv_arc_set_rotation(o, v); }
lv_obj_t *kit_busy(lv_obj_t *l, const char *title, const char *line, lv_obj_t **sub) {
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_t *a = lv_arc_create(l);                  /* a quarter of a ring turning at one even speed */
    lv_obj_set_size(a, 44, 44); lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE); lv_group_remove_obj(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(a, 5, 0); lv_obj_set_style_arc_width(a, 5, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(a, 0, 360); lv_arc_set_angles(a, 0, 90); lv_obj_set_style_margin_bottom(a, 10, 0);
    lv_anim_t an; lv_anim_init(&an);
    lv_anim_set_var(&an, a); lv_anim_set_exec_cb(&an, spin); lv_anim_set_values(&an, 0, 360);
    lv_anim_set_duration(&an, 900); lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE); lv_anim_set_path_cb(&an, lv_anim_path_linear);
    lv_anim_start(&an);
    lv_obj_t *t = lv_label_create(l); lv_obj_set_style_text_font(t, &inter_24, 0); lv_label_set_text(t, title);
    lv_obj_t *s = lv_label_create(l); lv_obj_set_style_text_opa(s, LV_OPA_60, 0); lv_label_set_text(s, line);
    if (sub) *sub = s;
    return t;
}
void kit_outcome(lv_obj_t *l, const char *title, const char *line) {
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_t *t = lv_label_create(l); lv_obj_set_style_text_font(t, &inter_24, 0); lv_label_set_text(t, title);
    lv_obj_t *s = lv_label_create(l); lv_obj_set_style_text_opa(s, LV_OPA_60, 0); lv_label_set_text(s, line);
}
void kit_empty(lv_obj_t *l, const char *title, const char *line) {   /* as the menu's home with no apps: an empty place, a few words */
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 24, 0);
    lv_obj_t *slot = lv_obj_create(l);
    lv_obj_remove_style_all(slot); lv_obj_set_size(slot, 96, 96); lv_obj_set_style_radius(slot, 22, 0);
    lv_obj_set_style_border_width(slot, 3, 0); lv_obj_set_style_border_color(slot, lv_palette_main(LV_PALETTE_GREY), 0); lv_obj_set_style_border_opa(slot, LV_OPA_50, 0);
    lv_obj_t *plus = lv_label_create(slot);
    lv_label_set_text(plus, LV_SYMBOL_PLUS); lv_obj_set_style_text_font(plus, &inter_32, 0); lv_obj_set_style_text_opa(plus, LV_OPA_50, 0); lv_obj_center(plus);
    lv_obj_t *t = lv_label_create(l); lv_label_set_text(t, title); lv_obj_set_style_text_font(t, &inter_24, 0);
    if (line && line[0]) { lv_obj_t *s = lv_label_create(l); lv_label_set_text(s, line); lv_obj_set_style_text_opa(s, LV_OPA_60, 0); lv_obj_set_style_margin_top(s, -14, 0); }
}
lv_obj_t *kit_bar(lv_obj_t *l) {
    lv_obj_t *b = lv_bar_create(l); lv_obj_set_size(b, 360, 8); lv_obj_set_style_margin_top(b, 14, 0);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    return b;
}

/* ---- screens: a stack, built again when they change */
static void (*on_a)(void);
void kit_on_a(void (*f)(void)) { on_a = f; }
static void build(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    on_a = 0;
    status = battery = clk = 0;
    lv_group_remove_all_objs(lv_group_get_default());
    quiet = 1; stack[depth](scr); quiet = 0;
    lv_screen_load(scr);
    uint32_t n = lv_group_get_obj_count(lv_group_get_default());
    if (n) {                                         /* shown selected, as after a button press (as the menu does) */
        int f = focus_at[depth] < (int)n ? focus_at[depth] : 0;
        lv_obj_t *o = lv_group_get_obj_by_index(lv_group_get_default(), (uint32_t)f);
        quiet = 1; lv_group_focus_obj(o); lv_obj_add_state(o, LV_STATE_FOCUS_KEY); quiet = 0;
    }
}
static int focused_index(void) {
    lv_group_t *g = lv_group_get_default(); lv_obj_t *f = lv_group_get_focused(g);
    for (uint32_t i = 0; i < lv_group_get_obj_count(g); i++) if (lv_group_get_obj_by_index(g, i) == f) return (int)i;
    return 0;
}
void kit_show(void) { lv_obj_t *prev = lv_screen_active(); build(); if (prev && prev != lv_screen_active()) lv_obj_delete(prev); }
void kit_push(kit_screen f) { if (depth < 7) { kit_click(); focus_at[depth] = focused_index(); stack[++depth] = f; focus_at[depth] = 0; kit_show(); } }
void kit_pop(void) { if (depth > 0) { kit_click(); depth--; kit_show(); } }
void kit_defer(void (*f)(void)) { deferred = f; }
void kit_hold(int on) { held_b = on; }
kit_screen kit_current(void) { return stack[depth]; }
static void on_focus(lv_group_t *g) { (void)g; if (!quiet) kit_click(); }

void kit_start(kit_screen first) {
    const wup_handoff_t *pb = wup_handoff();
    ui = pb ? pb->ui : 0;
#if defined(WUP_KIT_PREVIEW) || defined(CATALOG_PREVIEW)
    if (!wup_time_known()) wup_time_set(1794994860, 0);   /* (9:41, as the menu's MENU_PREVIEW) */
#endif
    make_click(click); wup_alert_sound = kit_note_sound;
    wup_lvgl_init();
    lv_display_t *d = lv_display_get_default();
    lv_display_set_theme(d, lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), kit_dark(), &inter_20));
    lv_group_set_focus_cb(lv_group_get_default(), on_focus);
    depth = 0; stack[0] = first;
    kit_show();
}
void kit_idle(void) {                          /* during long work: the spinner turns, bars move (about 60 times a second) */
    static uint32_t last;
    uint32_t now = (uint32_t)wup_us();
    if (now - last < 16000) return;
    last = now; lv_timer_handler();
}
int kit_step(void) {
    static unsigned held = ~0u; static int ticks;
    wup_lvgl_run(10);
    if (deferred) { void (*f)(void) = deferred; deferred = 0; f(); held = wup_buttons(); return 1; }
    unsigned b = wup_buttons(), pressed = b & ~held; held = b;
    if (wup_alert_shown()) {                       /* an alert (wup_alert): the buttons are its own */
        if (pressed & (WUP_A | WUP_B | WUP_LEFT | WUP_RIGHT)) kit_click();
        wup_alert_step(pressed);
        return 1;
    }
    if (pressed & WUP_A) { kit_click(); if (on_a) { void (*f)(void) = on_a; f(); return 1; } }
    if ((pressed & WUP_B) && !held_b) {
        if (!depth) { kit_click(); return 0; }
        kit_pop();
    }
    if (++ticks % 100 == 0) status_text();
    return 1;
}
