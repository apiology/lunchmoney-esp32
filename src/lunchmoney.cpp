#include "lunchmoney.h"
#include "display.h"
#include <lvgl.h>

#include <ArduinoJson.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

namespace {

constexpr const char* kApiBase = "https://dev.lunchmoney.app/v1";
constexpr int32_t kStatusBarHeight = 56;
constexpr int32_t kBudgetBarHeight = 40;

bool syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 20; i++) {
    if (time(nullptr) > 1700000000) {
      return true;
    }
    delay(250);
  }
  return false;
}

void appendAsciiCodepoint(String& out, uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F) {
    return;
  }
  if (cp <= 0x7E) {
    out += static_cast<char>(cp);
    return;
  }
  if (cp == 0xA0) {
    out += ' ';
  } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x201B) {
    out += '\'';
  } else if (cp == 0x201C || cp == 0x201D) {
    out += '"';
  } else if (cp == 0x2013 || cp == 0x2014) {
    out += '-';
  } else if (cp == 0x2026) {
    out += "...";
  }
}

uint32_t readUtf8Codepoint(const char* text, size_t& index) {
  const unsigned char c0 = static_cast<unsigned char>(text[index]);
  if (c0 < 0x80) {
    return text[index++];
  }
  if ((c0 & 0xE0) == 0xC0 && text[index + 1]) {
    const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
    const uint32_t cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    index += 2;
    return cp;
  }
  if ((c0 & 0xF0) == 0xE0 && text[index + 2]) {
    const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
    const unsigned char c2 = static_cast<unsigned char>(text[index + 2]);
    const uint32_t cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    index += 3;
    return cp;
  }
  if ((c0 & 0xF8) == 0xF0 && text[index + 3]) {
    const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
    const unsigned char c2 = static_cast<unsigned char>(text[index + 2]);
    const unsigned char c3 = static_cast<unsigned char>(text[index + 3]);
    const uint32_t cp =
        ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    index += 4;
    return cp;
  }
  index++;
  return 0xFFFD;
}

bool parseNamedEntity(const char* text, size_t& index, uint32_t& cp) {
  struct EntityMap {
    const char* name;
    uint32_t cp;
  };
  static const EntityMap kEntities[] = {
      {"amp", '&'},  {"lt", '<'},   {"gt", '>'},  {"quot", '"'},
      {"apos", '\''}, {"nbsp", ' '},
  };
  const size_t start = index;
  size_t end = start;
  while (text[end] && std::isalnum(static_cast<unsigned char>(text[end]))) {
    end++;
  }
  if (text[end] != ';') {
    return false;
  }
  const size_t len = end - start;
  for (const EntityMap& entity : kEntities) {
    if (len == strlen(entity.name) && strncmp(text + start, entity.name, len) == 0) {
      cp = entity.cp;
      index = end + 1;
      return true;
    }
  }
  return false;
}

bool parseNumericEntity(const char* text, size_t& index, uint32_t& cp) {
  if (text[index] != '#' || !text[index + 1]) {
    return false;
  }
  index++;
  const bool hex = text[index] == 'x' || text[index] == 'X';
  if (hex) {
    index++;
  }
  uint32_t value = 0;
  bool parsed = false;
  while (text[index]) {
    const char ch = text[index];
    if (ch == ';') {
      if (!parsed) {
        return false;
      }
      index++;
      cp = value;
      return true;
    }
    int digit = -1;
    if (ch >= '0' && ch <= '9') {
      digit = ch - '0';
    } else if (hex && ch >= 'a' && ch <= 'f') {
      digit = 10 + (ch - 'a');
    } else if (hex && ch >= 'A' && ch <= 'F') {
      digit = 10 + (ch - 'A');
    } else {
      return false;
    }
    value = value * (hex ? 16 : 10) + static_cast<uint32_t>(digit);
    parsed = true;
    index++;
  }
  return false;
}

String sanitizeDisplayText(const char* raw) {
  if (!raw) {
    return "";
  }
  String out;
  out.reserve(strlen(raw));
  for (size_t i = 0; raw[i];) {
    const uint32_t cp = readUtf8Codepoint(raw, i);
    appendAsciiCodepoint(out, cp);
  }
  out.trim();
  return out;
}

}  // namespace

void LunchMoneyClient::begin(const char* api_key) { api_key_ = api_key; }

int LunchMoneyClient::daysInMonth(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

void LunchMoneyClient::monthRange(char* start_date, size_t start_len, char* end_date,
                                  size_t end_len) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  const int year = t.tm_year + 1900;
  const int month = t.tm_mon + 1;
  snprintf(start_date, start_len, "%04d-%02d-01", year, month);
  snprintf(end_date, end_len, "%04d-%02d-%02d", year, month, daysInMonth(year, month));
}

String LunchMoneyClient::decodeEntities(const char* raw) {
  if (!raw) {
    return "";
  }
  String decoded;
  decoded.reserve(strlen(raw));
  for (size_t i = 0; raw[i];) {
    if (raw[i] != '&') {
      const uint32_t cp = readUtf8Codepoint(raw, i);
      appendAsciiCodepoint(decoded, cp);
      continue;
    }
    const size_t entity_start = i;
    i++;
    uint32_t cp = 0;
    if (parseNumericEntity(raw, i, cp) || parseNamedEntity(raw, i, cp)) {
      appendAsciiCodepoint(decoded, cp);
      continue;
    }
    i = entity_start;
    appendAsciiCodepoint(decoded, readUtf8Codepoint(raw, i));
  }
  return sanitizeDisplayText(decoded.c_str());
}

bool LunchMoneyClient::fetchCurrentMonth(std::vector<BudgetItem>& out, String& error) {
  out.clear();

  if (WiFi.status() != WL_CONNECTED) {
    error = "WiFi not connected";
    return false;
  }

  if (!syncTime()) {
    error = "NTP sync failed";
    return false;
  }

  char start_date[11];
  char end_date[11];
  monthRange(start_date, sizeof(start_date), end_date, sizeof(end_date));

  String url = String(kApiBase) + "/budgets?start_date=" + start_date + "&end_date=" + end_date;

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + api_key_);
  http.addHeader("Content-Type", "application/json");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = "HTTP " + String(code) + ": " + http.errorToString(code);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    error = String("JSON: ") + err.c_str();
    return false;
  }

  if (doc.is<JsonObject>() && doc["error"].is<const char*>()) {
    error = doc["error"].as<const char*>();
    return false;
  }

  if (!doc.is<JsonArray>()) {
    error = "Unexpected API response";
    return false;
  }

  for (JsonObject row : doc.as<JsonArray>()) {
    if (row["archived"] | false) {
      continue;
    }
    if (row["is_income"] | false) {
      continue;
    }
    if (row["exclude_from_budget"] | false) {
      continue;
    }

    JsonObject month = row["data"][start_date];
    if (month.isNull() || month["budget_to_base"].isNull()) {
      continue;
    }

    BudgetItem item;
    item.name = decodeEntities(row["category_name"] | "");
    item.budget = month["budget_to_base"].as<float>();
    item.spent = month["spending_to_base"] | 0.0f;
    item.remaining = item.budget - item.spent;
    item.order = row["order"] | 0;
    out.push_back(item);
  }

  std::sort(out.begin(), out.end(), [](const BudgetItem& a, const BudgetItem& b) {
    if (a.remaining != b.remaining) {
      return a.remaining < b.remaining;
    }
    return a.order < b.order;
  });

  return true;
}

void BudgetUI::formatMoney(char* buf, size_t len, float amount) {
  snprintf(buf, len, "$%.0f", amount);
}

void BudgetUI::makePointerTransparent(lv_obj_t* obj) {
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SNAPPABLE);

  const uint32_t child_cnt = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < child_cnt; i++) {
    makePointerTransparent(lv_obj_get_child(obj, i));
  }
}

void BudgetUI::begin(lv_obj_t* parent) {
  status_label_ = lv_label_create(parent);
  lv_label_set_text(status_label_, "Starting...");
  lv_obj_set_width(status_label_, lv_pct(100));
  lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_update_layout(parent);

  scroll_container_ = lv_obj_create(parent);
  lv_obj_set_width(scroll_container_, lv_pct(100));
  lv_obj_set_height(scroll_container_, lv_display_get_vertical_resolution(nullptr) - kStatusBarHeight);
  lv_obj_align(scroll_container_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(scroll_container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scroll_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(scroll_container_, 12, 0);
  lv_obj_set_style_pad_all(scroll_container_, 8, 0);
  lv_obj_set_style_border_width(scroll_container_, 0, 0);
  lv_obj_set_style_bg_opa(scroll_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(scroll_container_, 0, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(scroll_container_, 0, LV_PART_SCROLLBAR);
  lv_obj_add_flag(scroll_container_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scroll_container_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll_container_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_snap_y(scroll_container_, LV_SCROLL_SNAP_NONE);
  lv_obj_update_layout(scroll_container_);
}

void BudgetUI::setStatus(const char* text) {
  if (status_label_) {
    lv_label_set_text(status_label_, text);
  }
}

void BudgetUI::clearRows() {
  if (!scroll_container_) {
    return;
  }
  while (lv_obj_get_child_cnt(scroll_container_) > 0) {
    lv_obj_delete(lv_obj_get_child(scroll_container_, 0));
  }
}

void BudgetUI::setItems(const std::vector<BudgetItem>& items) {
  clearRows();

  for (const BudgetItem& item : items) {
    lv_obj_t* card = lv_obj_create(scroll_container_);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 0, 0);
    makePointerTransparent(card);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_text(title, item.name.c_str());

    lv_obj_t* bar = lv_bar_create(card);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, kBudgetBarHeight);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    const int pct = item.budget > 0 ? std::min(100, (int)roundf((item.spent / item.budget) * 100)) : 0;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    const lv_color_t bar_color =
        item.remaining >= 0 ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);

    char detail_text[64];
    char budget_str[16];
    char spent_str[16];
    char remain_str[16];
    formatMoney(budget_str, sizeof(budget_str), item.budget);
    formatMoney(spent_str, sizeof(spent_str), item.spent);
    formatMoney(remain_str, sizeof(remain_str), fabsf(item.remaining));
    if (item.remaining >= 0) {
      snprintf(detail_text, sizeof(detail_text), "%s of %s - %s left", spent_str, budget_str, remain_str);
    } else {
      snprintf(detail_text, sizeof(detail_text), "%s of %s - %s over", spent_str, budget_str, remain_str);
    }

    lv_obj_t* summary_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(summary_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(summary_lbl, detail_text);
    lv_obj_set_style_text_color(
        summary_lbl,
        item.remaining >= 0 ? lv_palette_main(LV_PALETTE_GREEN)
                            : lv_palette_main(LV_PALETTE_RED),
        0);
  }

  lv_obj_update_layout(scroll_container_);
  lv_obj_scroll_to_y(scroll_container_, 0, LV_ANIM_OFF);
  boardLogScrollMetrics(scroll_container_);
}
