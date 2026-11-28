#include "security/fingerprint_status.h"
#include "tests/test_check.h"

#include <string>

using fingerprint::fingerLabelKey;
using fingerprint::isRetry;
using fingerprint::parseScanResult;
using fingerprint::ScanResult;

int main() {
  TEST_CHECK(parseScanResult("enroll-stage-passed") == ScanResult::StagePassed);
  TEST_CHECK(parseScanResult("enroll-completed") == ScanResult::Completed);
  TEST_CHECK(parseScanResult("enroll-duplicate") == ScanResult::Duplicate);
  TEST_CHECK(parseScanResult("enroll-data-full") == ScanResult::DataFull);
  TEST_CHECK(parseScanResult("enroll-failed") == ScanResult::Failed);
  TEST_CHECK(parseScanResult("verify-match") == ScanResult::Match);
  TEST_CHECK(parseScanResult("verify-no-match") == ScanResult::NoMatch);
  TEST_CHECK(parseScanResult("verify-disconnected") == ScanResult::Disconnected);
  TEST_CHECK(parseScanResult("verify-unknown-error") == ScanResult::Failed);
  TEST_CHECK(parseScanResult("something-new") == ScanResult::Failed);
  TEST_CHECK(parseScanResult("") == ScanResult::Failed);

  // The retry results are shared by enroll and verify.
  for (const char* prefix : {"enroll-", "verify-"}) {
    const std::string p(prefix);
    TEST_CHECK(parseScanResult(p + "retry-scan") == ScanResult::RetryScan);
    TEST_CHECK(parseScanResult(p + "swipe-too-short") == ScanResult::SwipeTooShort);
    TEST_CHECK(parseScanResult(p + "finger-not-centered") == ScanResult::FingerNotCentered);
    TEST_CHECK(parseScanResult(p + "remove-and-retry") == ScanResult::RemoveAndRetry);
  }
  TEST_CHECK(isRetry(ScanResult::RetryScan));
  TEST_CHECK(isRetry(ScanResult::RemoveAndRetry));
  TEST_CHECK(!isRetry(ScanResult::StagePassed));
  TEST_CHECK(!isRetry(ScanResult::NoMatch));

  TEST_CHECK(fingerLabelKey("left-thumb") == "settings.fingerprint.finger.left-thumb");
  TEST_CHECK(fingerLabelKey("left-index-finger") == "settings.fingerprint.finger.left-index");
  TEST_CHECK(fingerLabelKey("right-little-finger") == "settings.fingerprint.finger.right-little");
  TEST_CHECK(fingerLabelKey("right-ring-finger") == "settings.fingerprint.finger.right-ring");
  TEST_CHECK(fingerLabelKey("right-thumb-finger").empty());
  TEST_CHECK(fingerLabelKey("left-pinky-finger").empty());
  TEST_CHECK(fingerLabelKey("index-finger").empty());
  TEST_CHECK(fingerLabelKey("any").empty());
  TEST_CHECK(fingerLabelKey("").empty());

  for (const auto finger : fingerprint::kLeftHandFingers) {
    TEST_CHECK(!fingerLabelKey(finger).empty());
  }
  for (const auto finger : fingerprint::kRightHandFingers) {
    TEST_CHECK(!fingerLabelKey(finger).empty());
  }
  return 0;
}
