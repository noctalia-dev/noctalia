#include "system/default_apps.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "default_apps_test: {}", message);
    }
    return condition;
  }

  bool contains(const std::vector<std::string>& values, std::string_view expected) {
    return std::ranges::find(values, std::string(expected)) != values.end();
  }

  bool containsApp(const std::vector<default_apps::App>& apps, std::string_view expectedId) {
    return std::ranges::any_of(apps, [expectedId](const default_apps::App& app) { return app.id == expectedId; });
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return {};
    }
    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(static_cast<std::size_t>(in.tellg()));
    in.seekg(0, std::ios::beg);
    in.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    return contents;
  }

  // True when the [Default Applications] group of a desktop mimeapps.list maps `mime`
  // to `appId`. GLib may append a trailing ';' to the value.
  bool mimeappsListBinds(std::string_view contents, std::string_view mime, std::string_view appId) {
    constexpr std::string_view kGroup = "[Default Applications]";
    const std::size_t groupPos = contents.find(kGroup);
    if (groupPos == std::string_view::npos) {
      return false;
    }
    std::string_view body = contents.substr(groupPos + kGroup.size());
    const std::size_t nextGroup = body.find("\n[");
    if (nextGroup != std::string_view::npos) {
      body = body.substr(0, nextGroup);
    }
    const std::string prefix = std::string(mime) + '=';
    std::size_t start = 0;
    while (start < body.size()) {
      const std::size_t end = body.find('\n', start);
      std::string_view line = body.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ';')) {
        line.remove_suffix(1);
      }
      if (line.size() > prefix.size()
          && line.substr(0, prefix.size()) == prefix
          && line.substr(prefix.size()) == appId) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

  struct ScopedXdg {
    std::filesystem::path root;
    std::optional<std::string> savedDataHome;
    std::optional<std::string> savedConfigHome;
    std::optional<std::string> savedDataDirs;
    std::optional<std::string> savedCurrentDesktop;

    ScopedXdg(const std::filesystem::path& tempRoot) : root(tempRoot) {
      namespace fs = std::filesystem;
      fs::create_directories(root / "data" / "applications");
      fs::create_directories(root / "config");
      fs::create_directories(root / "system");

      const char* dataHome = std::getenv("XDG_DATA_HOME");
      const char* configHome = std::getenv("XDG_CONFIG_HOME");
      const char* dataDirs = std::getenv("XDG_DATA_DIRS");
      const char* currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
      if (dataHome != nullptr) {
        savedDataHome = dataHome;
      }
      if (configHome != nullptr) {
        savedConfigHome = configHome;
      }
      if (dataDirs != nullptr) {
        savedDataDirs = dataDirs;
      }
      if (currentDesktop != nullptr) {
        savedCurrentDesktop = currentDesktop;
      }
      // Isolate every XDG lookup so setDefault/currentDefault round-trips never
      // touch the real user's mimeapps.list. XDG_DATA_DIRS is pointed at an
      // empty dir so g_app_info_get_all() finds only the fixture apps.
      ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
      ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
      ::setenv("XDG_DATA_DIRS", (root / "system").c_str(), 1);
      // A desktop name makes GIO read config/sway-mimeapps.list before the single
      // config/mimeapps.list, so the per-desktop override path is exercised.
      ::setenv("XDG_CURRENT_DESKTOP", "sway", 1);
    }

    ~ScopedXdg() {
      if (savedDataHome.has_value()) {
        ::setenv("XDG_DATA_HOME", savedDataHome->c_str(), 1);
      } else {
        ::unsetenv("XDG_DATA_HOME");
      }
      if (savedConfigHome.has_value()) {
        ::setenv("XDG_CONFIG_HOME", savedConfigHome->c_str(), 1);
      } else {
        ::unsetenv("XDG_CONFIG_HOME");
      }
      if (savedDataDirs.has_value()) {
        ::setenv("XDG_DATA_DIRS", savedDataDirs->c_str(), 1);
      } else {
        ::unsetenv("XDG_DATA_DIRS");
      }
      if (savedCurrentDesktop.has_value()) {
        ::setenv("XDG_CURRENT_DESKTOP", savedCurrentDesktop->c_str(), 1);
      } else {
        ::unsetenv("XDG_CURRENT_DESKTOP");
      }
      std::filesystem::remove_all(root);
    }

    ScopedXdg(const ScopedXdg&) = delete;
    ScopedXdg& operator=(const ScopedXdg&) = delete;
  };

  // The fixtures use "Exec=true" (an executable that is always on PATH) rather
  // than a fake program name: GIO hides desktop entries whose Exec program it
  // cannot find, so a fake name would make the fixtures invisible to
  // g_app_info_get_all() and unresolvable for setDefault().
  //
  // Install a fake .desktop file without MimeType so GIO does not claim it via
  // MIME association; categoryFallback via desktopEntries() picks it up instead.
  void installFakeBrowserXdg(const ScopedXdg& xdg) {
    std::ofstream entry(xdg.root / "data" / "applications" / "noctalia-test-browser.desktop");
    entry
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Noctalia Test Browser\n"
        << "Exec=true\n"
        << "Icon=noctalia-test-browser\n"
        << "Categories=webbrowser;\n";
  }

  // Install a fake .desktop file WITH a MimeType key so the primary MIME+category
  // candidate path is exercised.
  void installFakePhotosXdg(const ScopedXdg& xdg) {
    std::ofstream entry(xdg.root / "data" / "applications" / "noctalia-test-photos.desktop");
    entry
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Noctalia Test Photos\n"
        << "Exec=true\n"
        << "Icon=noctalia-test-photos\n"
        << "Categories=Graphics;\n"
        << "MimeType=image/jpeg;image/png;\n";
  }

  // Install a fake .desktop file in an applications subdirectory so the GIO-style
  // canonical id ("graphics-noctalia-test-subdir.desktop") is exercised.
  void installSubdirPdfXdg(const ScopedXdg& xdg) {
    const std::filesystem::path dir = xdg.root / "data" / "applications" / "graphics";
    std::filesystem::create_directories(dir);
    std::ofstream entry(dir / "noctalia-test-subdir.desktop");
    entry
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Noctalia Test PDF\n"
        << "Exec=true\n"
        << "Icon=noctalia-test-subdir\n"
        << "Categories=Viewer;\n"
        << "MimeType=application/pdf;\n";
  }

  void installFakeHiddenPdfXdg(const ScopedXdg& xdg) {
    std::ofstream entry(xdg.root / "data" / "applications" / "noctalia-test-hidden.desktop");
    entry
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Noctalia Test Hidden PDF\n"
        << "Exec=true\n"
        << "Icon=noctalia-test-hidden\n"
        << "Categories=Viewer;\n"
        << "MimeType=application/pdf;\n"
        << "NoDisplay=true\n";
  }

  void installFakeBrowserAudioXdg(const ScopedXdg& xdg) {
    std::ofstream entry(xdg.root / "data" / "applications" / "noctalia-test-browser-audio.desktop");
    entry
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Noctalia Test Browser Audio\n"
        << "Exec=true\n"
        << "Icon=noctalia-test-browser-audio\n"
        << "Categories=Network;WebBrowser;\n"
        << "MimeType=audio/ogg;\n";
  }

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("noctalia-default-apps-test-" + std::to_string(getpid()));
  // ScopedXdg must be constructed before ANY GLib call so g_get_user_data_dir /
  // g_get_user_config_dir cache the temp paths on first access.
  ScopedXdg xdg(root);

  // Seed the XDG data dir with fake entries. The MimeType-less browser exercises
  // categoryFallback (its web category, not a MIME match) and the canonical-desktop-id
  // path; the photos and subdir PDF entries exercise the primary MIME+category gate.
  installFakeBrowserXdg(xdg);
  installFakePhotosXdg(xdg);
  installSubdirPdfXdg(xdg);
  installFakeHiddenPdfXdg(xdg);
  installFakeBrowserAudioXdg(xdg);

  // Seed a per-desktop mimeapps.list before the first GIO query, so a read that still
  // consults it would resolve this fixture. Noctalia owns only the single
  // config/mimeapps.list and must ignore it entirely.
  const std::filesystem::path singleFile = xdg.root / "config" / "mimeapps.list";
  const std::filesystem::path shadowFile = xdg.root / "config" / "sway-mimeapps.list";
  {
    std::ofstream shadow(shadowFile);
    shadow << "[Default Applications]\n" << "application/pdf=graphics-noctalia-test-subdir.desktop\n";
  }
  const std::string shadowBefore = readFile(shadowFile);

  bool ok = true;

  // --- mimeTypes() invariants (pure logic, no GLib) ---

  ok = expect(
           !default_apps::mimeTypes(default_apps::Role::WebBrowser).empty()
               && !default_apps::mimeTypes(default_apps::Role::DocumentViewer).empty()
               && !default_apps::mimeTypes(default_apps::Role::TextEditor).empty()
               && !default_apps::mimeTypes(default_apps::Role::MailClient).empty()
               && !default_apps::mimeTypes(default_apps::Role::MusicPlayer).empty()
               && !default_apps::mimeTypes(default_apps::Role::VideoPlayer).empty()
               && !default_apps::mimeTypes(default_apps::Role::PhotoViewer).empty(),
           "every role should be bound to at least one mime/content type"
       )
      && ok;

  const auto& browser = default_apps::mimeTypes(default_apps::Role::WebBrowser);
  ok = expect(
           contains(browser, "x-scheme-handler/http") && contains(browser, "x-scheme-handler/https"),
           "the web browser role should cover http and https schemes"
       )
      && ok;

  const auto& viewer = default_apps::mimeTypes(default_apps::Role::DocumentViewer);
  ok = expect(viewer == std::vector<std::string>{"application/pdf"}, "the document viewer role should target PDFs")
      && ok;

  const auto& editor = default_apps::mimeTypes(default_apps::Role::TextEditor);
  ok = expect(editor == std::vector<std::string>{"text/plain"}, "the text editor role should target plain text") && ok;

  const auto& mail = default_apps::mimeTypes(default_apps::Role::MailClient);
  ok = expect(mail == std::vector<std::string>{"x-scheme-handler/mailto"}, "the email client role should target mailto")
      && ok;

  const auto& music = default_apps::mimeTypes(default_apps::Role::MusicPlayer);
  ok = expect(
           contains(music, "audio/mpeg")
               && contains(music, "audio/flac")
               && contains(music, "audio/x-wav")
               && contains(music, "audio/ogg"),
           "the music player role should cover common audio types"
       )
      && ok;

  const auto& video = default_apps::mimeTypes(default_apps::Role::VideoPlayer);
  ok = expect(
           contains(video, "video/mp4") && contains(video, "video/x-matroska") && contains(video, "video/webm"),
           "the video player role should cover common video types"
       )
      && ok;

  const auto& photo = default_apps::mimeTypes(default_apps::Role::PhotoViewer);
  ok = expect(
           contains(photo, "image/jpeg")
               && contains(photo, "image/png")
               && contains(photo, "image/gif")
               && contains(photo, "image/webp"),
           "the photo viewer role should cover common image types"
       )
      && ok;

  // --- candidates() invariants ---

  for (const auto role : {
           default_apps::Role::WebBrowser,
           default_apps::Role::DocumentViewer,
           default_apps::Role::TextEditor,
           default_apps::Role::MailClient,
           default_apps::Role::MusicPlayer,
           default_apps::Role::VideoPlayer,
           default_apps::Role::PhotoViewer,
       }) {
    const auto apps = default_apps::candidates(role);
    for (const auto& app : apps) {
      ok =
          expect(!app.id.empty() && !app.name.empty(), "candidates should carry a non-empty id and display name") && ok;
    }
    for (std::size_t i = 0; i < apps.size(); ++i) {
      for (std::size_t j = i + 1; j < apps.size(); ++j) {
        ok = expect(apps[i].id != apps[j].id, "candidates should be deduplicated by id") && ok;
      }
    }
  }

  // The MimeType-less browser fixture is discovered through the category fallback
  // path: no fixture declares a web scheme, so no entry passes the MIME gate and
  // the fallback is always exercised in the isolated environment.
  const auto browserCandidates = default_apps::candidates(default_apps::Role::WebBrowser);
  ok = expect(
           containsApp(browserCandidates, "noctalia-test-browser.desktop"),
           "the browser desktop entry should be found via the category fallback"
       )
      && ok;

  // The MimeType-bearing photo fixture surfaces as a candidate through the primary
  // MIME+category gate (image/jpeg with a Graphics category).
  const auto photoCandidates = default_apps::candidates(default_apps::Role::PhotoViewer);
  ok = expect(
           containsApp(photoCandidates, "noctalia-test-photos.desktop"),
           "the photos desktop entry should be found via its MimeType association"
       )
      && ok;

  // The subdirectory PDF fixture reports the GIO-style id with its directory
  // folded in, so picker options and default lookups agree.
  const auto pdfCandidates = default_apps::candidates(default_apps::Role::DocumentViewer);
  ok = expect(
           containsApp(pdfCandidates, "graphics-noctalia-test-subdir.desktop"),
           "an applications-subdirectory entry should use the GIO-style canonical id"
       )
      && ok;

  ok = expect(
           !containsApp(pdfCandidates, "noctalia-test-hidden.desktop"),
           "a NoDisplay entry declaring the role mime should not be a candidate"
       )
      && ok;

  const auto musicCandidates = default_apps::candidates(default_apps::Role::MusicPlayer);
  ok = expect(
           !containsApp(musicCandidates, "noctalia-test-browser-audio.desktop"),
           "a Network/WebBrowser entry should not be a music player candidate even when it declares an audio mime"
       )
      && ok;

  // --- setDefault / currentDefault round-trip ---

  ok = expect(!default_apps::setDefault(default_apps::Role::WebBrowser, ""), "setDefault should reject empty id") && ok;

  ok = expect(
           !default_apps::setDefault(default_apps::Role::WebBrowser, "this.desktop.does.not.exist.desktop"),
           "setDefault should return false for a non-existent desktop file id"
       )
      && ok;

  // Round-trips are driven by the fixture apps because their ids always resolve
  // in the isolated XDG directories. Host apps surfaced by the category fallback
  // are not guaranteed to resolve here (GIO only sees the fixture apps), so they
  // are not suitable default targets for the isolated test.
  constexpr std::pair<default_apps::Role, std::string_view> kFixtureDefaults[] = {
      {default_apps::Role::WebBrowser, "noctalia-test-browser.desktop"},
      {default_apps::Role::DocumentViewer, "graphics-noctalia-test-subdir.desktop"},
      {default_apps::Role::PhotoViewer, "noctalia-test-photos.desktop"},
  };
  // Before the first write the single file does not exist, so no role has a default even
  // though the per-desktop file names one (documented above).
  ok = expect(
           !std::filesystem::exists(singleFile), "the single mimeapps.list should not exist before the first setDefault"
       )
      && ok;
  for (const auto& entry : kFixtureDefaults) {
    ok = expect(
             !default_apps::currentDefault(entry.first).has_value(),
             "currentDefault should ignore the per-desktop mimeapps.list"
         )
        && ok;
  }

  for (const auto& [role, fixtureId] : kFixtureDefaults) {
    ok = expect(default_apps::setDefault(role, fixtureId), "setDefault should succeed for a fixture candidate") && ok;
    ok = expect(std::filesystem::exists(singleFile), "setDefault should create the single mimeapps.list on first write")
        && ok;

    const auto currentId = default_apps::currentDefault(role);
    ok = expect(currentId.has_value(), "currentDefault should return a value after setDefault") && ok;
    if (currentId.has_value()) {
      ok = expect(*currentId == fixtureId, "currentDefault should read the single mimeapps.list") && ok;
    }

    // Read back the on-disk effect so the test fails if setDefault ever stops writing,
    // even if the GIO round-trip above still resolves.
    const std::string mimeapps = readFile(singleFile);
    for (const auto& mime : default_apps::mimeTypes(role)) {
      ok = expect(
               mimeappsListBinds(mimeapps, mime, fixtureId),
               "setDefault should write the association to the single mimeapps.list"
           )
          && ok;
    }
  }

  ok = expect(readFile(shadowFile) == shadowBefore, "setDefault should not modify the per-desktop mimeapps.list") && ok;

  return ok ? 0 : 1;
}
