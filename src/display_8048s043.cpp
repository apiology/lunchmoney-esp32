#ifdef BOARD_8048S043

#include "display.h"

#include <stdarg.h>

#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <Wire.h>
#include <esp_heap_caps.h>

extern "C" {
#include "draw/sw/lv_draw_sw.h"
}

extern int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);

#define TFT_BL 2

#ifndef TOUCH_DEBUG
#define TOUCH_DEBUG 1
#endif

// GT911 library applies ROTATION_NORMAL (800-x, 480-y). On this board the
// transformed coordinates are already 0..799 x 0..479 — do not use the
// vendor 480x272 map (it extrapolates to negative values for typical touches).

static Arduino_ESP32RGBPanel* rgb_bus = nullptr;
static Arduino_RPi_DPI_RGBPanel* gfx = nullptr;
static TAMC_GT911* touch = nullptr;
static lv_display_t* lv_disp = nullptr;
static uint8_t* draw_buf = nullptr;
static uint8_t* rotate_buf = nullptr;
static uint32_t flush_count = 0;

static constexpr int32_t kPhysHorRes = 800;
static constexpr int32_t kPhysVerRes = 480;
static constexpr uint32_t kDrawBufLines = 80;

static lv_obj_t* scroll_target = nullptr;
static int touch_last_x = 0;
static int touch_last_y = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;
static int32_t drag_last_y = 0;
static int32_t scroll_velocity = 0;
static int32_t last_drag_dy = 0;
static int32_t pending_scroll_dy = 0;
static bool drag_active = false;
static bool touch_debug = (TOUCH_DEBUG != 0);
static uint32_t last_inertia_ms = 0;

static constexpr int32_t kMaxScrollVelocity = 140;
static constexpr int32_t kScrollFrictionPct = 98;
static constexpr uint32_t kInertiaStepMs = 16;
static constexpr int32_t kReleaseBoostPct = 200;
static constexpr int32_t kTapMoveThreshold = 24;

static constexpr int32_t kTapMaxMovePx = 24;
static constexpr uint32_t kTapMaxDurationMs = 350;
static constexpr uint32_t kDoubleTapGapMs = 450;
static constexpr int32_t kDoubleTapMaxDistPx = 48;
// Top/bottom bands page the list; the middle band is for double-tap refresh only.
static constexpr int32_t kPageZonePct = 40;

static bool tap_tracking = false;
static int32_t tap_down_x = 0;
static int32_t tap_down_y = 0;
static uint32_t tap_down_ms = 0;
static int32_t tap_max_move = 0;
static int32_t last_tap_x = 0;
static int32_t last_tap_y = 0;
static uint32_t last_tap_ms = 0;
static bool double_tap_ready = false;

static void touchDebug(const char* msg) {
  if (touch_debug) {
    Serial.println(msg);
  }
}

static void dispLogf(const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
}

static void touchDebugf(const char* fmt, ...) {
  if (!touch_debug) {
    return;
  }
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
}

static void registerTap(int x, int y, uint32_t now_ms) {
  if (last_tap_ms != 0 && (now_ms - last_tap_ms) <= kDoubleTapGapMs) {
    const int32_t dx = x - last_tap_x;
    const int32_t dy = y - last_tap_y;
    if ((dx < 0 ? -dx : dx) <= kDoubleTapMaxDistPx &&
        (dy < 0 ? -dy : dy) <= kDoubleTapMaxDistPx) {
      double_tap_ready = true;
      last_tap_ms = 0;
      touchDebug("touch: double-tap detected");
      return;
    }
  }
  last_tap_x = x;
  last_tap_y = y;
  last_tap_ms = now_ms;
}

static void mapTouchPoint(int& x, int& y) {
  int32_t px = touch->points[0].x;
  int32_t py = touch->points[0].y;

  if (px < 0) {
    px = 0;
  } else if (px > kPhysHorRes - 1) {
    px = kPhysHorRes - 1;
  }
  if (py < 0) {
    py = 0;
  } else if (py > kPhysVerRes - 1) {
    py = kPhysVerRes - 1;
  }

  const lv_display_rotation_t rot =
      lv_disp ? lv_display_get_rotation(lv_disp) : LV_DISPLAY_ROTATION_0;
  if (rot == LV_DISPLAY_ROTATION_180 || rot == LV_DISPLAY_ROTATION_270) {
    px = kPhysHorRes - px - 1;
    py = kPhysVerRes - py - 1;
  }
  if (rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270) {
    const int32_t tmp = py;
    py = px;
    px = kPhysVerRes - tmp - 1;
  }

  x = static_cast<int>(px);
  y = static_cast<int>(py);

  const int max_x = static_cast<int>(lv_display_get_horizontal_resolution(lv_disp)) - 1;
  const int max_y = static_cast<int>(lv_display_get_vertical_resolution(lv_disp)) - 1;
  if (x < 0) {
    x = 0;
  } else if (x > max_x) {
    x = max_x;
  }
  if (y < 0) {
    y = 0;
  } else if (y > max_y) {
    y = max_y;
  }
}

static uint8_t probeGt911Addr() {
  Wire.beginTransmission(GT911_ADDR1);
  if (Wire.endTransmission() == 0) {
    return GT911_ADDR1;
  }
  Wire.beginTransmission(GT911_ADDR2);
  if (Wire.endTransmission() == 0) {
    return GT911_ADDR2;
  }
  return 0;
}

static void flushDisplay(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  const lv_color_format_t cf = lv_display_get_color_format(disp);
  const uint32_t px_size = lv_color_format_get_size(cf);
  int32_t w = lv_area_get_width(area);
  int32_t h = lv_area_get_height(area);
  const uint8_t* src = px_map;
  lv_area_t phys_area = *area;

  const lv_display_rotation_t rotation = lv_display_get_rotation(disp);
  if (rotation != LV_DISPLAY_ROTATION_0) {
    const uint32_t w_stride = lv_draw_buf_width_to_stride(w, cf);
    const uint32_t h_stride = lv_draw_buf_width_to_stride(h, cf);
    if (rotation == LV_DISPLAY_ROTATION_180) {
      lv_draw_sw_rotate(px_map, rotate_buf, w, h, w_stride, w_stride, rotation, cf);
    } else {
      lv_draw_sw_rotate(px_map, rotate_buf, w, h, w_stride, h_stride, rotation, cf);
    }
    src = rotate_buf;
    lv_display_rotate_area(disp, &phys_area);
    w = lv_area_get_width(&phys_area);
    h = lv_area_get_height(&phys_area);
  }

  if (flush_count < 5) {
    dispLogf("flush #%u logical (%d,%d)-(%d,%d) phys (%d,%d)-(%d,%d) %dx%d rot=%d",
             flush_count, area->x1, area->y1, area->x2, area->y2, phys_area.x1, phys_area.y1,
             phys_area.x2, phys_area.y2, w, h, static_cast<int>(rotation));
    flush_count++;
  }

  uint16_t* fb = gfx->getFramebuffer();
  for (int32_t y = phys_area.y1; y <= phys_area.y2; y++) {
    uint16_t* dst = fb + static_cast<int32_t>(y) * kPhysHorRes + phys_area.x1;
    lv_memcpy(dst, src, static_cast<size_t>(w) * px_size);
    src += static_cast<size_t>(w) * px_size;
    Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(dst), static_cast<uint32_t>(w) * px_size);
  }

  lv_display_flush_ready(disp);
}

void boardInitDisplay() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  rgb_bus = new Arduino_ESP32RGBPanel(
      GFX_NOT_DEFINED, GFX_NOT_DEFINED, GFX_NOT_DEFINED,
      40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
      45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
      5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
      8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */);

  gfx = new Arduino_RPi_DPI_RGBPanel(
      rgb_bus,
      800, 0, 8, 4, 8,
      480, 0, 8, 4, 8,
      1, 14000000, true);

  gfx->begin();
  gfx->fillScreen(WHITE);
  dispLogf("display: panel %dx%d fb=%p", gfx->width(), gfx->height(), gfx->getFramebuffer());
}

lv_display_t* boardInitLvgl() {
  const int32_t logical_w = kPhysVerRes;
  const size_t draw_buf_bytes =
      static_cast<size_t>(logical_w) * kDrawBufLines * sizeof(lv_color_t);

  draw_buf = static_cast<uint8_t*>(
      heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rotate_buf = static_cast<uint8_t*>(
      heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!draw_buf || !rotate_buf) {
    dispLogf("display: draw buffer alloc failed (%u bytes each)", static_cast<unsigned>(draw_buf_bytes));
    return nullptr;
  }

  lv_disp = lv_display_create(kPhysHorRes, kPhysVerRes);
  lv_display_set_color_format(lv_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(lv_disp, flushDisplay);
  lv_display_set_buffers(lv_disp, draw_buf, nullptr, draw_buf_bytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_rotation(lv_disp, LV_DISPLAY_ROTATION_90);
  dispLogf("display: LVGL %dx%d rot=90 partial buf=%u bytes (%u lines)",
           lv_display_get_horizontal_resolution(lv_disp),
           lv_display_get_vertical_resolution(lv_disp), static_cast<unsigned>(draw_buf_bytes),
           kDrawBufLines);
  return lv_disp;
}

void boardLogDisplayInfo() {
  if (!lv_disp) {
    dispLogf("display: lv_disp is null");
    return;
  }
  dispLogf("display: logical %dx%d physical %dx%d rotation=%d flushes=%u",
           lv_display_get_horizontal_resolution(lv_disp),
           lv_display_get_vertical_resolution(lv_disp), kPhysHorRes, kPhysVerRes,
           static_cast<int>(lv_display_get_rotation(lv_disp)), flush_count);
}

void boardInitTouch() {
  touch = new TAMC_GT911(19, 20, -1, 38, 800, 480);
  touch->begin();
  touch->setRotation(ROTATION_NORMAL);

  const uint8_t addr = probeGt911Addr();
  if (addr == 0) {
    touchDebug("touch: GT911 not found on I2C (expected 0x5D or 0x14)");
  } else {
    touchDebugf("touch: GT911 OK at 0x%02X (SDA=19 SCL=20 RST=38)", addr);
  }

  touchDebug("touch: manual scroll (no LVGL indev — avoids inverted coast)");
}

void boardSetScrollTarget(lv_obj_t* scroll_obj) {
  scroll_target = scroll_obj;
}

void boardPollTouchScroll() {
  if (!touch || !scroll_target) {
    return;
  }

  touch->read();

  if (!touch->isTouched) {
    if (drag_active) {
      const int32_t total_dx = touch_last_x - drag_start_x;
      const int32_t total_dy = drag_last_y - drag_start_y;
      const int32_t abs_dx = total_dx < 0 ? -total_dx : total_dx;
      const int32_t abs_dy = total_dy < 0 ? -total_dy : total_dy;

      if (abs_dx <= kTapMoveThreshold && abs_dy <= kTapMoveThreshold) {
        lv_area_t area;
        lv_obj_get_coords(scroll_target, &area);
        const int32_t page_h = lv_obj_get_height(scroll_target);
        const int32_t rel_y = drag_start_y - area.y1;
        const int32_t zone_top = page_h * kPageZonePct / 100;
        const int32_t zone_bottom = page_h - zone_top;

        if (rel_y < zone_top) {
          lv_obj_scroll_by_bounded(scroll_target, 0, -page_h, LV_ANIM_OFF);
          scroll_velocity = 0;
          last_drag_dy = 0;
          last_tap_ms = 0;
          touchDebugf("touch: tap up page dy=%d y=%d", -page_h, drag_start_y);
        } else if (rel_y >= zone_bottom) {
          lv_obj_scroll_by_bounded(scroll_target, 0, page_h, LV_ANIM_OFF);
          scroll_velocity = 0;
          last_drag_dy = 0;
          last_tap_ms = 0;
          touchDebugf("touch: tap down page dy=%d y=%d", page_h, drag_start_y);
        } else if (tap_tracking && tap_max_move <= kTapMaxMovePx) {
          const uint32_t now_ms = millis();
          if ((now_ms - tap_down_ms) <= kTapMaxDurationMs) {
            registerTap(tap_down_x, tap_down_y, now_ms);
          }
        }
      } else {
        if (scroll_velocity == 0 && last_drag_dy != 0) {
          scroll_velocity = last_drag_dy;
        }
        if (scroll_velocity != 0) {
          scroll_velocity = (scroll_velocity * kReleaseBoostPct) / 100;
          if (scroll_velocity > kMaxScrollVelocity) {
            scroll_velocity = kMaxScrollVelocity;
          } else if (scroll_velocity < -kMaxScrollVelocity) {
            scroll_velocity = -kMaxScrollVelocity;
          }
          last_inertia_ms = 0;
        }
      }
    }
    tap_tracking = false;
    drag_active = false;
    return;
  }

  int x = 0;
  int y = 0;
  mapTouchPoint(x, y);
  touch_last_x = x;
  touch_last_y = y;

  lv_area_t area;
  lv_obj_get_coords(scroll_target, &area);
  const bool in_scroll_area =
      x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2;
  if (!in_scroll_area) {
    drag_active = false;
    scroll_velocity = 0;
    tap_tracking = false;
    return;
  }

  if (!drag_active) {
    drag_active = true;
    drag_start_x = x;
    drag_start_y = y;
    drag_last_y = y;
    scroll_velocity = 0;
    last_drag_dy = 0;
    tap_tracking = true;
    tap_down_x = x;
    tap_down_y = y;
    tap_down_ms = millis();
    tap_max_move = 0;
    return;
  }

  if (tap_tracking) {
    const int32_t dx = x - tap_down_x;
    const int32_t dy_from_down = y - tap_down_y;
    const int32_t dist = (dx < 0 ? -dx : dx) + (dy_from_down < 0 ? -dy_from_down : dy_from_down);
    if (dist > tap_max_move) {
      tap_max_move = dist;
    }
    if (tap_max_move > kTapMaxMovePx) {
      tap_tracking = false;
    }
  }

  const int32_t dy = drag_last_y - y;
  drag_last_y = y;
  if (dy == 0) {
    return;
  }

  last_drag_dy = dy;
  scroll_velocity = (scroll_velocity + dy * 4) / 5;
  if (scroll_velocity > kMaxScrollVelocity) {
    scroll_velocity = kMaxScrollVelocity;
  } else if (scroll_velocity < -kMaxScrollVelocity) {
    scroll_velocity = -kMaxScrollVelocity;
  }

  pending_scroll_dy += dy;
}

bool boardConsumeDoubleTap() {
  if (!double_tap_ready) {
    return false;
  }
  double_tap_ready = false;
  scroll_velocity = 0;
  pending_scroll_dy = 0;
  return true;
}

void boardFlushPendingScroll() {
  if (!scroll_target || pending_scroll_dy == 0) {
    return;
  }

  lv_obj_scroll_by_bounded(scroll_target, 0, pending_scroll_dy, LV_ANIM_OFF);
  pending_scroll_dy = 0;
}

bool boardScrollActive() {
  return drag_active || scroll_velocity != 0 || pending_scroll_dy != 0;
}

void boardPollScrollInertia() {
  if (!scroll_target || drag_active || scroll_velocity == 0) {
    return;
  }

  const uint32_t now = millis();
  if (last_inertia_ms != 0 && (now - last_inertia_ms) < kInertiaStepMs) {
    return;
  }
  last_inertia_ms = now;

  const int32_t before = lv_obj_get_scroll_y(scroll_target);
  lv_obj_scroll_by_bounded(scroll_target, 0, scroll_velocity, LV_ANIM_OFF);
  const int32_t after = lv_obj_get_scroll_y(scroll_target);
  if (after == before) {
    scroll_velocity = 0;
    return;
  }

  scroll_velocity = (scroll_velocity * kScrollFrictionPct) / 100;
  if (scroll_velocity > -1 && scroll_velocity < 1) {
    scroll_velocity = 0;
  }
}

void boardLogScrollMetrics(lv_obj_t* scroll_target) {
  if (!scroll_target) {
    touchDebug("scroll: no scroll target set");
    return;
  }

  lv_obj_update_layout(scroll_target);
  lv_area_t area;
  lv_obj_get_coords(scroll_target, &area);

  const int32_t scroll_y = lv_obj_get_scroll_y(scroll_target);
  const int32_t top = lv_obj_get_scroll_top(scroll_target);
  const int32_t bottom = lv_obj_get_scroll_bottom(scroll_target);
  const uint32_t children = lv_obj_get_child_count(scroll_target);

  touchDebugf("scroll: area (%d,%d)-(%d,%d) h=%d children=%u",
              area.x1, area.y1, area.x2, area.y2, lv_obj_get_height(scroll_target),
              static_cast<unsigned>(children));
  touchDebugf("scroll: y=%d top=%d bottom=%d (bottom>0 means list can scroll)",
              scroll_y, top, bottom);
}

void boardSetTouchDebug(bool enable) {
  touch_debug = enable;
  touchDebugf("touch: debug %s", enable ? "enabled" : "disabled");
}

#endif
