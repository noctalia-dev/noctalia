// Locks the atomic set-and-clear contract of ConfigService::mutateOverrides. Switching a calendar
// account to another provider writes the new keys and retires the ones the new provider does not own.
// Doing that as a clear followed by a set publishes an intermediate account that is neither shape and
// fails schema validation, so the mutation must reach the config exactly once.

#include "config/config_service.h"
#include "config/config_types.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_override_mutation: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  const CalendarConfig::Account* findAccount(const Config& config, std::string_view id) {
    const auto it = std::ranges::find(config.calendar.accounts, id, &CalendarConfig::Account::id);
    return it == config.calendar.accounts.end() ? nullptr : &*it;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-override-mutation-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "noctalia" / "config.toml", "\n");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  const std::vector<std::string> typePath{"calendar", "account", "feed", "type"};
  const std::vector<std::string> serverUrlPath{"calendar", "account", "feed", "server_url"};
  const std::vector<std::string> vdirPath{"calendar", "account", "feed", "path"};

  {
    ConfigService config;

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> create;
    create.emplace_back(std::vector<std::string>{"calendar", "enabled"}, true);
    create.emplace_back(typePath, std::string("ics"));
    create.emplace_back(serverUrlPath, std::string("https://example.com/calendar.ics"));
    expect(config.setOverrides(std::move(create)), "ics account writes");
    expect(config.hasOverride(serverUrlPath), "server_url stored for the ics account");

    int reloads = 0;
    config.addReloadCallback([&reloads]() { ++reloads; }, "mutation-test");

    // Switch the account to a local vdir directory. `server_url` is a hard error on a vdir account and
    // `path` is required, so both edits belong to the same commit.
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> switchToVdir;
    switchToVdir.emplace_back(typePath, std::string("vdir"));
    switchToVdir.emplace_back(vdirPath, std::string("/tmp/noctalia-vdir-mutation-test"));
    expect(config.mutateOverrides(switchToVdir, {serverUrlPath}, nullptr), "provider switch writes");

    expect(reloads == 1, "provider switch reaches the config exactly once");
    expect(!config.hasOverride(serverUrlPath), "server_url retired by the switch");
    expect(config.hasOverride(vdirPath), "path stored by the switch");
    expect(config.lastMutationError().empty(), "provider switch produced no mutation error");

    const CalendarConfig::Account* account = findAccount(config.config(), "feed");
    expect(account != nullptr, "account survives the switch");
    if (account != nullptr) {
      expect(account->type == "vdir", "account type is vdir");
      expect(account->path == "/tmp/noctalia-vdir-mutation-test", "account path is set");
      expect(account->serverUrl.empty(), "account server_url is cleared");
    }

    // A mutation that changes nothing must not commit, so consumers are not woken for a no-op.
    expect(config.mutateOverrides({}, {serverUrlPath}, nullptr), "clearing an absent key succeeds");
    expect(reloads == 1, "no-op mutation does not reach the config");
  }

  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::println("config_override_mutation_test passed");
    return 0;
  }
  return 1;
}
