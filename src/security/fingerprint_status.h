#pragma once

#include <array>
#include <string>
#include <string_view>

namespace fingerprint {

  // fprintd's EnrollStatus/VerifyStatus results, with the "enroll-"/"verify-" prefix dropped.
  enum class ScanResult {
    StagePassed,
    Completed,
    Match,
    NoMatch,
    RetryScan,
    SwipeTooShort,
    FingerNotCentered,
    RemoveAndRetry,
    Duplicate,
    DataFull,
    Disconnected,
    // enroll-failed, verify-unknown-error and anything newer than this code.
    Failed,
  };

  [[nodiscard]] ScanResult parseScanResult(std::string_view result);

  // A retry asks for another scan of the same finger; the operation goes on.
  [[nodiscard]] bool isRetry(ScanResult result);

  // Fingers offered for enrollment on each hand, in the order the enroll sheet lists them.
  inline constexpr std::array<std::string_view, 3> kLeftHandFingers{
      "left-thumb", "left-index-finger", "left-middle-finger"
  };
  inline constexpr std::array<std::string_view, 3> kRightHandFingers{
      "right-thumb", "right-index-finger", "right-middle-finger"
  };

  // Translation key naming one of fprintd's ten finger names ("left-index-finger" ->
  // "settings.fingerprint.finger.left-index"), or "" for anything else.
  [[nodiscard]] std::string fingerLabelKey(std::string_view finger);

} // namespace fingerprint
