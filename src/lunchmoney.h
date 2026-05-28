#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <vector>

struct BudgetItem {
  String name;
  float budget = 0;
  float spent = 0;
  float upcoming = 0;
  float remaining = 0;
  float available = 0;
  bool has_available = false;
  bool schedule_suffix = false;
  int order = 0;
};

class LunchMoneyClient {
 public:
  void begin(const char* api_key);
  bool fetchCurrentMonth(std::vector<BudgetItem>& out, String& error);
  static String decodeEntities(const char* raw);

 private:
  const char* api_key_ = nullptr;
  void monthRange(char* start_date, size_t start_len, char* end_date, size_t end_len);
  static int daysInMonth(int year, int month);
};

class BudgetUI {
 public:
  void begin(lv_obj_t* parent);
  void setStatus(const char* text);
  void setItems(const std::vector<BudgetItem>& items);
  lv_obj_t* scrollContainer() const { return scroll_container_; }

 private:
  lv_obj_t* status_label_ = nullptr;
  lv_obj_t* scroll_container_ = nullptr;
  void clearRows();
  static void formatMoney(char* buf, size_t len, float amount);
  static void makePointerTransparent(lv_obj_t* obj);
};
