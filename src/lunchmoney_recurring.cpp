#include "lunchmoney_recurring.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <time.h>

namespace {

constexpr time_t kDaySeconds = 24 * 60 * 60;

time_t startOfDay(time_t t) {
  struct tm parts;
  localtime_r(&t, &parts);
  parts.tm_hour = 0;
  parts.tm_min = 0;
  parts.tm_sec = 0;
  return mktime(&parts);
}

time_t endOfDay(time_t t) { return startOfDay(t) + kDaySeconds - 1; }

int daysInMonth(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

bool parseIsoDayParts(const char* text, int& year, int& month, int& day) {
  if (!text || strlen(text) < 10) {
    return false;
  }
  if (text[4] != '-' || text[7] != '-') {
    return false;
  }
  year = atoi(text);
  month = atoi(text + 5);
  day = atoi(text + 8);
  return year > 1900 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

time_t makeDay(int year, int month, int day) {
  struct tm parts = {};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  return startOfDay(mktime(&parts));
}

bool isWithinInterval(time_t date, time_t start, time_t end) {
  return date >= start && date <= end;
}

bool isBeforeDay(time_t a, time_t b) { return a < b; }
bool isAfterDay(time_t a, time_t b) { return a > b; }

time_t lastDayOfMonth(time_t anchor) {
  struct tm parts;
  localtime_r(&anchor, &parts);
  const int dim = daysInMonth(parts.tm_year + 1900, parts.tm_mon + 1);
  parts.tm_mday = dim;
  return startOfDay(mktime(&parts));
}

time_t alignToWindowMonth(time_t base, time_t window_start) {
  struct tm base_parts;
  struct tm window_parts;
  localtime_r(&base, &base_parts);
  localtime_r(&window_start, &window_parts);
  const int desired = base_parts.tm_mday;
  const int last = daysInMonth(window_parts.tm_year + 1900, window_parts.tm_mon + 1);
  window_parts.tm_mday = desired > last ? last : desired;
  return startOfDay(mktime(&window_parts));
}

struct Duration {
  int years = 0;
  int months = 0;
  int weeks = 0;
  int days = 0;
};

bool parseCadenceDuration(const String& cadence, Duration& out) {
  String normalized = cadence;
  normalized.toLowerCase();
  normalized.trim();
  if (normalized.isEmpty()) {
    return false;
  }

  int magnitude = 1;
  int pos = 0;
  while (pos < (int)normalized.length() && isdigit((unsigned char)normalized[pos])) {
    pos++;
  }
  if (pos > 0) {
    magnitude = normalized.substring(0, pos).toInt();
    if (magnitude <= 0) {
      magnitude = 1;
    }
  }
  const bool has_number = pos > 0;

  if (normalized.indexOf("quarter") >= 0) {
    out.months = magnitude * 3;
    return true;
  }
  if (normalized.indexOf("year") >= 0 || normalized.indexOf("annual") >= 0) {
    out.years = magnitude;
    return true;
  }
  if (normalized.indexOf("month") >= 0) {
    if (!has_number && (normalized.indexOf("other") >= 0 || normalized.indexOf("bi") >= 0)) {
      out.months = 2;
    } else if (!has_number &&
               (normalized.indexOf("semi") >= 0 || normalized.indexOf("twice") >= 0)) {
      out.days = 15;
    } else {
      out.months = magnitude;
    }
    return true;
  }
  if (normalized.indexOf("week") >= 0) {
    out.weeks = (!has_number && normalized.indexOf("bi") >= 0) ? 2 : magnitude;
    return true;
  }
  if (normalized.indexOf("day") >= 0) {
    out.days = (!has_number && normalized.indexOf("bi") >= 0) ? 2 : magnitude;
    return true;
  }
  return false;
}

time_t addDuration(time_t date, const Duration& duration) {
  struct tm parts;
  localtime_r(&date, &parts);
  if (duration.years) {
    parts.tm_year += duration.years;
  }
  if (duration.months) {
    parts.tm_mon += duration.months;
  }
  if (duration.weeks) {
    parts.tm_mday += duration.weeks * 7;
  }
  if (duration.days) {
    parts.tm_mday += duration.days;
  }
  return startOfDay(mktime(&parts));
}

time_t projectForwardWithCadence(time_t candidate, const Duration& duration, time_t target,
                                 time_t window_start, time_t window_end) {
  time_t projected = candidate;
  for (int i = 0; i < 60 && isBeforeDay(projected, target); i++) {
    projected = addDuration(projected, duration);
  }
  if (window_start && window_end && !isWithinInterval(projected, window_start, window_end)) {
    return 0;
  }
  return projected;
}

struct AdjustmentResult {
  time_t candidate;
  bool adjusted;
};

AdjustmentResult adjustCandidateToWindow(time_t candidate, time_t window_start, time_t window_end,
                                         const Duration* cadence) {
  AdjustmentResult result;
  result.candidate = candidate;
  result.adjusted = false;
  if (isAfterDay(candidate, window_end)) {
    return result;
  }
  if (!isBeforeDay(candidate, window_start)) {
    return result;
  }

  time_t adjusted = candidate;
  bool was_adjusted = false;
  if (cadence) {
    const time_t projected =
        projectForwardWithCadence(candidate, *cadence, window_start, window_start, window_end);
    if (projected && isWithinInterval(projected, window_start, window_end)) {
      adjusted = projected;
      was_adjusted = true;
    }
  }
  if (!isWithinInterval(adjusted, window_start, window_end)) {
    const time_t aligned = alignToWindowMonth(candidate, window_start);
    if (isWithinInterval(aligned, window_start, window_end)) {
      adjusted = aligned;
      was_adjusted = true;
    }
  }
  result.candidate = adjusted;
  result.adjusted = was_adjusted;
  return result;
}

time_t adjustCandidateToReference(time_t candidate, time_t reference, const Duration* cadence,
                                  bool candidate_adjusted_by_window, time_t window_start,
                                  time_t window_end) {
  if (!isBeforeDay(candidate, reference)) {
    return candidate;
  }
  if (!cadence) {
    return candidate_adjusted_by_window ? candidate : 0;
  }

  time_t projected = candidate;
  for (int i = 0; i < 60 && isBeforeDay(projected, reference); i++) {
    const time_t next = addDuration(projected, *cadence);
    if (window_start && window_end && isAfterDay(next, window_end)) {
      break;
    }
    projected = next;
    const bool in_window =
        !window_start || !window_end || isWithinInterval(projected, window_start, window_end);
    if (in_window && !isBeforeDay(projected, reference)) {
      return projected;
    }
  }
  return candidate;
}

time_t pickCandidateDate(const RecurringExpense& expense) {
  if (!expense.next_occurrence.isEmpty()) {
    time_t day = 0;
    if (parseIsoDay(expense.next_occurrence.c_str(), day)) {
      return day;
    }
  }
  if (!expense.anchor_date.isEmpty()) {
    time_t day = 0;
    if (parseIsoDay(expense.anchor_date.c_str(), day)) {
      return day;
    }
  }
  if (!expense.start_date.isEmpty()) {
    time_t day = 0;
    if (parseIsoDay(expense.start_date.c_str(), day)) {
      return day;
    }
  }
  return 0;
}

}  // namespace

time_t startOfToday() { return startOfDay(time(nullptr)); }

bool parseIsoDay(const char* text, time_t& out_day) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (!parseIsoDayParts(text, year, month, day)) {
    return false;
  }
  out_day = makeDay(year, month, day);
  return out_day > 0;
}

time_t deriveReferenceDay(const char* start_date, const char* end_date, time_t today_day) {
  time_t start = 0;
  time_t end = 0;
  if (!parseIsoDay(start_date, start) || !parseIsoDay(end_date, end)) {
    return today_day;
  }
  if (today_day < start) {
    return start;
  }
  struct tm end_parts;
  localtime_r(&end, &end_parts);
  end_parts.tm_mday += 1;
  const time_t day_after_end = startOfDay(mktime(&end_parts));
  if (today_day >= day_after_end) {
    return end;
  }
  return today_day;
}

bool getWindowRange(const char* start_date, const char* end_date, time_t& window_start,
                    time_t& window_end) {
  if (!parseIsoDay(start_date, window_start) || !parseIsoDay(end_date, window_end)) {
    return false;
  }
  window_end = endOfDay(window_end);
  return true;
}

time_t getRecurringOccurrenceDay(const RecurringExpense& expense, const char* window_start_text,
                                 const char* window_end_text, time_t reference_day) {
  time_t candidate = pickCandidateDate(expense);
  if (!candidate) {
    return 0;
  }

  time_t window_start = 0;
  time_t window_end = 0;
  const bool has_window =
      getWindowRange(window_start_text, window_end_text, window_start, window_end);

  Duration cadence_duration;
  const Duration* cadence =
      parseCadenceDuration(expense.cadence, cadence_duration) ? &cadence_duration : nullptr;

  bool adjusted_by_window = false;
  if (has_window) {
    if (isAfterDay(candidate, window_end)) {
      return 0;
    }
    const AdjustmentResult adjustment =
        adjustCandidateToWindow(candidate, window_start, window_end, cadence);
    candidate = adjustment.candidate;
    adjusted_by_window = adjustment.adjusted;
    if (!isWithinInterval(candidate, window_start, window_end)) {
      return 0;
    }
  }

  if (has_window) {
    const time_t adjusted = adjustCandidateToReference(
        candidate, reference_day, cadence, adjusted_by_window, window_start, window_end);
    if (!adjusted) {
      return 0;
    }
    candidate = adjusted;
  }

  return candidate;
}

bool isRecurringInstancePending(const RecurringInstance& instance, time_t reference_day,
                                time_t window_start, time_t window_end) {
  (void)window_start;
  (void)window_end;
  if (!instance.expense || !instance.occurrence_day) {
    return false;
  }

  const time_t occurrence = startOfDay(instance.occurrence_day);
  const time_t reference = startOfDay(reference_day);

  const String& type = instance.expense->type;
  if (type == "cleared") {
    return occurrence > reference;
  }
  return true;
}

bool hasFoundTransactionForOccurrence(const RecurringInstance& instance, int tolerance_days) {
  if (!instance.expense || !instance.occurrence_day) {
    return false;
  }
  const int tolerance = tolerance_days < 0 ? 0 : tolerance_days;
  const time_t tolerance_seconds = static_cast<time_t>(tolerance) * kDaySeconds;
  const time_t target = startOfDay(instance.occurrence_day);

  for (const String& date_text : instance.expense->found_dates) {
    time_t entry_day = 0;
    if (!parseIsoDay(date_text.c_str(), entry_day)) {
      continue;
    }
    entry_day = startOfDay(entry_day);
    const time_t diff = llabs(entry_day - target);
    if (diff <= tolerance_seconds) {
      return true;
    }
  }
  return false;
}

float resolveRecurringAmount(const RecurringExpense& expense) {
  if (expense.has_to_base) {
    return fabsf(expense.to_base);
  }
  return fabsf(expense.amount);
}

void buildRecurringInstances(const std::vector<RecurringExpense>& expenses,
                             const char* window_start, const char* window_end,
                             time_t reference_day, std::vector<RecurringInstance>& out_instances) {
  out_instances.clear();
  out_instances.reserve(expenses.size());

  for (const RecurringExpense& expense : expenses) {
    const time_t occurrence =
        getRecurringOccurrenceDay(expense, window_start, window_end, reference_day);
    if (!occurrence) {
      continue;
    }
    RecurringInstance instance;
    instance.expense = &expense;
    instance.occurrence_day = occurrence;
    out_instances.push_back(instance);
  }
}

float upcomingTotalForCategory(const std::vector<RecurringInstance>& instances, int category_id,
                               time_t reference_day, time_t window_start, time_t window_end) {
  float total = 0;
  for (const RecurringInstance& instance : instances) {
    if (!instance.expense || !instance.expense->has_category ||
        instance.expense->category_id != category_id) {
      continue;
    }
    if (!isRecurringInstancePending(instance, reference_day, window_start, window_end)) {
      continue;
    }
    if (hasFoundTransactionForOccurrence(instance)) {
      continue;
    }
    total += resolveRecurringAmount(*instance.expense);
  }
  return total;
}
