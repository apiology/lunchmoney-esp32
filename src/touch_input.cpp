#ifndef BOARD_8048S043
#include "touch_input.h"

#include <Arduino.h>
#include <Wire.h>
#include <XPT2046_Touchscreen.h>

// Classic CYD (ESP32-2432S028R) — resistive XPT2046
#ifndef BOARD_ES3C28P

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

#if defined(CONFIG_IDF_TARGET_ESP32S3)
static SPIClass touchscreenSPI(HSPI);
#else
static SPIClass touchscreenSPI(VSPI);
#endif

static XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
static bool ready = false;

void initTouch() {
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);
  ready = true;
}

void readTouch(lv_indev_t* indev, lv_indev_data_t* data) {
  if (!ready) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = map(p.x, 200, 3700, 1, 240);
    data->point.y = map(p.y, 240, 3800, 1, 320);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

#else
// ES3C28P — capacitive FT6336G on I2C
#define TOUCH_SDA 16
#define TOUCH_SCL 15
#define TOUCH_RST 18
#define TOUCH_INT 17
#define FT6336_ADDR 0x38

static bool ready = false;

void initTouch() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ready = true;
}

void readTouch(lv_indev_t* indev, lv_indev_data_t* data) {
  if (!ready) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  if (Wire.requestFrom(FT6336_ADDR, 5) < 5) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  const uint8_t touches = Wire.read() & 0x0F;
  if (touches == 0) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  const uint8_t xh = Wire.read();
  const uint8_t xl = Wire.read();
  const uint8_t yh = Wire.read();
  const uint8_t yl = Wire.read();

  const int x = ((xh & 0x0F) << 8) | xl;
  const int y = ((yh & 0x0F) << 8) | yl;

  data->state = LV_INDEV_STATE_PRESSED;
  data->point.x = map(y, 0, 320, 0, 239);
  data->point.y = map(x, 0, 240, 0, 319);
}
#endif
#endif
