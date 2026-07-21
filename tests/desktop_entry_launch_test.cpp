#include "system/desktop_entry_launch.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "desktop_entry_launch_test: %s\n", message);
    }
    return condition;
  }

  bool expectArgs(
      const std::optional<desktop_entry_launch::PreparedCommand>& command, const std::vector<std::string>& expected,
      const char* message
  ) {
    if (!expect(command.has_value(), message)) {
      return false;
    }
    if (command->args == expected) {
      return true;
    }

    std::fprintf(stderr, "desktop_entry_launch_test: %s\n", message);
    std::fprintf(stderr, "  expected:");
    for (const auto& arg : expected) {
      std::fprintf(stderr, " [%s]", arg.c_str());
    }
    std::fprintf(stderr, "\n  actual:");
    for (const auto& arg : command->args) {
      std::fprintf(stderr, " [%s]", arg.c_str());
    }
    std::fprintf(stderr, "\n");
    return false;
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
  bool ok = true;

  ok = expectArgs(
           desktop_entry_launch::prepareCommand("sample --name %% --file %f --url %U --keep", false),
           {"sample", "--name", "%", "--file", "--url", "--keep"}, "field codes should be removed"
       )
      && ok;

  ok = expectArgs(
           desktop_entry_launch::prepareCommand("sample --title \"Hello World\" --single 'Two Words'", false),
           {"sample", "--title", "Hello World", "--single", "Two Words"}, "quoted arguments should stay together"
       )
      && ok;

  ok = expectArgs(
           desktop_entry_launch::prepareCommand(R"(/bin/sh -c "\\$SHELL -i -c scrcpy")", false),
           {"/bin/sh", "-c", "$SHELL -i -c scrcpy"},
           "desktop-entry escaping should preserve a shell variable in a quoted argument"
       )
      && ok;

  desktop_entry_launch::PrepareOptions terminalOptions;
  const std::string fakeTerminal = makeExecutableFixture();
  terminalOptions.terminalCandidates = {"missing-terminal-candidate", fakeTerminal};
  ok = expectArgs(
           desktop_entry_launch::prepareCommand("sample --flag", true, terminalOptions),
           {fakeTerminal, "-e", "sh", "-lc", "sample --flag"},
           "terminal candidates should use the first executable candidate"
       )
      && ok;
  std::remove(fakeTerminal.c_str());

  const char* oldPathRaw = std::getenv("PATH");
  const bool hadOldPath = oldPathRaw != nullptr;
  const std::string oldPath = hadOldPath ? oldPathRaw : std::string{};
  const std::string fakeGnomeTerminal = makeExecutableFixtureNamed("gnome-terminal");
  const std::string fakeTerminalDir = dirnameOf(fakeGnomeTerminal);
  if (!fakeTerminalDir.empty()) {
    setenv("PATH", (fakeTerminalDir + ":" + oldPath).c_str(), 1);
  }
  terminalOptions.terminalCandidates = {"gnome-terminal"};
  ok = expectArgs(
           desktop_entry_launch::prepareCommand("sample --flag", true, terminalOptions),
           {"gnome-terminal", "--", "sh", "-lc", "sample --flag"},
           "gnome-style terminals should use -- before the shell command"
       )
      && ok;
  if (hadOldPath) {
    setenv("PATH", oldPath.c_str(), 1);
  } else {
    unsetenv("PATH");
  }
  std::remove(fakeGnomeTerminal.c_str());
  rmdir(fakeTerminalDir.c_str());

  desktop_entry_launch::PrepareOptions noTerminalDiscovery;
  noTerminalDiscovery.useSystemTerminalDiscovery = false;
  ok = expect(
           !desktop_entry_launch::prepareCommand("sample --flag", true, noTerminalDiscovery).has_value(),
           "terminal preparation should fail when discovery is disabled and no candidates are provided"
       )
      && ok;

  ok = expect(
           !desktop_entry_launch::prepareCommand(" %f %U ", false).has_value(),
           "field-code-only command should not prepare an empty argv"
       )
      && ok;

  return ok ? 0 : 1;
}
