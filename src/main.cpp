#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "display.h"
#include "lunchmoney.h"
#include "secrets.h"

LunchMoneyClient lunchmoney;
BudgetUI budget_ui;
std::vector<BudgetItem> budgets;

unsigned long last_fetch_ms = 0;
constexpr unsigned long kRefreshMs = 3UL * 60UL * 60UL * 1000UL;
bool fetching = false;

static bool secretsLookConfigured() {
  return strcmp(WIFI_SSID, "your-wifi-ssid") != 0 &&
         strcmp(WIFI_PASSWORD, "your-wifi-password") != 0;
}

static bool connectWifi() {
  if (!secretsLookConfigured()) {
    budget_ui.setStatus("Edit include/secrets.h");
    Serial.println("WIFI_SSID/WIFI_PASSWORD still set to example placeholders");
    return false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  budget_ui.setStatus("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (int attempt = 0; attempt < 120; attempt++) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      char msg[48];
      snprintf(msg, sizeof(msg), "WiFi OK  %s", WiFi.localIP().toString().c_str());
      budget_ui.setStatus(msg);
      Serial.print("WiFi connected: ");
      Serial.println(WiFi.localIP());
      delay(800);
      return true;
    }

    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      break;
    }

    char msg[32];
    snprintf(msg, sizeof(msg), "WiFi... %ds", (attempt + 1) / 4);
    budget_ui.setStatus(msg);
    lv_task_handler();
    delay(250);
  }

  char msg[40];
  snprintf(msg, sizeof(msg), "WiFi failed (%d)", WiFi.status());
  budget_ui.setStatus(msg);
  Serial.print("WiFi failed, status=");
  Serial.println(WiFi.status());
  return false;
}

void refreshBudgets(bool force = false) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long now = millis();
  if (!force && (now - last_fetch_ms) < kRefreshMs) {
    return;
  }
  if (fetching) {
    return;
  }

  fetching = true;
  budget_ui.setStatus("Fetching budgets...");

  String error;
  if (lunchmoney.fetchCurrentMonth(budgets, error)) {
    budget_ui.setItems(budgets);
    char status[48];
    snprintf(status, sizeof(status), "%u budgets", static_cast<unsigned>(budgets.size()));
    budget_ui.setStatus(status);
    last_fetch_ms = now;
  } else {
    budget_ui.setStatus(error.c_str());
  }

  fetching = false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("lunchmoney-esp32 starting");
  Serial.println("touch debug: open monitor at 115200, drag the budget list");

  lv_init();
  boardInitDisplay();
  if (!boardInitLvgl()) {
    Serial.println("display init failed — halting");
    while (true) {
      delay(1000);
    }
  }
  boardInitTouch();
  boardLogDisplayInfo();

  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  budget_ui.begin(screen);
  boardSetScrollTarget(budget_ui.scrollContainer());
  budget_ui.setStatus("Starting...");

  for (int i = 0; i < 8; i++) {
    lv_task_handler();
  }
  boardLogDisplayInfo();
  Serial.printf("screen children: %u\n",
                static_cast<unsigned>(lv_obj_get_child_count(screen)));

  lunchmoney.begin(LUNCHMONEY_API_KEY);

  if (connectWifi()) {
    refreshBudgets(true);
  }
}

void loop() {
  const bool scrolling = boardScrollActive();

  if (!scrolling) {
    if (WiFi.status() != WL_CONNECTED) {
      if (connectWifi()) {
        refreshBudgets(true);
      }
    } else {
      refreshBudgets(false);
    }
  }

  boardPollTouchScroll();
  boardFlushPendingScroll();
  boardPollScrollInertia();

  const int handler_passes = scrolling ? 6 : 1;
  for (int i = 0; i < handler_passes; i++) {
    lv_task_handler();
  }

  lv_tick_inc(scrolling ? 2 : 5);
  delay(scrolling ? 0 : 5);
}
