#include "i18n/i18n_service.h"
#include "launcher/app_provider.h"
#include "system/desktop_entry.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  const DesktopEntry& findEntry(std::string_view id) {
    const auto& entries = desktopEntries();
    const auto match = std::ranges::find(entries, id, &DesktopEntry::id);
    TEST_CHECK(match != entries.end());
    return *match;
  }

  const LauncherResult& findResult(const std::vector<LauncherResult>& results, std::string_view title) {
    const auto match = std::ranges::find(results, title, &LauncherResult::title);
    TEST_CHECK(match != results.end());
    return *match;
  }

} // namespace

int main(int argc, char* argv[]) {
  namespace fs = std::filesystem;

  TEST_CHECK(argc == 2);
  setenv("NOCTALIA_ASSETS_DIR", argv[1], 1);
  unsetenv("LC_ALL");
  unsetenv("LC_MESSAGES");
  setenv("LANG", "zh_CN.UTF-8", 1);

  const fs::path root = fs::temp_directory_path() / ("noctalia-desktop-entry-locale-" + std::to_string(getpid()));
  const fs::path applications = root / "data/applications";
  fs::create_directories(applications);
  {
    std::ofstream entry(applications / "noctalia-locale-probe.desktop");
    entry
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Disk Locale Probe\n"
        << "Name[en@shaw]=𐑛𐑦𐑕𐑒 Locale Probe\n"
        << "Name[ru]=Диски Locale Probe\n"
        << "Name[pt_BR]=Discos Locale Probe\n"
        << "Name[zh]=磁盘 Locale Probe\n"
        << "Name[zh_CN]=软件\n"
        << "GenericName=Storage Utility\n"
        << "GenericName[ru]=Дисковая утилита\n"
        << "Keywords=disk;storage;\n"
        << "Keywords[ru]=диск;хранилище;\n"
        << "Exec=noctalia-locale-probe\n";
  }

  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  setenv("XDG_DATA_DIRS", (root / "empty").c_str(), 1);

  i18n::Service::instance().init();
  TEST_CHECK(i18n::Service::instance().language() == "zh-Hans");
  TEST_CHECK(i18n::Service::instance().requestedLanguage() == "zh-CN");
  setDesktopEntryLanguage(i18n::Service::instance().requestedLanguage());
  TEST_CHECK(findEntry("noctalia-locale-probe").name == "软件");

  setDesktopEntryLanguage("en");
  TEST_CHECK(findEntry("noctalia-locale-probe").name == "Disk Locale Probe");

  AppProvider provider(nullptr, nullptr);
  provider.initialize();
  const auto translatedSearch = provider.query("Диск");
  TEST_CHECK(findResult(translatedSearch, "Disk Locale Probe").id == (applications / "noctalia-locale-probe.desktop"));

  const std::uint64_t englishVersion = desktopEntriesVersion();
  setDesktopEntryLanguage("ru");
  const DesktopEntry& russian = findEntry("noctalia-locale-probe");
  TEST_CHECK(desktopEntriesVersion() > englishVersion);
  TEST_CHECK(russian.name == "Диски Locale Probe");
  TEST_CHECK(russian.genericName == "Дисковая утилита");
  TEST_CHECK(russian.keywords == "диск;хранилище;");

  setDesktopEntryLanguage("pt-BR");
  TEST_CHECK(findEntry("noctalia-locale-probe").name == "Discos Locale Probe");

  fs::remove_all(root);
  return 0;
}
