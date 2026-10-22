#include "capture/screenshot_service.h"
#include "cli/schema_msg.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <optional>
#include <string_view>

namespace {

  using Options = ScreenshotService::OutputOptions;
  namespace msg = noctalia::cli::msg;

  Options baseOptions() {
    Options options;
    options.annotate = false;
    options.copyToClipboard = true;
    options.closeOnCopy = true;
    return options;
  }

  auto parse(
      const noctalia::cli::Command& command, std::string_view args,
      std::optional<std::string_view> callerCwd = std::nullopt
  ) {
    return parseScreenshotIpcRequest(command, args, baseOptions(), callerCwd);
  }

  void checkNoArgsKeepsConfig() {
    for (const noctalia::cli::Command* command :
         {&msg::screenshotRegion, &msg::screenshotFullscreen, &msg::screenshotWindow, &msg::screenshotAnnotate,
          &msg::annotate}) {
      const auto request = parse(*command, "");
      TEST_CHECK(request.has_value());
      TEST_CHECK(request->options == baseOptions());
      TEST_CHECK(request->target.empty());
    }
  }

  void checkBooleanOverrides() {
    const auto request = parse(msg::screenshotRegion, "--annotate --no-copy-to-clipboard --no-close-on-copy");
    TEST_CHECK(request.has_value());
    Options expected = baseOptions();
    expected.annotate = true;
    expected.copyToClipboard = false;
    expected.closeOnCopy = false;
    TEST_CHECK(request->options == expected);
  }

  void checkErrors() {
    const auto conflict = parse(msg::screenshotRegion, "--annotate --no-annotate");
    TEST_CHECK(!conflict.has_value());
    TEST_CHECK(conflict.error() == "error: --annotate and --no-annotate cannot be combined\n");

    const auto unknown = parse(msg::screenshotRegion, "--bogus");
    TEST_CHECK(!unknown.has_value());
    TEST_CHECK(unknown.error().starts_with("error: unknown argument: --bogus"));

    const auto extra = parse(msg::screenshotRegion, "extra");
    TEST_CHECK(!extra.has_value());
    TEST_CHECK(extra.error() == "error: unexpected argument: extra\n");

    const auto unterminated = parse(msg::screenshotRegion, "'open");
    TEST_CHECK(!unterminated.has_value());
    TEST_CHECK(unterminated.error() == "error: unterminated quote in arguments\n");
  }

  void checkValues() {
    const auto pipe = parse(msg::screenshotRegion, "--pipe-command 'satty -f -' --pipe-to-command");
    TEST_CHECK(pipe.has_value());
    TEST_CHECK(pipe->options.pipeCommand == "satty -f -");
    TEST_CHECK(pipe->options.pipeToCommand);

    const auto directory = parse(msg::screenshotRegion, "--directory shots", "/tmp/a");
    TEST_CHECK(directory.has_value());
    TEST_CHECK(directory->options.directory == "/tmp/a/shots");
  }

  void checkTargets() {
    const auto description = parse(msg::screenshotFullscreen, "--show-cursor Dell U2720Q");
    TEST_CHECK(description.has_value());
    TEST_CHECK(description->target == "Dell U2720Q");
    TEST_CHECK(description->options.showCursor);

    const auto connector = parse(msg::screenshotFullscreen, "DP-2");
    TEST_CHECK(connector.has_value());
    TEST_CHECK(connector->target == "DP-2");

    const auto path = parse(msg::annotate, "~/My Pictures/x.png");
    TEST_CHECK(path.has_value());
    TEST_CHECK(path->target == "~/My Pictures/x.png");
  }

} // namespace

int main() {
  checkNoArgsKeepsConfig();
  checkBooleanOverrides();
  checkErrors();
  checkValues();
  checkTargets();
  return EXIT_SUCCESS;
}
