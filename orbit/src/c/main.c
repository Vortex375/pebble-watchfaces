/**
 * Orbit
 *
 * Two-ring watch face for Pebble Time 2 (emery, 200x228). The outer ring
 * carries a dot for every minute, the inner ring a dot for every hour of a
 * 24-hour day. Both turn clockwise: the dot at the top is the current minute /
 * hour, and the yellow 0-minute / 0-hour marks can be read like clock hands.
 * The hour ring is coloured like the sky: cream by day, slate by night,
 * periwinkle at dawn and apricot at dusk (sunrise and sunset from Open-Meteo
 * via PebbleKit JS).
 */

#include <pebble.h>

// ---------------------------------------------------------------------------
// Layout and palette
// ---------------------------------------------------------------------------

#define R_MIN      90   // minute dots
#define R_HOUR_OUT 76   // hour band
#define R_HOUR_IN  66
#define R_HOUR     71   // hour dots, middle of the band
#define R_STEPS    29   // steps progress ring in the centre
#define STEP_GOAL  10000
#define TWILIGHT   30   // minutes either side of sunrise / sunset

#define C_BG       GColorOxfordBlue
#define C_INFO     GColorCeleste
#define C_ACCENT   GColorYellow
#define C_DAY      GColorPastelYellow
#define C_NIGHT    GColorLiberty
#define C_DAWN     GColorBabyBlueEyes
#define C_DUSK     GColorRajah

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
static char s_hour_txt[3] = "";
static char s_date[12] = "";
static char s_temp[8] = "--°";
static char s_hr[8] = "--";
static char s_steps_txt[8] = "0";
static char s_battery_txt[8] = "";

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Minutes of a day (any sign) -> trig angle, a full turn per day
static int32_t day_angle(int minutes) {
  return (((minutes % 1440) + 1440) % 1440) * TRIG_MAX_ANGLE / 1440;
}

// Point at radius r around the centre, angle clockwise from 12 o'clock
static GPoint polar(int32_t angle, int r) {
  return GPoint(s_center.x + (r * sin_lookup(angle) + TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO,
                s_center.y - (r * cos_lookup(angle) + TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO);
}

static void fill_circle(GContext *ctx, GPoint c, int r, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, c, r);
}

static GRect radial_rect(int r) {
  return GRect(s_center.x - r, s_center.y - r, 2 * r + 1, 2 * r + 1);
}

// Fill the hour band from time of day t0 to t1 (minutes, t0 < t1). Time t
// sits (now - t) / 4 degrees clockwise from the top: the past to the right,
// the future to the left.
static void hour_arc(GContext *ctx, int t0, int t1, GColor color) {
  int32_t a0 = day_angle(s_now - t1);
  int32_t a1 = a0 + (t1 - t0) * TRIG_MAX_ANGLE / 1440;
  GRect r = radial_rect(R_HOUR_OUT);
  int inset = R_HOUR_OUT - R_HOUR_IN;
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, inset, a0,
                       a1 < TRIG_MAX_ANGLE ? a1 : TRIG_MAX_ANGLE);
  if (a1 > TRIG_MAX_ANGLE) {
    graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, inset, 0, a1 - TRIG_MAX_ANGLE);
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
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_circle(ctx, p, 4);
    } else if (k % 15 == 0) {
      fill_circle(ctx, p, 3, GColorWhite);
    } else if (k % 5 == 0) {
      fill_circle(ctx, p, 2, C_INFO);
    } else {
      fill_circle(ctx, p, 1, GColorLiberty);
    }
  }
}

static void draw_hour_ring(GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_radial(ctx, radial_rect(R_HOUR_OUT), GOvalScaleModeFitCircle,
                       R_HOUR_OUT - R_HOUR_IN, 0, TRIG_MAX_ANGLE);
  hour_arc(ctx, s_sunrise + TWILIGHT, s_sunset - TWILIGHT, C_DAY);
  hour_arc(ctx, s_sunrise - TWILIGHT, s_sunrise + TWILIGHT, C_DAWN);
  hour_arc(ctx, s_sunset - TWILIGHT, s_sunset + TWILIGHT, C_DUSK);
  for (int k = 0; k < 24; k++) {
    GPoint p = polar(day_angle(s_now - k * 60), R_HOUR);
    if (k == 0) {
      fill_circle(ctx, p, 6, C_BG);
      fill_circle(ctx, p, 4, C_ACCENT);
    } else {
      fill_circle(ctx, p, 1, C_BG);
    }
  }
}

// Pill across both rings at the top: minute above, hour below. GOTHIC 24
// Bold digits are 14 px tall, starting 10 px below the text box top.
static void draw_readout(GContext *ctx) {
  GRect pill = GRect(s_center.x - 14, s_center.y - R_MIN - 12, 28, 44);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, pill, 9, GCornersAll);
  graphics_context_set_stroke_color(ctx, C_INFO);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, pill, 9);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_min_txt, font, GRect(s_center.x - 20, s_center.y - R_MIN - 17, 40, 30),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, C_ACCENT);
  graphics_draw_text(ctx, s_hour_txt, font, GRect(s_center.x - 20, s_center.y - R_HOUR - 17, 40, 30),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Steps in the centre with a progress ring towards the daily goal
static void draw_steps(GContext *ctx) {
  GPoint c = GPoint(s_center.x, s_center.y + 6);
  GRect r = GRect(c.x - R_STEPS, c.y - R_STEPS, 2 * R_STEPS + 1, 2 * R_STEPS + 1);
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
  graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, 3, 0, TRIG_MAX_ANGLE);
  int steps = s_steps < STEP_GOAL ? s_steps : STEP_GOAL;
  if (steps > 0) {
    graphics_context_set_fill_color(ctx, GColorElectricBlue);
    graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, 3, 0,
                         (int64_t)TRIG_MAX_ANGLE * steps / STEP_GOAL);
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_steps_txt, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(c.x - 28, c.y - 21, 56, 30), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, C_INFO);
  graphics_draw_text(ctx, "steps", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(c.x - 28, c.y + 2, 56, 18), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
}

static void draw_heart(GContext *ctx, GPoint c) {
  graphics_context_set_fill_color(ctx, C_INFO);
  graphics_fill_circle(ctx, GPoint(c.x - 3, c.y - 2), 3);
  graphics_fill_circle(ctx, GPoint(c.x + 3, c.y - 2), 3);
  GPoint tri[3] = { { c.x - 6, c.y - 1 }, { c.x + 6, c.y - 1 }, { c.x, c.y + 6 } };
  GPathInfo info = { .num_points = 3, .points = tri };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Corners: date, temperature, heart rate, battery. GOTHIC 18 Bold ink is
// 11 px tall, starting 7 px below the text box top.
static void draw_corners(GContext *ctx, GRect bounds) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  int w = bounds.size.w, bottom = bounds.size.h - 4 - 18;  // box top for ink ending 4 px from the edge
  graphics_context_set_text_color(ctx, C_INFO);
  graphics_draw_text(ctx, s_date, font, GRect(4, -3, 70, 24), GTextOverflowModeFill,
                     GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, s_temp, font, GRect(w - 74, -3, 70, 24), GTextOverflowModeFill,
                     GTextAlignmentRight, NULL);
  draw_heart(ctx, GPoint(10, bottom + 12));
  graphics_draw_text(ctx, s_hr, font, GRect(20, bottom, 50, 24), GTextOverflowModeFill,
                     GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, s_battery_txt, font, GRect(w - 74, bottom, 70, 24),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_antialiased(ctx, true);
  draw_minute_ring(ctx);
  draw_hour_ring(ctx);
  draw_readout(ctx);
  draw_steps(ctx);
  draw_corners(ctx, bounds);
}

// ---------------------------------------------------------------------------
// Data: time, health, battery, weather
// ---------------------------------------------------------------------------

#define PERSIST_TEMP    1  // last temperature received from the phone
#define PERSIST_SUNRISE 2  // minutes after midnight
#define PERSIST_SUNSET  3

static void update_time(struct tm *t) {
  s_now = t->tm_hour * 60 + t->tm_min;
  snprintf(s_min_txt, sizeof(s_min_txt), "%02d", t->tm_min);
  snprintf(s_hour_txt, sizeof(s_hour_txt), "%02d", t->tm_hour);
  strftime(s_date, sizeof(s_date), "%a %e", t);
  for (char *c = s_date; *c; c++) {
    if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
  }
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

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *temp = dict_find(iter, MESSAGE_KEY_TEMPERATURE);
  if (temp) {
    set_temp(temp->value->int32);
    persist_write_int(PERSIST_TEMP, temp->value->int32);
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
}

static void main_window_unload(Window *window) {
  if (s_canvas_layer) { layer_destroy(s_canvas_layer); s_canvas_layer = NULL; }
}

static void init(void) {
  if (persist_exists(PERSIST_TEMP)) set_temp(persist_read_int(PERSIST_TEMP));
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
  app_message_open(64, 32);
  health_service_events_subscribe(health_handler, NULL);
  battery_state_service_subscribe(battery_handler);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
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
