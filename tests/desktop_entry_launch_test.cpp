#include "system/desktop_entry_launch.h"
#include "tests/test_check.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

  void checkArgs(
      const std::optional<desktop_entry_launch::PreparedCommand>& command, const std::vector<std::string>& expected
  ) {
    TEST_CHECK(command.has_value());
    TEST_CHECK(command->args == expected);
  }

  std::string makeExecutableFixture() {
    char path[] = "/tmp/noctalia-terminal-fixture-XXXXXX";
    const int fd = mkstemp(path);
    if (fd >= 0) {
      close(fd);
      chmod(path, 0700);
    }
    return path;
  }

  std::string makeExecutableFixtureNamed(const char* name) {
    char dir[] = "/tmp/noctalia-terminal-fixture-dir-XXXXXX";
    if (mkdtemp(dir) == nullptr) {
      return {};
    }

    std::string path(dir);
    path.push_back('/');
    path.append(name);

    FILE* file = std::fopen(path.c_str(), "w");
    if (file != nullptr) {
      std::fclose(file);
      chmod(path.c_str(), 0700);
    }
    return path;
  }

  std::string dirnameOf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
  }

} // namespace

int main() {
  checkArgs(desktop_entry_launch::prepareCommand("Telegram -- %U", false), {"Telegram", "--"});

  checkArgs(
      desktop_entry_launch::prepareCommand("sample --name %% --file %f --url %U --keep", false),
      {"sample", "--name", "%", "--file", "--url", "--keep"}
  );

  checkArgs(
      desktop_entry_launch::prepareCommand("sample --title \"Hello World\" --single 'Two Words'", false),
      {"sample", "--title", "Hello World", "--single", "Two Words"}
  );

  checkArgs(
      desktop_entry_launch::prepareCommand(R"(/bin/sh -c "\\$SHELL -i -c scrcpy")", false),
      {"/bin/sh", "-c", "$SHELL -i -c scrcpy"}
  );

  checkArgs(
      desktop_entry_launch::prepareCommand("SAL_USE_VCLPLUGIN=gtk3 libreoffice --writer %U", false),
      {"env", "SAL_USE_VCLPLUGIN=gtk3", "libreoffice", "--writer"}
  );

  checkArgs(
      desktop_entry_launch::prepareCommand("FOO=\"a b\" _BAR1=2 sample --opt=value", false),
      {"env", "FOO=a b", "_BAR1=2", "sample", "--opt=value"}
  );

  TEST_CHECK(!desktop_entry_launch::prepareCommand("FOO=bar", false).has_value());

  desktop_entry_launch::PrepareOptions terminalOptions;
  const std::string fakeTerminal = makeExecutableFixture();
  terminalOptions.terminalCandidates = {"missing-terminal-candidate", fakeTerminal};
  const auto fakeTerminalCommand = desktop_entry_launch::prepareCommand("sample --flag", true, terminalOptions);
  std::remove(fakeTerminal.c_str());
  checkArgs(fakeTerminalCommand, {fakeTerminal, "-e", "sh", "-lc", "sample --flag"});

  const char* oldPathRaw = std::getenv("PATH");
  const bool hadOldPath = oldPathRaw != nullptr;
  const std::string oldPath = hadOldPath ? oldPathRaw : std::string{};
  const std::string fakeGnomeTerminal = makeExecutableFixtureNamed("gnome-terminal");
  const std::string fakeTerminalDir = dirnameOf(fakeGnomeTerminal);
  if (!fakeTerminalDir.empty()) {
    setenv("PATH", (fakeTerminalDir + ":" + oldPath).c_str(), 1);
  }
  terminalOptions.terminalCandidates = {"gnome-terminal"};
  const auto gnomeTerminalCommand = desktop_entry_launch::prepareCommand("sample --flag", true, terminalOptions);
  if (hadOldPath) {
    setenv("PATH", oldPath.c_str(), 1);
  } else {
    unsetenv("PATH");
  }
  std::remove(fakeGnomeTerminal.c_str());
  rmdir(fakeTerminalDir.c_str());
  checkArgs(gnomeTerminalCommand, {"gnome-terminal", "--", "sh", "-lc", "sample --flag"});

  desktop_entry_launch::PrepareOptions noTerminalDiscovery;
  noTerminalDiscovery.useSystemTerminalDiscovery = false;
  TEST_CHECK(!desktop_entry_launch::prepareCommand("sample --flag", true, noTerminalDiscovery).has_value());

  TEST_CHECK(!desktop_entry_launch::prepareCommand(" %f %U ", false).has_value());

  return 0;
}
