/**
 * Tellura
 *
 * Two-ring watch face for Pebble Time 2 (emery, 200x228). The outer ring
 * carries a dot for every minute, the inner ring a dot for every hour (of the
 * whole day, or of the next 12 hours in 12-hour mode). Both turn clockwise: the dot
 * at the top is the current minute / hour, and the yellow 0-minute / 0-hour
 * marks can be read like clock hands. The hour ring is coloured like the sky:
 * cream by day, blue by night, purple at dawn and apricot at dusk
 * (sunrise and sunset from Open-Meteo via PebbleKit JS).
 *
 * The four corners and the centre each show a configurable metric: weather,
 * date, steps, heart rate or battery (settings via Clay, see src/pkjs/config.js).
 */

#include <pebble.h>

// ---------------------------------------------------------------------------
// Layout and palette
// ---------------------------------------------------------------------------

#define R_TICK     98   // inner end of the static 5-minute tick marks
#define R_MIN      91   // minute dots
#define R_HOUR_OUT 78   // hour band
#define R_HOUR_IN  62
#define R_HOUR     70   // hour dots, middle of the band
#define R_HTICK    59   // outer end of the static hour tick marks
#define HTICK_LEN  4
#define HTICK_LEN_QUARTER 7
#define HTICK_GROW 4    // extra length when the hour hand is right over a tick
#define R_PROGRESS 29   // steps / battery progress ring in the centre
#define STEP_GOAL  10000
#define TWILIGHT   30   // minutes either side of sunrise / sunset

#define C_BG       GColorOxfordBlue
#define C_INFO     GColorCeleste
#define C_ACCENT   GColorYellow
#define C_TICK     GColorCobaltBlue
#define C_DAY      GColorPastelYellow
#define C_NIGHT    GColorBlueMoon
#define C_DAWN     GColorLavenderIndigo
#define C_DUSK     GColorRajah

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

#define SETTINGS_KEY 10

enum { ITEM_NONE, ITEM_WEATHER, ITEM_DATE, ITEM_STEPS, ITEM_HEART, ITEM_BATTERY, ITEM_COUNT };
enum { CORNER_TL, CORNER_TR, CORNER_BL, CORNER_BR };

typedef struct {
  bool ring_12h;
  bool digital_12h;
  uint8_t center;
  uint8_t corner[4];
  bool minute_ticks;  // appended later: older saved settings keep the defaults
  bool hour_ticks;
} Settings;

static Settings s_settings = {
  .ring_12h = false,
  .digital_12h = false,
  .center = ITEM_STEPS,
  .corner = { ITEM_DATE, ITEM_WEATHER, ITEM_HEART, ITEM_BATTERY },
  .minute_ticks = true,
  .hour_ticks = true,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static Window *s_window;
static Layer *s_canvas_layer;
static GPoint s_center;

static int s_now;   // minutes after midnight
static int s_sunrise = 6 * 60;  // defaults until the phone sends the real times
static int s_sunset = 18 * 60;
static int s_steps;
static int s_battery;

static char s_min_txt[3] = "";
static char s_hour_txt[4] = "";
static char s_date[12] = "";       // "FRI 23"
static char s_mday[4] = "";
static char s_weekday[12] = "";    // "Friday"
static char s_month_year[20] = ""; // "October 2026"
static char s_temp[8] = "--°";
static char s_hr[12] = "--";
static char s_steps_txt[8] = "0";
static char s_battery_txt[8] = "";

// Weather icons (official Pebble timeline icons, see tools/weather_icons.py):
// 16 px for the corners, 32 px for the centre
static GBitmap *s_weather_bmp;
static GBitmap *s_weather_large;
static int s_weather_icon = -1;

// Sneaker icon for steps (relative to its top-left corner)
static GPoint s_shoe_points[9] = {
  { 1, 0 }, { 6, 0 }, { 7, 3 }, { 11, 4 }, { 15, 6 }, { 17, 9 }, { 17, 12 },
  { 0, 12 }, { 0, 1 }
};
static GPathInfo s_shoe_info = { .num_points = 9, .points = s_shoe_points };
static GPath *s_shoe;

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

// Minutes covered by one turn of the hour ring
static int ring_span(void) {
  return s_settings.ring_12h ? 12 * 60 : 24 * 60;
}

// Minutes (any sign) -> trig angle on the hour ring
static int32_t ring_angle(int minutes) {
  int span = ring_span();
  return (((minutes % span) + span) % span) * TRIG_MAX_ANGLE / span;
}

// v / TRIG_MAX_RATIO rounded to the nearest integer, also for negative v
// (C division truncates towards zero)
static int trig_round(int32_t v) {
  return v >= 0 ? (v + TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO
                : -((-v + TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO);
}

// Point at radius r around the centre, angle clockwise from 12 o'clock
static GPoint polar(int32_t angle, int r) {
  return GPoint(s_center.x + trig_round(r * sin_lookup(angle)),
                s_center.y - trig_round(r * cos_lookup(angle)));
}

static void fill_circle(GContext *ctx, GPoint c, int r, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, c, r);
}

static GRect square_around(GPoint c, int r) {
  return GRect(c.x - r, c.y - r, 2 * r + 1, 2 * r + 1);
}

// Ring sector of the given thickness inside radius r, from angle start to
// end (start may be negative, down to minus a full turn); wraps past 12 o'clock
static void fill_sector(GContext *ctx, int r, int thickness, int32_t start, int32_t end) {
  if (start < 0) { start += TRIG_MAX_ANGLE; end += TRIG_MAX_ANGLE; }
  GRect rect = square_around(s_center, r);
  graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, thickness, start,
                       end < TRIG_MAX_ANGLE ? end : TRIG_MAX_ANGLE);
  if (end > TRIG_MAX_ANGLE) {
    graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, thickness, 0, end - TRIG_MAX_ANGLE);
  }
}

static void draw_text(GContext *ctx, const char *text, const char *font, GRect box,
                      GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, fonts_get_system_font(font), box, GTextOverflowModeFill,
                     align, NULL);
}

// ---------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------

// Static tick marks every 5 minutes around the outside, cut off by the screen
// edge at 3 and 9 o'clock. When the 0 or a 15-minute dot lines up with a tick
// (on minutes divisible by 5), the tick takes on that dot's colour.
//
// Rounding both ends of such a short line separately would tilt it by up to
// 4 degrees, so each tick runs from its rounded inner end along a whole-pixel
// step (right, up) that points within 1 degree of straight outwards. These
// are the steps for 0, 30 and 60 degrees, about 6 px long; the other quadrants
// are the same steps rotated.
static const int8_t s_tick_step[3][2] = { { 0, 6 }, { 3, 5 }, { 5, 3 } };

static void draw_ticks(GContext *ctx) {
  int m = s_now % 60;
  graphics_context_set_stroke_width(ctx, 3);
  for (int i = 0; i < 12; i++) {
    int p = i * 5;              // minute position, 0 at 12 o'clock
    int k = (m - p + 60) % 60;  // minute dot at this position
    graphics_context_set_stroke_color(ctx, k == 0 ? C_ACCENT : k % 15 == 0 ? GColorWhite : C_TICK);
    int dx = s_tick_step[i % 3][0], dy = s_tick_step[i % 3][1];
    for (int q = 0; q < i / 3; q++) {  // rotate a quarter turn clockwise
      int t = dx;
      dx = dy;
      dy = -t;
    }
    GPoint in = polar(DEG_TO_TRIGANGLE(p * 6), R_TICK);
    graphics_draw_line(ctx, in, GPoint(in.x + dx, in.y - dy));
  }
}

// Static tick marks inside the hour ring, one per hour position (24 or 12).
// Each is a thin annular sector 3 degrees wide, so its edges run exactly
// towards the centre without any rounding tilt. The quarter positions (12, 3,
// 6 and 9 o'clock) are longer, 4 degrees wide and white like the 15-minute
// dots.
//
// The tick nearest the hour hand (the yellow 0-hour dot) reacts to it: within
// 30 minutes it grows longer and wider, up to HTICK_GROW px and 2 degrees
// more when the hand is right over it, and within 15 minutes it turns yellow.
// That also keeps the hand visible at 12 o'clock, where the readout hides it.
static void draw_hour_ticks(GContext *ctx) {
  int span = ring_span(), hours = span / 60, hand = s_now % span;
  for (int i = 0; i < hours; i++) {
    int32_t a = TRIG_MAX_ANGLE * i / hours;
    bool quarter = i % (hours / 4) == 0;
    int d = ((hand - i * 60) % span + span + span / 2) % span - span / 2;
    if (d < 0) d = -d;              // minutes between the hand and this tick
    int near = d < 30 ? 30 - d : 0;  // 0 (30 min or more away) .. 30 (on it)
    int len = (quarter ? HTICK_LEN_QUARTER : HTICK_LEN) + HTICK_GROW * near / 30;
    int32_t half = (quarter ? DEG_TO_TRIGANGLE(2) : DEG_TO_TRIGANGLE(3) / 2) +
                   DEG_TO_TRIGANGLE(1) * near / 30;
    graphics_context_set_fill_color(ctx, d <= 15 ? C_ACCENT : quarter ? GColorWhite : C_TICK);
    fill_sector(ctx, R_HTICK, len, a - half, a + half);
  }
}

static void draw_minute_ring(GContext *ctx) {
  int m = s_now % 60;
  for (int k = 0; k < 60; k++) {
    // Dots run counter-clockwise so the ring turning clockwise brings the
    // current minute to the top
    GPoint p = polar(DEG_TO_TRIGANGLE(((m - k) * 6 + 360) % 360), R_MIN);
    if (k == 0) {
      graphics_context_set_stroke_color(ctx, C_ACCENT);
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_circle(ctx, p, 5);
    } else if (k % 15 == 0) {
      fill_circle(ctx, p, 4, GColorWhite);
    } else if (k % 5 == 0) {
      fill_circle(ctx, p, 3, GColorPictonBlue);
    } else {
      fill_circle(ctx, p, 1, GColorLiberty);
    }
  }
}

// Fill the hour band from time of day t0 to t1 (minutes, t0 < t1). The ring
// shows the next turn's worth of time: time t sits (t - now) / span of a turn
// counter-clockwise from the top, so every hour position is coloured for the
// next time that hour comes round. For the 12-hour ring that is the next 12
// hours, with the join at the top under the readout; the 24-hour ring shows
// the whole day either way.
static void hour_arc(GContext *ctx, int t0, int t1, GColor color) {
  int span = ring_span();
  graphics_context_set_fill_color(ctx, color);
  for (int shift = -24 * 60; shift <= 24 * 60; shift += 24 * 60) {
    int a = t0 + shift - s_now, b = t1 + shift - s_now;  // relative to now
    if (a < 0) a = 0;
    if (b > span) b = span;
    if (a >= b) continue;
    fill_sector(ctx, R_HOUR_OUT, R_HOUR_OUT - R_HOUR_IN, -b * TRIG_MAX_ANGLE / span,
                -a * TRIG_MAX_ANGLE / span);
  }
}

static void draw_hour_ring(GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_radial(ctx, square_around(s_center, R_HOUR_OUT), GOvalScaleModeFitCircle,
                       R_HOUR_OUT - R_HOUR_IN, 0, TRIG_MAX_ANGLE);
  hour_arc(ctx, s_sunrise + TWILIGHT, s_sunset - TWILIGHT, C_DAY);
  hour_arc(ctx, s_sunrise - TWILIGHT, s_sunrise + TWILIGHT, C_DAWN);
  hour_arc(ctx, s_sunset - TWILIGHT, s_sunset + TWILIGHT, C_DUSK);
  int hours = ring_span() / 60;
  for (int k = 0; k < hours; k++) {
    GPoint p = polar(ring_angle(s_now - k * 60), R_HOUR);
    if (k == 0) {
      fill_circle(ctx, p, 6, C_BG);
      fill_circle(ctx, p, 5, C_ACCENT);
    } else {
      fill_circle(ctx, p, 2, C_BG);
    }
  }
}

// Pill across both rings at the top: minute above, hour below. GOTHIC 24
// Bold digits are 14 px tall, starting 10 px below the text box top. The pill
// reaches 7-8 px past the digits, beyond the inner edge of the hour band.
static void draw_readout(GContext *ctx) {
  GRect pill = GRect(s_center.x - 14, s_center.y - R_MIN - 14, 28, 50);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, pill, 9, GCornersAll);
  graphics_context_set_stroke_color(ctx, C_INFO);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, pill, 9);
  draw_text(ctx, s_min_txt, FONT_KEY_GOTHIC_24_BOLD,
            GRect(s_center.x - 20, s_center.y - R_MIN - 17, 40, 30), GTextAlignmentCenter,
            GColorWhite);
  draw_text(ctx, s_hour_txt, FONT_KEY_GOTHIC_24_BOLD,
            GRect(s_center.x - 20, s_center.y - R_HOUR - 17, 40, 30), GTextAlignmentCenter,
            C_ACCENT);
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

static void draw_bitmap(GContext *ctx, GBitmap *bmp, GPoint c) {
  if (!bmp) return;
  GSize s = gbitmap_get_bounds(bmp).size;
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, bmp, GRect(c.x - s.w / 2, c.y - s.h / 2, s.w, s.h));
}

// Heart centred on c, scale 1 is 13x12 px
static void draw_heart(GContext *ctx, GPoint c, int s, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, GPoint(c.x - 3 * s, c.y - 2 * s), 3 * s);
  graphics_fill_circle(ctx, GPoint(c.x + 3 * s, c.y - 2 * s), 3 * s);
  GPoint tri[3] = { { c.x - 6 * s, c.y - s }, { c.x + 6 * s, c.y - s }, { c.x, c.y + 6 * s } };
  GPathInfo info = { .num_points = 3, .points = tri };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

static void draw_shoe(GContext *ctx, GPoint c) {
  GPoint o = GPoint(c.x - 9, c.y - 6);
  graphics_context_set_fill_color(ctx, C_INFO);
  gpath_move_to(s_shoe, o);
  gpath_draw_filled(ctx, s_shoe);
  // Sole stripe and laces cut out in the background colour
  graphics_context_set_stroke_color(ctx, C_BG);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(o.x + 1, o.y + 9), GPoint(o.x + 16, o.y + 9));
  graphics_draw_line(ctx, GPoint(o.x + 8, o.y + 4), GPoint(o.x + 9, o.y + 6));
  graphics_draw_line(ctx, GPoint(o.x + 11, o.y + 5), GPoint(o.x + 12, o.y + 7));
}

// Battery 16x8 px centred on c, filled to the charge level
static void draw_battery(GContext *ctx, GPoint c) {
  GRect body = GRect(c.x - 8, c.y - 4, 14, 9);
  graphics_context_set_stroke_color(ctx, C_INFO);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, body);
  graphics_context_set_fill_color(ctx, C_INFO);
  graphics_fill_rect(ctx, GRect(c.x + 6, c.y - 2, 2, 5), 0, GCornerNone);
  int w = (s_battery * 10 + 50) / 100;
  if (w > 0) graphics_fill_rect(ctx, GRect(body.origin.x + 2, c.y - 2, w, 5), 0, GCornerNone);
}

// Calendar page: a solid header band over an outlined body. With a day it is
// 24x21 px and the day (GOTHIC 18 Bold, ink 11 px tall, 7 px below the box
// top) sits in the body; without one it is a 14x13 px icon.
static void draw_calendar(GContext *ctx, GPoint c, const char *day) {
  int w = day ? 24 : 14, h = day ? 21 : 13, band = day ? 5 : 4;
  GRect r = GRect(c.x - w / 2, c.y - h / 2, w, h);
  graphics_context_set_fill_color(ctx, C_INFO);
  graphics_fill_rect(ctx, r, 2, GCornersAll);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, GRect(r.origin.x + 2, r.origin.y + band, w - 4, h - band - 2), 0,
                     GCornerNone);
  if (day) {
    draw_text(ctx, day, FONT_KEY_GOTHIC_18_BOLD, GRect(r.origin.x - 4, r.origin.y, w + 8, 21),
              GTextAlignmentCenter, C_INFO);
  }
}

// ---------------------------------------------------------------------------
// Metrics: corners and centre
// ---------------------------------------------------------------------------

// Corner: 16 px icon hugging the outer screen edge, value next to it. Both
// are centred on the same line, 10 px from the top / bottom edge. GOTHIC 18
// Bold ink is 11 px tall, starting 7 px below the text box top.
static void draw_corner(GContext *ctx, int item, GRect bounds, bool right, bool bottom) {
  const char *value;
  switch (item) {
    case ITEM_WEATHER: value = s_temp; break;
    case ITEM_DATE:    value = s_date; break;
    case ITEM_STEPS:   value = s_steps_txt; break;
    case ITEM_HEART:   value = s_hr; break;
    case ITEM_BATTERY: value = s_battery_txt; break;
    default: return;
  }
  int w = bounds.size.w, y = bottom ? bounds.size.h - 10 : 10;
  GPoint icon = GPoint(right ? w - 12 : 12, y);
  switch (item) {
    case ITEM_WEATHER: draw_bitmap(ctx, s_weather_bmp, icon); break;
    case ITEM_DATE:    draw_calendar(ctx, icon, NULL); break;
    case ITEM_STEPS:   draw_shoe(ctx, icon); break;
    case ITEM_HEART:   draw_heart(ctx, icon, 1, C_INFO); break;
    case ITEM_BATTERY: draw_battery(ctx, icon); break;
  }
  GRect box = GRect(right ? w - 24 - 70 : 24, y - 13, 70, 24);
  draw_text(ctx, value, FONT_KEY_GOTHIC_18_BOLD, box,
            right ? GTextAlignmentRight : GTextAlignmentLeft, C_INFO);
}

// Progress ring with a number and a label in it (steps, battery)
static void draw_progress(GContext *ctx, GPoint c, int value, int max, const char *text,
                          const char *label) {
  GRect r = square_around(c, R_PROGRESS);
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
  graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, 3, 0, TRIG_MAX_ANGLE);
  if (value > max) value = max;
  if (value > 0) {
    graphics_context_set_fill_color(ctx, GColorElectricBlue);
    graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, 3, 0,
                         (int64_t)TRIG_MAX_ANGLE * value / max);
  }
  draw_text(ctx, text, FONT_KEY_GOTHIC_24_BOLD, GRect(c.x - 28, c.y - 21, 56, 30),
            GTextAlignmentCenter, GColorWhite);
  draw_text(ctx, label, FONT_KEY_GOTHIC_14, GRect(c.x - 28, c.y + 2, 56, 18),
            GTextAlignmentCenter, C_INFO);
}

// Centre metric, drawn around c (a little below the screen centre, which
// balances the readout at the top). The space inside the hour ring is a
// circle of radius 62 around the screen centre.
static void draw_center(GContext *ctx) {
  GPoint c = GPoint(s_center.x, s_center.y + 6);
  switch (s_settings.center) {
    case ITEM_STEPS:
      draw_progress(ctx, c, s_steps, STEP_GOAL, s_steps_txt, "steps");
      break;
    case ITEM_BATTERY:
      draw_progress(ctx, c, s_battery, 100, s_battery_txt, "battery");
      break;
    case ITEM_WEATHER:
      // 32 px icon above the temperature (GOTHIC 28 Bold: ink 18 px tall,
      // 10 px below the box top)
      draw_bitmap(ctx, s_weather_large, GPoint(c.x, c.y - 16));
      draw_text(ctx, s_temp, FONT_KEY_GOTHIC_28_BOLD, GRect(c.x - 40, c.y - 7, 80, 34),
                GTextAlignmentCenter, GColorWhite);
      break;
    case ITEM_DATE:
      draw_calendar(ctx, GPoint(c.x, c.y - 21), s_mday);
      draw_text(ctx, s_weekday, FONT_KEY_GOTHIC_18_BOLD, GRect(c.x - 50, c.y - 11, 100, 22),
                GTextAlignmentCenter, GColorWhite);
      draw_text(ctx, s_month_year, FONT_KEY_GOTHIC_14, GRect(c.x - 50, c.y + 9, 100, 18),
                GTextAlignmentCenter, C_INFO);
      break;
    case ITEM_HEART:
      draw_heart(ctx, GPoint(c.x, c.y - 20), 2, GColorSunsetOrange);
      draw_text(ctx, s_hr, FONT_KEY_GOTHIC_28_BOLD, GRect(c.x - 40, c.y - 11, 80, 34),
                GTextAlignmentCenter, GColorWhite);
      draw_text(ctx, "bpm", FONT_KEY_GOTHIC_14, GRect(c.x - 40, c.y + 15, 80, 18),
                GTextAlignmentCenter, C_INFO);
      break;
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_antialiased(ctx, true);
  if (s_settings.minute_ticks) draw_ticks(ctx);
  draw_minute_ring(ctx);
  draw_hour_ring(ctx);
  if (s_settings.hour_ticks) draw_hour_ticks(ctx);
  draw_readout(ctx);
  draw_center(ctx);
  draw_corner(ctx, s_settings.corner[CORNER_TL], bounds, false, false);
  draw_corner(ctx, s_settings.corner[CORNER_TR], bounds, true, false);
  draw_corner(ctx, s_settings.corner[CORNER_BL], bounds, false, true);
  draw_corner(ctx, s_settings.corner[CORNER_BR], bounds, true, true);
}

// ---------------------------------------------------------------------------
// Data: time, health, battery, weather
// ---------------------------------------------------------------------------

#define PERSIST_TEMP    1  // last temperature received from the phone
#define PERSIST_SUNRISE 2  // minutes after midnight
#define PERSIST_SUNSET  3
#define PERSIST_WEATHER 4  // WMO weather code

static void update_time(struct tm *t) {
  s_now = t->tm_hour * 60 + t->tm_min;
  snprintf(s_min_txt, sizeof(s_min_txt), "%02d", t->tm_min);
  if (s_settings.digital_12h) {
    snprintf(s_hour_txt, sizeof(s_hour_txt), "%d", (t->tm_hour + 11) % 12 + 1);
  } else {
    snprintf(s_hour_txt, sizeof(s_hour_txt), "%02d", t->tm_hour);
  }
  char wday[4];
  strftime(wday, sizeof(wday), "%a", t);
  for (char *c = wday; *c; c++) {
    if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
  }
  snprintf(s_mday, sizeof(s_mday), "%d", t->tm_mday);
  snprintf(s_date, sizeof(s_date), "%s %s", wday, s_mday);
  strftime(s_weekday, sizeof(s_weekday), "%A", t);
  strftime(s_month_year, sizeof(s_month_year), "%B %Y", t);
}

static bool health_available(HealthMetric metric, time_t start, time_t end) {
  return health_service_metric_accessible(metric, start, end) &
         HealthServiceAccessibilityMaskAvailable;
}

static void update_health(void) {
  time_t now = time(NULL);
  if (health_available(HealthMetricStepCount, time_start_of_today(), now)) {
    s_steps = (int)health_service_sum_today(HealthMetricStepCount);
    snprintf(s_steps_txt, sizeof(s_steps_txt), "%d", s_steps);
  }
  HealthValue bpm = 0;
  if (health_available(HealthMetricHeartRateBPM, now, now)) {
    bpm = health_service_peek_current_value(HealthMetricHeartRateBPM);
  }
  if (bpm > 0) {
    snprintf(s_hr, sizeof(s_hr), "%d", (int)bpm);
  } else {
    snprintf(s_hr, sizeof(s_hr), "--");
  }
}

static void battery_handler(BatteryChargeState state) {
  s_battery = state.charge_percent;
  snprintf(s_battery_txt, sizeof(s_battery_txt), "%d%%", s_battery);
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

// WMO weather code (as reported by Open-Meteo) -> index into the icon tables
enum { W_SUN, W_PARTLY, W_CLOUDY, W_LIGHT_RAIN, W_HEAVY_RAIN, W_LIGHT_SNOW, W_HEAVY_SNOW,
       W_RAIN_SNOW, W_GENERIC };

static const uint32_t s_icons_small[] = {
  RESOURCE_ID_WEATHER_SUN, RESOURCE_ID_WEATHER_PARTLY_CLOUDY, RESOURCE_ID_WEATHER_CLOUDY,
  RESOURCE_ID_WEATHER_LIGHT_RAIN, RESOURCE_ID_WEATHER_HEAVY_RAIN, RESOURCE_ID_WEATHER_LIGHT_SNOW,
  RESOURCE_ID_WEATHER_HEAVY_SNOW, RESOURCE_ID_WEATHER_RAIN_SNOW, RESOURCE_ID_WEATHER_GENERIC
};
static const uint32_t s_icons_large[] = {
  RESOURCE_ID_WEATHER_SUN_LARGE, RESOURCE_ID_WEATHER_PARTLY_CLOUDY_LARGE,
  RESOURCE_ID_WEATHER_CLOUDY_LARGE, RESOURCE_ID_WEATHER_LIGHT_RAIN_LARGE,
  RESOURCE_ID_WEATHER_HEAVY_RAIN_LARGE, RESOURCE_ID_WEATHER_LIGHT_SNOW_LARGE,
  RESOURCE_ID_WEATHER_HEAVY_SNOW_LARGE, RESOURCE_ID_WEATHER_RAIN_SNOW_LARGE,
  RESOURCE_ID_WEATHER_GENERIC_LARGE
};

static int weather_icon(int code) {
  switch (code) {
    case 0: case 1:                     return W_SUN;
    case 2:                             return W_PARTLY;
    case 3: case 45: case 48:           return W_CLOUDY;      // + fog
    case 51: case 53: case 55: case 56: case 57:              // drizzle
    case 61: case 80:                   return W_LIGHT_RAIN;
    case 63: case 65: case 81: case 82:
    case 95: case 96: case 99:          return W_HEAVY_RAIN;  // + storm
    case 66: case 67:                   return W_RAIN_SNOW;   // freezing rain
    case 71: case 77: case 85:          return W_LIGHT_SNOW;
    case 73: case 75: case 86:          return W_HEAVY_SNOW;
    default:                            return W_GENERIC;
  }
}

// The icons are white; the corner ones are recoloured to match the text
static GBitmap *load_icon(uint32_t res, GColor color) {
  GBitmap *bmp = gbitmap_create_with_resource(res);
  GColor *pal = gbitmap_get_palette(bmp);
  int n = gbitmap_get_format(bmp) == GBitmapFormat1BitPalette ? 2 : 4;
  for (int k = 0; pal && k < n; k++) {
    if (pal[k].a) pal[k] = (GColor){ .argb = (color.argb & 0x3f) | (pal[k].argb & 0xc0) };
  }
  return bmp;
}

static void set_weather_code(int code) {
  int icon = weather_icon(code);
  if (icon == s_weather_icon) return;
  if (s_weather_bmp) gbitmap_destroy(s_weather_bmp);
  if (s_weather_large) gbitmap_destroy(s_weather_large);
  s_weather_bmp = load_icon(s_icons_small[icon], C_INFO);
  s_weather_large = load_icon(s_icons_large[icon], GColorWhite);
  s_weather_icon = icon;
}

static void set_temp(int32_t temp) {
  snprintf(s_temp, sizeof(s_temp), "%d°", (int)temp);
}

static void request_weather(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
    app_message_outbox_send();
  }
}

// Clay sends selects as strings and toggles as integers
static int tuple_int(Tuple *t) {
  return t->type == TUPLE_CSTRING ? atoi(t->value->cstring) : (int)t->value->int32;
}

static bool read_settings(DictionaryIterator *iter) {
  static const uint32_t *corner_keys[4] = {
    &MESSAGE_KEY_CornerTL, &MESSAGE_KEY_CornerTR, &MESSAGE_KEY_CornerBL, &MESSAGE_KEY_CornerBR
  };
  bool changed = false;
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_Ring12h))) {
    s_settings.ring_12h = tuple_int(t) != 0;
    changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_MinuteTicks))) {
    s_settings.minute_ticks = tuple_int(t) != 0;
    changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_HourTicks))) {
    s_settings.hour_ticks = tuple_int(t) != 0;
    changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_Digital12h))) {
    s_settings.digital_12h = tuple_int(t) != 0;
    changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_Center))) {
    int v = tuple_int(t);
    if (v >= ITEM_NONE && v < ITEM_COUNT) s_settings.center = v;
    changed = true;
  }
  for (int i = 0; i < 4; i++) {
    if ((t = dict_find(iter, *corner_keys[i]))) {
      int v = tuple_int(t);
      if (v >= ITEM_NONE && v < ITEM_COUNT) s_settings.corner[i] = v;
      changed = true;
    }
  }
  return changed;
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  if (read_settings(iter)) {
    persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
    time_t now = time(NULL);
    update_time(localtime(&now));
  }
  Tuple *temp = dict_find(iter, MESSAGE_KEY_TEMPERATURE);
  if (temp) {
    set_temp(temp->value->int32);
    persist_write_int(PERSIST_TEMP, temp->value->int32);
  }
  Tuple *code = dict_find(iter, MESSAGE_KEY_WEATHER_CODE);
  if (code) {
    set_weather_code(code->value->int32);
    persist_write_int(PERSIST_WEATHER, code->value->int32);
  }
  Tuple *rise = dict_find(iter, MESSAGE_KEY_SUNRISE);
  Tuple *set = dict_find(iter, MESSAGE_KEY_SUNSET);
  if (rise && set) {
    s_sunrise = rise->value->int32;
    s_sunset = set->value->int32;
    persist_write_int(PERSIST_SUNRISE, s_sunrise);
    persist_write_int(PERSIST_SUNSET, s_sunset);
  }
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

static void health_handler(HealthEventType event, void *context) {
  update_health();
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
  update_health();
  if (tick_time->tm_min % 30 == 0) request_weather();
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  s_center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);
  s_shoe = gpath_create(&s_shoe_info);
}

static void main_window_unload(Window *window) {
  if (s_shoe) { gpath_destroy(s_shoe); s_shoe = NULL; }
  if (s_canvas_layer) { layer_destroy(s_canvas_layer); s_canvas_layer = NULL; }
}

static void init(void) {
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  if (persist_exists(PERSIST_TEMP)) set_temp(persist_read_int(PERSIST_TEMP));
  set_weather_code(persist_exists(PERSIST_WEATHER) ? persist_read_int(PERSIST_WEATHER) : -1);
  if (persist_exists(PERSIST_SUNSET)) {
    s_sunrise = persist_read_int(PERSIST_SUNRISE);
    s_sunset = persist_read_int(PERSIST_SUNSET);
  }
  time_t now = time(NULL);
  update_time(localtime(&now));
  update_health();
  battery_handler(battery_state_service_peek());

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_window, true);

  // The phone sends the weather on startup; the watch asks again every 30 min
  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);  // settings arrive as one message
  health_service_events_subscribe(health_handler, NULL);
  battery_state_service_subscribe(battery_handler);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  if (s_weather_bmp) gbitmap_destroy(s_weather_bmp);
  if (s_weather_large) gbitmap_destroy(s_weather_large);
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  health_service_events_unsubscribe();
  app_message_deregister_callbacks();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
