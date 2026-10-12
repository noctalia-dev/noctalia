#include "core/process/process.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "tests/test_check.h"

#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main(const int argc, char* argv[]) {
  using greeter::detail::ApplyHelperProtocol;
  using greeter::detail::classifyApplyHelperProtocol;

  process::RunResult secure;
  secure.exitCode = 0;
  secure.out = "secure-sync-v1";
  TEST_CHECK(classifyApplyHelperProtocol(secure) == ApplyHelperProtocol::SecureSyncV1);

  process::RunResult legacy;
  legacy.exitCode = 2;
  legacy.err = R"(usage: /usr/bin/noctalia-greeter-apply-appearance <staging-directory>
       /usr/bin/noctalia-greeter-apply-appearance --setup-system
       /usr/bin/noctalia-greeter-apply-appearance --print-greeter-user
)";
  TEST_CHECK(classifyApplyHelperProtocol(legacy) == ApplyHelperProtocol::Legacy);

  process::RunResult currentUsage = legacy;
  currentUsage.err += "       /usr/bin/noctalia-greeter-apply-appearance --sync <staging-directory>\n";
  TEST_CHECK(classifyApplyHelperProtocol(currentUsage) == ApplyHelperProtocol::Unknown);

  process::RunResult malformed = secure;
  malformed.out = "secure-sync-v1 trailing-data";
  TEST_CHECK(classifyApplyHelperProtocol(malformed) == ApplyHelperProtocol::Unknown);

  process::RunResult futureCapability = secure;
  futureCapability.out = "secure-sync-v2";
  TEST_CHECK(classifyApplyHelperProtocol(futureCapability) == ApplyHelperProtocol::Unknown);

  process::RunResult versionOnly = secure;
  versionOnly.out = "1.4.0";
  TEST_CHECK(classifyApplyHelperProtocol(versionOnly) == ApplyHelperProtocol::Unknown);

  process::RunResult padded = secure;
  padded.out = " secure-sync-v1";
  TEST_CHECK(classifyApplyHelperProtocol(padded) == ApplyHelperProtocol::Unknown);

  process::RunResult extraLine = secure;
  extraLine.out = "secure-sync-v1\nunexpected";
  TEST_CHECK(classifyApplyHelperProtocol(extraLine) == ApplyHelperProtocol::Unknown);

  process::RunResult warning = secure;
  warning.err = "unexpected warning";
  TEST_CHECK(classifyApplyHelperProtocol(warning) == ApplyHelperProtocol::Unknown);

  process::RunResult failed = legacy;
  failed.exitCode = 1;
  TEST_CHECK(classifyApplyHelperProtocol(failed) == ApplyHelperProtocol::Unknown);

  process::RunResult missing;
  missing.exitCode = 127;
  TEST_CHECK(classifyApplyHelperProtocol(missing) == ApplyHelperProtocol::Unknown);

  process::RunResult timedOut = legacy;
  timedOut.timedOut = true;
  TEST_CHECK(classifyApplyHelperProtocol(timedOut) == ApplyHelperProtocol::Unknown);

  process::RunResult truncatedOut = legacy;
  truncatedOut.outTruncated = true;
  TEST_CHECK(classifyApplyHelperProtocol(truncatedOut) == ApplyHelperProtocol::Unknown);

  process::RunResult truncatedErr = legacy;
  truncatedErr.errTruncated = true;
  TEST_CHECK(classifyApplyHelperProtocol(truncatedErr) == ApplyHelperProtocol::Unknown);

  if (argc != 1) {
    TEST_CHECK(argc == 3);
    const std::string_view expectedName(argv[2]);
    TEST_CHECK(expectedName == "legacy" || expectedName == "secure");
    const ApplyHelperProtocol expected =
        expectedName == "legacy" ? ApplyHelperProtocol::Legacy : ApplyHelperProtocol::SecureSyncV1;

    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 16U * 1024U;
    const process::RunResult actual =
        process::runSync(std::vector<std::string>{argv[1], "--supports", "secure-sync-v1"}, std::move(options));
    if (classifyApplyHelperProtocol(actual) != expected) {
      std::println(
          stderr,
          "actual helper probe: exit={} timeout={:d} stdout-truncated={:d} stderr-truncated={:d} stdout=<{}> "
          "stderr=<{}>",
          actual.exitCode, actual.timedOut, actual.outTruncated, actual.errTruncated, actual.out, actual.err
      );
    }
    TEST_CHECK(classifyApplyHelperProtocol(actual) == expected);
  }

  return 0;
}
