#include "security/fingerprint_status.h"

#include <utility>

namespace fingerprint {

  ScanResult parseScanResult(std::string_view result) {
    for (const std::string_view prefix : {"enroll-", "verify-"}) {
      if (result.starts_with(prefix)) {
        result.remove_prefix(prefix.size());
        break;
      }
    }
    static constexpr std::pair<std::string_view, ScanResult> kResults[] = {
        {"stage-passed", ScanResult::StagePassed},
        {"completed", ScanResult::Completed},
        {"match", ScanResult::Match},
        {"no-match", ScanResult::NoMatch},
        {"retry-scan", ScanResult::RetryScan},
        {"swipe-too-short", ScanResult::SwipeTooShort},
        {"finger-not-centered", ScanResult::FingerNotCentered},
        {"remove-and-retry", ScanResult::RemoveAndRetry},
        {"duplicate", ScanResult::Duplicate},
        {"data-full", ScanResult::DataFull},
        {"disconnected", ScanResult::Disconnected},
    };
    for (const auto& [name, value] : kResults) {
      if (result == name) {
        return value;
      }
    }
    return ScanResult::Failed;
  }

  bool isRetry(ScanResult result) {
    return result == ScanResult::RetryScan
        || result == ScanResult::SwipeTooShort
        || result == ScanResult::FingerNotCentered
        || result == ScanResult::RemoveAndRetry;
  }

  std::string fingerLabelKey(std::string_view finger) {
    std::string_view hand;
    for (const std::string_view side : {"left-", "right-"}) {
      if (finger.starts_with(side)) {
        hand = side;
        finger.remove_prefix(side.size());
        break;
      }
    }
    if (hand.empty()) {
      return {};
    }
    if (finger != "thumb") {
      if (!finger.ends_with("-finger")) {
        return {};
      }
      finger.remove_suffix(std::string_view("-finger").size());
      if (finger != "index" && finger != "middle" && finger != "ring" && finger != "little") {
        return {};
      }
    }
    return std::string("settings.fingerprint.finger.").append(hand).append(finger);
  }

} // namespace fingerprint
