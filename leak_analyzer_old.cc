#include "base/logging.h"
#define dump(x) LOG(ERROR) << __func__ << " " << __LINE__ << ": " << #x << " = " << (x);
#define dumphex(x) LOG(ERROR) << __func__ << " " << __LINE__ << ": " << #x << " = " << std::hex << (unsigned long long)(x);
#define dumpptr(x) LOG(ERROR) << __func__ << " " << __LINE__ << ": " << #x << " = " << (x);
#define dumpstr(x) LOG(ERROR) << __func__ << " " << __LINE__ << ": " << #x << " = " << (x);

// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/leak_detector/leak_analyzer.h"

#include <algorithm>
#include <set>
#include <utility>

#define PrintSuspects(s) { \
  char buf[1024]; int offset = 0; \
  offset = snprintf(buf, sizeof(buf), "%d : {", __LINE__); \
  const char* delimiter = ""; \
  for (const auto& sus : (s)) { \
    offset += snprintf(buf + offset, sizeof(buf) - offset, "%s%d", delimiter, sus.size());\
    delimiter = ", "; \
  } \
  snprintf(buf + offset, sizeof(buf) - offset, "}\n"); \
  RAW_LOG(ERROR, buf); }

#define PrintHistogram(s) { \
  char buf[1024]; int offset = 0; \
  offset = snprintf(buf, sizeof(buf), "%d : {", __LINE__); \
  const char* delimiter = ""; \
  for (const auto& sus : (s)) { \
    offset += snprintf(buf + offset, sizeof(buf) - offset, \
        "%s%d : %d", delimiter, sus.first.size(), sus.second);\
    delimiter = ", "; \
  } \
  snprintf(buf + offset, sizeof(buf) - offset, "}\n"); \
  RAW_LOG(ERROR, buf); }

namespace leak_detector {

namespace {

using RankedEntry = RankedList::Entry;

// Increase suspicion scores by this much each time an entry is suspected as
// being a leak.
const int kSuspicionScoreIncrease = 1;

}  // namespace

void LeakAnalyzer::AddSample(const RankedList& ranked_list) {
  // Save the ranked entries from the previous call.
  prev_ranked_entries_ = ranked_entries_;

  // Save the current entries.
  ranked_entries_ = ranked_list;

  RankedList ranked_deltas(ranking_size_);
  for (const RankedEntry& entry : ranked_list) {
    // Determine what count was recorded for this value last time.
    uint32_t prev_count = 0;
    if (GetPreviousCountForValue(entry.value, &prev_count))
      ranked_deltas.Add(entry.value, entry.count - prev_count);
  }

  AnalyzeDeltas(ranked_deltas);
}

size_t LeakAnalyzer::Dump(const size_t buffer_size, char* buffer) const {
  size_t size_remaining = buffer_size;
  int attempted_size = 0;

  // Add a null terminator in case the rest of the code (which is conditional)
  // doesn't print anything.
  if (size_remaining)
    buffer[0] = '\0';

  // Buffer used for calling LeakDetectorValueType::ToString().
  char to_string_buffer[256];

  if (ranked_entries_.size() > 0) {
    // Dump the top entries.
    if (size_remaining > 1) {
      attempted_size =
          snprintf(buffer, size_remaining, "***** Top %zu %ss *****\n",
                   ranked_entries_.size(),
                   ranked_entries_.begin()->value.GetTypeName());
      size_remaining -= attempted_size;
      buffer += attempted_size;
    }

    for (const RankedEntry& entry : ranked_entries_) {
      if (size_remaining <= 1)
        break;
      if (entry.count == 0)
        break;

      // Determine what count was recorded for this value last time.
      char prev_entry_buffer[256];
      prev_entry_buffer[0] = '\0';

      uint32_t prev_count = 0;
      if (GetPreviousCountForValue(entry.value, &prev_count)) {
        snprintf(prev_entry_buffer, sizeof(prev_entry_buffer),
                 "(%10d)", entry.count - prev_count);
      }

      attempted_size =
          snprintf(
              buffer, size_remaining, "%10s: %10u %s\n",
              entry.value.ToString(sizeof(to_string_buffer), to_string_buffer),
              entry.count, prev_entry_buffer);
      size_remaining -= attempted_size;
      buffer += attempted_size;
    }
  }

  if (!suspected_leaks_.empty()) {
    // Report the suspected sizes.
    if (size_remaining > 1) {
      const ValueType& first_leak_value = suspected_leaks_[0];
      attempted_size = snprintf(buffer, size_remaining, "Suspected %ss: ",
                                first_leak_value.GetTypeName());
      size_remaining -= attempted_size;
      buffer += attempted_size;
    }
    if (size_remaining > 1) {
      // Change this to a comma + space after the first item is printed, so that
      // subsequent items will be separated by a comma.
      const char* optional_comma = "";
      for (const ValueType& leak_value : suspected_leaks_) {
        attempted_size =
            snprintf(buffer, size_remaining, "%s%s",
                     optional_comma,
                     leak_value.ToString(
                         sizeof(to_string_buffer), to_string_buffer));
        size_remaining -= attempted_size;
        buffer += attempted_size;
        optional_comma = ", ";
      }
    }
    if (size_remaining > 1) {
      attempted_size = snprintf(buffer, size_remaining, "\n");
      size_remaining -= attempted_size;
      buffer += attempted_size;
    }
  }

  // Return the number of bytes written, excluding the null terminator.
  return buffer_size - size_remaining;
}

void LeakAnalyzer::AnalyzeDeltas(const RankedList& ranked_deltas) {
  bool found_drop = false;
  RankedList::const_iterator drop_position = ranked_deltas.end();

  if (ranked_deltas.size() > 1) {
    RankedList::const_iterator entry_iter = ranked_deltas.begin();
    RankedList::const_iterator next_entry_iter = ranked_deltas.begin();
    ++next_entry_iter;

    // If the first entry is 0, that means all deltas are 0 or negative. Do
    // not treat this as a suspicion of leaks; just quit.
    if (entry_iter->count > 0) {
      while (next_entry_iter != ranked_deltas.end()) {
        const RankedEntry& entry = *entry_iter;
        const RankedEntry& next_entry = *next_entry_iter;

        // Find the first major drop in values (i.e. by 50% or more).
        if (entry.count > next_entry.count * 2) {
          found_drop = true;
          drop_position = next_entry_iter;
          break;
        }
        ++entry_iter;
        ++next_entry_iter;
      }
    }
  }

  // All leak values before the drop are suspected during this analysis.
  std::set<ValueType,
           std::less<ValueType>,
           Allocator<ValueType>> current_suspects;
  if (found_drop) {
    for (RankedList::const_iterator ranked_list_iter = ranked_deltas.begin();
         ranked_list_iter != drop_position;
         ++ranked_list_iter) {
      current_suspects.insert(ranked_list_iter->value);
    }
  }
  PrintSuspects(current_suspects);

  // Reset the score to 0 for all previously suspected leak values that did
  // not get suspected this time.
  auto iter = suspected_histogram_.begin();
  while (iter != suspected_histogram_.end()) {
    const ValueType& value = iter->first;
    // Erase entries whose suspicion score reaches 0.
    auto erase_iter = iter++;
    if (current_suspects.find(value) == current_suspects.end())
      suspected_histogram_.erase(erase_iter);
  }
  PrintHistogram(suspected_histogram_);

  // For currently suspected values, increase the leak score.
  for (const ValueType& value : current_suspects) {
    auto histogram_iter = suspected_histogram_.find(value);
    if (histogram_iter != suspected_histogram_.end()) {
      histogram_iter->second += kSuspicionScoreIncrease;
    } else if (suspected_histogram_.size() < ranking_size_) {
      // Create a new entry if it didn't already exist.
      suspected_histogram_[value] = kSuspicionScoreIncrease;
    }
  }
  PrintHistogram(suspected_histogram_);

  // Now check the leak suspicion scores. Make sure to erase the suspected
  // leaks from the previous call.
  suspected_leaks_.clear();
  for (const auto& entry : suspected_histogram_) {
    if (suspected_leaks_.size() > ranking_size_)
      break;

    // Only report suspected values that have accumulated a suspicion score.
    // This is achieved by maintaining suspicion for several cycles, with few
    // skips.
    if (entry.second >= score_threshold_)
      suspected_leaks_.emplace_back(entry.first);
  }
}

bool LeakAnalyzer::GetPreviousCountForValue(const ValueType& value,
                                            uint32_t* count) const {
  // Determine what count was recorded for this value last time.
  for (const RankedEntry& entry : prev_ranked_entries_) {
    if (entry.value == value) {
      *count = entry.count;
      return true;
    }
  }
  return false;
}

}  // namespace leak_detector
