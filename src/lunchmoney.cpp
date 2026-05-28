#include "lunchmoney.h"
#include "display.h"
#include "lunchmoney_recurring.h"
#include <lvgl.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <time.h>

namespace {

constexpr const char* kApiBase = "https://api.lunchmoney.dev/v2";
constexpr int32_t kStatusBarHeight = 56;
constexpr int32_t kBudgetBarHeight = 40;

struct CategoryMeta {
  int id = 0;
  String name;
  bool is_income = false;
  bool exclude_from_budget = false;
  bool archived = false;
  bool is_group = false;
  int group_id = 0;
  int order = 0;
  bool schedule_suffix = false;
};

bool timeLooksValid() {
  return time(nullptr) > 1700000000;
}

bool waitForDns(int max_attempts) {
  IPAddress ip;
  for (int i = 0; i < max_attempts; i++) {
    if (WiFi.hostByName("pool.ntp.org", ip)) {
      return true;
    }
    delay(250);
    lv_task_handler();
  }
  return false;
}

bool syncTime() {
  if (timeLooksValid()) {
    return true;
  }

  static constexpr const char* kNtp1 = "pool.ntp.org";
  static constexpr const char* kNtp2 = "time.nist.gov";
  static constexpr const char* kNtp3 = "time.google.com";

  // DNS often lags WiFi association; keep polling while SNTP retries below.
  waitForDns(40);

  for (int attempt = 0; attempt < 5; attempt++) {
    if (esp_sntp_enabled()) {
      esp_sntp_stop();
    }
    configTime(0, 0, kNtp1, kNtp2, kNtp3);

    const int polls = 32 + attempt * 16;
    for (int i = 0; i < polls; i++) {
      if (timeLooksValid()) {
        return true;
      }
      delay(250);
      lv_task_handler();
    }
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

bool decodeNextCodepoint(const char* raw, size_t& index, uint32_t& cp) {
  if (!raw[index]) {
    return false;
  }
  if (raw[index] != '&') {
    cp = readUtf8Codepoint(raw, index);
    return true;
  }
  const size_t entity_start = index;
  index++;
  if (parseNumericEntity(raw, index, cp) || parseNamedEntity(raw, index, cp)) {
    return true;
  }
  index = entity_start;
  cp = readUtf8Codepoint(raw, index);
  return true;
}

bool isTrailingWhitespaceCodepoint(uint32_t cp) {
  return cp == 0x20 || cp == 0xA0 || cp == 0x09 || cp == 0x0A || cp == 0x0D;
}

bool isScheduleSuffixCodepoint(uint32_t cp) {
  return cp == 0x1F5D3 || cp == 0x1F37E || cp == 0x1F512;
}

bool hasScheduleSuffixAtEnd(const char* raw) {
  if (!raw) {
    return false;
  }

  uint32_t codepoints[128];
  int count = 0;
  for (size_t i = 0; raw[i] && count < 127;) {
    uint32_t cp = 0;
    if (!decodeNextCodepoint(raw, i, cp)) {
      break;
    }
    codepoints[count++] = cp;
  }

  while (count > 0 && isTrailingWhitespaceCodepoint(codepoints[count - 1])) {
    count--;
  }
  if (count == 0) {
    return false;
  }

  int end = count - 1;
  if (codepoints[end] == 0xFE0F && end > 0) {
    end--;
  }
  return isScheduleSuffixCodepoint(codepoints[end]);
}

JsonObject pickOccurrence(JsonArray occurrences) {
  for (JsonObject occurrence : occurrences) {
    if (occurrence["current"] | false) {
      return occurrence;
    }
  }
  for (JsonObject occurrence : occurrences) {
    if (occurrence["in_range"] | false) {
      return occurrence;
    }
  }
  if (occurrences.size() > 0) {
    return occurrences[0].as<JsonObject>();
  }
  return JsonObject();
}

bool httpGet(const char* api_key, const String& url, String& body, String& error, int& http_code) {
  HTTPClient http;
  http.setTimeout(20000);
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + api_key);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cache-Control", "no-cache");

  http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    error = "HTTP " + String(http_code) + ": " + http.errorToString(http_code);
    http.end();
    return false;
  }

  body = http.getString();
  http.end();
  return true;
}

bool parseCategories(const String& body, std::vector<CategoryMeta>& categories, String& error) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    error = String("JSON categories: ") + err.c_str();
    return false;
  }

  JsonArray list = doc["categories"].as<JsonArray>();
  if (list.isNull()) {
    error = "Unexpected categories response";
    return false;
  }

  categories.clear();
  categories.reserve(list.size());
  for (JsonObject row : list) {
    CategoryMeta meta;
    meta.id = row["id"] | 0;
    const char* raw_name = row["name"] | "";
    meta.schedule_suffix = hasScheduleSuffixAtEnd(raw_name);
    meta.name = LunchMoneyClient::decodeEntities(raw_name);
    meta.is_income = row["is_income"] | false;
    meta.exclude_from_budget = row["exclude_from_budget"] | false;
    meta.archived = row["archived"] | false;
    meta.is_group = row["is_group"] | false;
    meta.group_id = row["group_id"] | 0;
    meta.order = row["order"] | 0;
    categories.push_back(meta);
  }
  return true;
}

const CategoryMeta* findCategory(const std::vector<CategoryMeta>& categories, int id) {
  for (const CategoryMeta& meta : categories) {
    if (meta.id == id) {
      return &meta;
    }
  }
  return nullptr;
}

bool parseRecurringItems(const String& body, std::vector<RecurringExpense>& recurring, String& error) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    error = String("JSON recurring: ") + err.c_str();
    return false;
  }

  JsonArray list = doc["recurring_items"].as<JsonArray>();
  if (list.isNull()) {
    error = "Unexpected recurring response";
    return false;
  }

  recurring.clear();
  recurring.reserve(list.size());
  for (JsonObject row : list) {
    JsonObject criteria = row["transaction_criteria"];
    if (criteria.isNull()) {
      continue;
    }

    RecurringExpense expense;
    expense.id = row["id"] | 0;
    if (!criteria["start_date"].isNull()) {
      expense.start_date = criteria["start_date"].as<const char*>();
    }
    if (!criteria["anchor_date"].isNull()) {
      expense.anchor_date = criteria["anchor_date"].as<const char*>();
    }
    expense.amount = criteria["amount"] | 0.0f;
    if (!criteria["to_base"].isNull()) {
      expense.to_base = criteria["to_base"].as<float>();
      expense.has_to_base = true;
    }

    const int quantity = criteria["quantity"] | 1;
    const char* granularity = criteria["granularity"] | "month";
    expense.cadence = String(quantity) + " " + granularity;

    const char* status = row["status"] | "suggested";
    expense.type = strcmp(status, "reviewed") == 0 ? "cleared" : "suggested";

    JsonObject overrides = row["overrides"];
    if (!overrides.isNull() && !overrides["category_id"].isNull()) {
      expense.category_id = overrides["category_id"] | 0;
      expense.has_category = true;
    }

    JsonObject matches = row["matches"];
    if (!matches.isNull()) {
      JsonArray expected = matches["expected_occurrence_dates"].as<JsonArray>();
      if (!expected.isNull() && expected.size() > 0) {
        expense.next_occurrence = expected[0].as<const char*>();
      }
      JsonArray found = matches["found_transactions"].as<JsonArray>();
      if (!found.isNull()) {
        for (JsonObject found_row : found) {
          const char* date = found_row["date"] | nullptr;
          if (date) {
            expense.found_dates.push_back(date);
          }
        }
      }
    }

    recurring.push_back(expense);
  }
  return true;
}

struct ProgressDraft {
  int category_id = 0;
  String name;
  float budget = 0;
  float spent = 0;
  float available = 0;
  bool has_available = false;
  float remaining_after_upcoming = 0;
  int order = 0;
};

bool draftHasCategoryId(const std::vector<ProgressDraft>& drafts, int category_id) {
  for (const ProgressDraft& draft : drafts) {
    if (draft.category_id == category_id) {
      return true;
    }
  }
  return false;
}

bool groupHasChildBudget(int group_id, const std::vector<ProgressDraft>& drafts,
                         const std::vector<CategoryMeta>& categories) {
  for (const ProgressDraft& draft : drafts) {
    const CategoryMeta* meta = findCategory(categories, draft.category_id);
    if (!meta || meta->is_group) {
      continue;
    }
    if (meta->group_id == group_id) {
      return true;
    }
  }
  return false;
}

bool shouldIncludeDraft(const ProgressDraft& draft, const CategoryMeta* meta,
                        const std::vector<ProgressDraft>& drafts,
                        const std::vector<CategoryMeta>& categories) {
  if (meta->is_group) {
    return !groupHasChildBudget(draft.category_id, drafts, categories);
  }

  if (meta->group_id > 0) {
    const CategoryMeta* parent = findCategory(categories, meta->group_id);
    if (parent && parent->is_group &&
        !groupHasChildBudget(meta->group_id, drafts, categories) &&
        draftHasCategoryId(drafts, meta->group_id)) {
      return false;
    }
  }

  return true;
}

bool buildExpenseDrafts(const String& summary_body, const std::vector<CategoryMeta>& categories,
                        const char* month_key, std::vector<ProgressDraft>& drafts, String& error) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, summary_body);
  if (err) {
    error = String("JSON summary: ") + err.c_str();
    return false;
  }

  if (doc.is<JsonObject>() && doc["error"].is<const char*>()) {
    error = doc["error"].as<const char*>();
    return false;
  }

  JsonArray rows = doc["categories"].as<JsonArray>();
  if (rows.isNull()) {
    error = "Unexpected summary response";
    return false;
  }

  drafts.clear();

  for (JsonObject row : rows) {
    const int category_id = row["category_id"] | 0;
    const CategoryMeta* meta = findCategory(categories, category_id);
    if (!meta || meta->archived || meta->exclude_from_budget || meta->is_income) {
      continue;
    }

    JsonObject totals = row["totals"];
    if (totals.isNull()) {
      continue;
    }

    JsonObject occurrence;
    JsonArray occurrences = row["occurrences"].as<JsonArray>();
    if (!occurrences.isNull()) {
      occurrence = pickOccurrence(occurrences);
    }

    float budget_amount = 0;
    if (!occurrence.isNull() && !occurrence["budgeted"].isNull()) {
      budget_amount = occurrence["budgeted"].as<float>();
    } else if (!totals["budgeted"].isNull()) {
      budget_amount = totals["budgeted"].as<float>();
    }

    if (budget_amount <= 0) {
      continue;
    }

    const float other_activity = totals["other_activity"] | 0.0f;
    const float recurring_activity = totals["recurring_activity"] | 0.0f;
    const float spent = other_activity + recurring_activity;

    ProgressDraft draft;
    draft.category_id = category_id;
    draft.name = meta->name;
    draft.budget = budget_amount;
    draft.spent = spent;
    if (!totals["available"].isNull()) {
      draft.available = totals["available"].as<float>();
      draft.has_available = true;
    }
    draft.order = meta->order;
    drafts.push_back(draft);
  }

  return true;
}

constexpr float kMoneyEpsilon = 0.005f;

int budgetSortTier(const BudgetItem& item) {
  if (item.remaining < -kMoneyEpsilon) {
    return 0;
  }
  if (item.remaining > kMoneyEpsilon) {
    return 1;
  }
  return 2;
}

bool hasScheduleSuffix(const BudgetItem& item) {
  return item.schedule_suffix;
}

bool compareBudgetItems(const BudgetItem& a, const BudgetItem& b) {
  const int tier_a = budgetSortTier(a);
  const int tier_b = budgetSortTier(b);
  if (tier_a != tier_b) {
    return tier_a < tier_b;
  }

  if (tier_a == 0) {
    if (fabsf(a.remaining - b.remaining) > kMoneyEpsilon) {
      return a.remaining < b.remaining;
    }
    return a.order < b.order;
  }

  if (tier_a == 1) {
    const bool suffix_a = hasScheduleSuffix(a);
    const bool suffix_b = hasScheduleSuffix(b);
    if (suffix_a != suffix_b) {
      return !suffix_a && suffix_b;
    }
    if (fabsf(a.remaining - b.remaining) > kMoneyEpsilon) {
      return a.remaining > b.remaining;
    }
    return a.order < b.order;
  }

  if (a.order != b.order) {
    return a.order < b.order;
  }
  return a.name < b.name;
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

  int http_code = 0;
  String body;

  String summary_url = String(kApiBase) + "/summary?start_date=" + start_date +
                       "&end_date=" + end_date + "&include_occurrences=true" +
                       "&include_exclude_from_budgets=true";
  if (!httpGet(api_key_, summary_url, body, error, http_code)) {
    return false;
  }
  const String summary_body = body;

  const String categories_url = String(kApiBase) + "/categories?format=flattened";
  if (!httpGet(api_key_, categories_url, body, error, http_code)) {
    return false;
  }

  std::vector<CategoryMeta> categories;
  if (!parseCategories(body, categories, error)) {
    return false;
  }

  const String recurring_url =
      String(kApiBase) + "/recurring_items?start_date=" + start_date + "&end_date=" + end_date;
  if (!httpGet(api_key_, recurring_url, body, error, http_code)) {
    return false;
  }

  std::vector<RecurringExpense> recurring;
  if (!parseRecurringItems(body, recurring, error)) {
    return false;
  }

  std::vector<ProgressDraft> drafts;
  if (!buildExpenseDrafts(summary_body, categories, start_date, drafts, error)) {
    return false;
  }

  const time_t today_day = startOfToday();
  time_t window_start = 0;
  time_t window_end = 0;
  getWindowRange(start_date, end_date, window_start, window_end);
  const time_t reference_day = deriveReferenceDay(start_date, end_date, today_day);

  std::vector<RecurringInstance> instances;
  buildRecurringInstances(recurring, start_date, end_date, reference_day, instances);

  for (ProgressDraft& draft : drafts) {
    const CategoryMeta* meta = findCategory(categories, draft.category_id);
    if (!meta) {
      continue;
    }
    if (!shouldIncludeDraft(draft, meta, drafts, categories)) {
      continue;
    }

    const float upcoming =
        upcomingTotalForCategory(instances, draft.category_id, reference_day, window_start,
                                 window_end);
    const float remaining_base =
        draft.has_available ? draft.available : (draft.budget - draft.spent);
    draft.remaining_after_upcoming = remaining_base - upcoming;

    BudgetItem item;
    item.name = draft.name;
    item.budget = draft.budget;
    item.spent = draft.spent;
    item.upcoming = upcoming;
    item.remaining = draft.remaining_after_upcoming;
    item.available = draft.available;
    item.has_available = draft.has_available;
    item.schedule_suffix = meta->schedule_suffix;
    item.order = draft.order;
    out.push_back(item);
  }

  std::sort(out.begin(), out.end(), compareBudgetItems);

  return true;
}

void BudgetUI::formatMoney(char* buf, size_t len, float amount) {
  snprintf(buf, len, "$%.2f", amount);
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
    const float projected_spent = item.spent + item.upcoming;
    const float progress_total =
        item.has_available ? (item.spent + item.available) : item.budget;
    int pct = 0;
    if (item.remaining < 0) {
      pct = 100;
    } else if (progress_total > 0) {
      pct = std::min(100, (int)roundf((projected_spent / progress_total) * 100));
    }
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
      snprintf(detail_text, sizeof(detail_text), "%s of %s - %s left", spent_str, budget_str,
               remain_str);
    } else {
      snprintf(detail_text, sizeof(detail_text), "%s of %s - %s over", spent_str, budget_str,
               remain_str);
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
