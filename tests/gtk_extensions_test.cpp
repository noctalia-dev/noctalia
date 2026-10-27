#include "tests/test_check.h"
#include "theme/gtk_extensions.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

  using namespace noctalia::theme;

  const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "noctalia_gtk_extensions_test";
  const std::filesystem::path kGtk3 = kRoot / "gtk-3.0";
  const std::filesystem::path kGtk4 = kRoot / "gtk-4.0";

  void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  toml::table manifest() {
    return toml::parse(R"(
      [catalog.thunar]
      name = "Thunar"
      [templates.gtk3]
      input_path = "thunar.css"
      gtk_extension = "gtk3"
      [templates.gtk4]
      input_path = "thunar4.css"
      gtk_extension = "gtk4"
      [templates.hooked]
      input_path = "x.css"
      gtk_extension = "gtk3"
      post_hook = "touch gtk.css"
      [templates.unknown]
      input_path = "x.css"
      gtk_extension = "qt"
      [templates.plain]
      input_path = "plain.conf"
      output_path = "$XDG_CONFIG_HOME/plain/plain.conf"
    )");
  }

  void test_prepare_assigns_owned_outputs_and_drops_invalid_entries() {
    toml::table root = manifest();
    const auto files = prepareGtkExtensions(root, "thunar", GtkToolkits{.gtk3 = true, .gtk4 = true}, kGtk3, kGtk4);

    TEST_CHECK(files.size() == 2);
    TEST_CHECK(
        root["templates"]["gtk3"]["output_path"].value_or(std::string{})
        == (kGtk3 / "noctalia-extensions" / "thunar.gtk3.css").string()
    );
    TEST_CHECK(
        root["templates"]["gtk4"]["output_path"].value_or(std::string{})
        == (kGtk4 / "noctalia-extensions" / "thunar.gtk4.css").string()
    );
    // An extension may not run hooks or pick an unknown toolkit.
    TEST_CHECK(!root["templates"]["hooked"]);
    TEST_CHECK(!root["templates"]["unknown"]);
    // Ordinary entries are untouched.
    TEST_CHECK(
        root["templates"]["plain"]["output_path"].value_or(std::string{}) == "$XDG_CONFIG_HOME/plain/plain.conf"
    );
  }

  void test_prepare_skips_extensions_of_disabled_toolkits() {
    toml::table root = manifest();
    const auto files = prepareGtkExtensions(root, "thunar", GtkToolkits{.gtk3 = false, .gtk4 = true}, kGtk3, kGtk4);

    TEST_CHECK(files.size() == 1);
    TEST_CHECK(files.front().toolkit == GtkToolkit::Gtk4);
    TEST_CHECK(!root["templates"]["gtk3"]);
  }

  void test_index_exists_before_any_extension_renders() {
    std::filesystem::remove_all(kGtk3);
    TEST_CHECK(ensureGtkExtensionIndex(kGtk3));
    TEST_CHECK(readFile(kGtk3 / "noctalia-extensions.css").find("@import") == std::string::npos);
  }

  void test_sync_imports_only_existing_files_and_prunes_stale_ones() {
    std::filesystem::remove_all(kGtk3);
    writeFile(kGtk3 / "noctalia-extensions" / "thunar.gtk3.css", ".thunar {}\n");
    writeFile(kGtk3 / "noctalia-extensions" / "disabled.gtk3.css", ".disabled {}\n");

    TEST_CHECK(syncGtkExtensions(kGtk3, {"thunar.gtk3.css", "missing.gtk3.css"}));

    const std::string index = readFile(kGtk3 / "noctalia-extensions.css");
    TEST_CHECK(index.find("@import url(\"noctalia-extensions/thunar.gtk3.css\");") != std::string::npos);
    TEST_CHECK(index.find("missing") == std::string::npos);
    TEST_CHECK(index.find("disabled") == std::string::npos);
    TEST_CHECK(std::filesystem::exists(kGtk3 / "noctalia-extensions" / "thunar.gtk3.css"));
    TEST_CHECK(!std::filesystem::exists(kGtk3 / "noctalia-extensions" / "disabled.gtk3.css"));

    // Disabling the last extension empties the index and removes its file.
    TEST_CHECK(syncGtkExtensions(kGtk3, {}));
    TEST_CHECK(readFile(kGtk3 / "noctalia-extensions.css").find("@import") == std::string::npos);
    TEST_CHECK(!std::filesystem::exists(kGtk3 / "noctalia-extensions" / "thunar.gtk3.css"));
  }

} // namespace

int main() {
  std::filesystem::remove_all(kRoot);

  test_prepare_assigns_owned_outputs_and_drops_invalid_entries();
  test_prepare_skips_extensions_of_disabled_toolkits();
  test_index_exists_before_any_extension_renders();
  test_sync_imports_only_existing_files_and_prunes_stale_ones();

  std::filesystem::remove_all(kRoot);
  return 0;
}
