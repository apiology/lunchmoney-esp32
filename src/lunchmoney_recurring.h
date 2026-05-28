#pragma once

#include <Arduino.h>
#include <vector>

struct RecurringExpense {
  int id = 0;
  int category_id = 0;
  bool has_category = false;
  String cadence;
  String type;  // "cleared" or "suggested"
  float amount = 0;
  float to_base = 0;
  bool has_to_base = false;
  String next_occurrence;
  String anchor_date;
  String start_date;
  std::vector<String> found_dates;
};

struct RecurringInstance {
  const RecurringExpense* expense = nullptr;
  time_t occurrence_day = 0;
};

bool parseIsoDay(const char* text, time_t& out_day);
time_t startOfToday();
time_t deriveReferenceDay(const char* start_date, const char* end_date, time_t today_day);
bool getWindowRange(const char* start_date, const char* end_date, time_t& window_start,
                    time_t& window_end);
time_t getRecurringOccurrenceDay(const RecurringExpense& expense, const char* window_start,
                                 const char* window_end, time_t reference_day);
bool isRecurringInstancePending(const RecurringInstance& instance, time_t reference_day,
                                time_t window_start, time_t window_end);
bool hasFoundTransactionForOccurrence(const RecurringInstance& instance, int tolerance_days = 7);
float resolveRecurringAmount(const RecurringExpense& expense);

void buildRecurringInstances(const std::vector<RecurringExpense>& expenses,
                             const char* window_start, const char* window_end,
                             time_t reference_day,
                             std::vector<RecurringInstance>& out_instances);

float upcomingTotalForCategory(const std::vector<RecurringInstance>& instances, int category_id,
                               time_t reference_day, time_t window_start, time_t window_end);
