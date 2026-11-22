#include "config/schema/greeter_schema.h"
#include "render/codecs/codecs.h"
#include "render/codecs/image_encoder.h"
#include "shell/greeter/greeter_customization_staging.h"
#include "tests/test_check.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string format(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{table};
    return out.str();
  }
  void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    out.close();
    TEST_CHECK(!out.fail());
  }
  GreeterProfile profileFor(const std::filesystem::path& image) {
    GreeterProfile profile;
    profile.layout.emplace();
    profile.layout->loginMode = GreeterLoginMode::Minimal;
    profile.layout->components["login"].width = 420;
    profile.layout->components["branding"].flipX = false;
    profile.widgets.emplace();
    profile.widgets->instances.emplace();
    for (const auto* type : {"label", "clock", "sticker", "sysmon", "button"}) {
      GreeterWidgetConfig widget;
      widget.id = std::string(type) + "_one";
      widget.type = type;
      widget.placement = GreeterAnchorPlacement{GreeterAnchor::BottomLeft, 8, -12};
      if (widget.type == "sticker") {
        widget.settings["image_path"] = image.string();
      }
      profile.widgets->instances->push_back(widget);
    }
    auto duplicate = profile.widgets->instances->at(2);
    duplicate.id = "disabled_image";
    duplicate.enabled = false;
    duplicate.rotation = 0.25;
    duplicate.flipY = true;
    profile.widgets->instances->push_back(duplicate);
    return profile;
  }
} // namespace

int main(int argc, char* argv[]) {
  char pattern[] = "/tmp/noctalia-customization-staging-XXXXXX";
  const auto* directory = ::mkdtemp(pattern);
  TEST_CHECK(directory);
  const std::filesystem::path root(directory), staging = root / "staging";
  std::filesystem::create_directory(staging);
  const auto imagePath = root / "source.png";
  const std::array<std::uint8_t, 4> rgba{128, 32, 64, 255};
  const auto png = encodePng(rgba.data(), 1, 1);
  write(imagePath, {reinterpret_cast<const char*>(png.data()), png.size()});
  auto profile = profileFor(imagePath);
  const auto original = format(noctalia::config::schema::writeGreeterConfig(profile));
  auto staged = greeter::detail::stageCustomization(staging, profile);
  TEST_CHECK(staged && format(noctalia::config::schema::writeGreeterConfig(profile)) == original);
  TEST_CHECK(staged->contains("layout") && staged->contains("widgets"));
  const auto* images = (*staged)["assets"]["images"].as_array();
  TEST_CHECK(images && images->size() == 1);
  const auto basename = images->front().value<std::string>();
  TEST_CHECK(basename && basename->starts_with("widget-") && basename->size() == 75);
  const auto managedPath = "widget-assets/" + basename->substr(7);
  TEST_CHECK((*staged)["widgets"]["widget"]["sticker_one"]["settings"]["image_path"] == managedPath);
  TEST_CHECK((*staged)["widgets"]["widget"]["disabled_image"]["settings"]["image_path"] == managedPath);
  gchar* hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, png.data(), png.size());
  TEST_CHECK(*basename == std::string("widget-") + hash + ".png");
  g_free(hash);
  struct stat st{};
  TEST_CHECK(::stat((staging / *basename).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  TEST_CHECK(std::distance(std::filesystem::directory_iterator(staging), std::filesystem::directory_iterator{}) == 1);

  // This export can be consumed by the real greeter helper's private fixture.
  if (argc == 2) {
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    ::chmod(output.c_str(), 0700);
    std::filesystem::copy_file(staging / *basename, output / *basename);
    ::chmod((output / *basename).c_str(), 0600);
    toml::table appearance{{"scheme", "Synced"}, {"theme_mode", "dark"}};
    toml::table palette;
    for (const auto* key :
         {"primary", "on_primary", "secondary", "on_secondary", "tertiary", "on_tertiary", "error", "on_error",
          "surface", "on_surface", "surface_variant", "on_surface_variant", "outline", "shadow", "hover", "on_hover"}) {
      palette.insert(key, "#123456");
    }
    appearance.insert("palette", std::move(palette));
    staged->insert("appearance", std::move(appearance));
    write(output / "sync.toml", format(*staged));
    ::chmod((output / "sync.toml").c_str(), 0600);
  }
  std::filesystem::remove_all(staging);
  std::filesystem::create_directory(staging);
  auto omitted = greeter::detail::stageCustomization(staging, {});
  TEST_CHECK(omitted && omitted->empty());
  GreeterProfile reset;
  reset.layout.emplace();
  reset.widgets.emplace();
  auto resetTables = greeter::detail::stageCustomization(staging, reset);
  TEST_CHECK(
      resetTables && (*resetTables)["layout"].as_table()->empty() && (*resetTables)["widgets"].as_table()->empty()
  );
  reset.widgets->instances.emplace();
  auto empty = greeter::detail::stageCustomization(staging, reset);
  TEST_CHECK(empty && (*empty)["widgets"]["widget_order"].as_array()->empty());
  TEST_CHECK(std::filesystem::is_empty(staging));
  auto invalid = profile;
  invalid.widgets->instances->front().type = "plugin/custom";
  TEST_CHECK(!greeter::detail::stageCustomization(staging, invalid));
  invalid = profile;
  invalid.widgets->instances->front().settings["script"] = "injected";
  TEST_CHECK(!greeter::detail::stageCustomization(staging, invalid));
  for (const auto* path :
       {"https://example.test/image.png", "data:image/png;base64,AAAA", "relative.png", "/missing"}) {
    invalid = profile;
    invalid.widgets->instances->at(2).settings["image_path"] = std::string(path);
    TEST_CHECK(!greeter::detail::stageCustomization(staging, invalid));
  }
  // Disabled instances must not bypass asset validation.
  invalid = profile;
  invalid.widgets->instances->erase(invalid.widgets->instances->begin() + 2);
  write(imagePath, "not an image");
  TEST_CHECK(!greeter::detail::stageCustomization(staging, invalid));
  write(imagePath, "");
  std::filesystem::resize_file(imagePath, 16U * 1024U * 1024U + 1);
  TEST_CHECK(!greeter::detail::stageCustomization(staging, profile));
  std::filesystem::remove(imagePath);
  std::filesystem::create_symlink(root / "missing", imagePath);
  TEST_CHECK(!greeter::detail::stageCustomization(staging, profile));
  std::filesystem::remove(imagePath);
  write(
      root / "source.svg",
      "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='3'><rect width='2' height='3' fill='red'/></svg>"
  );
  auto svg = profileFor(root / "source.svg");
  TEST_CHECK(greeter::detail::stageCustomization(staging, svg));
  std::filesystem::remove_all(staging);
  std::filesystem::create_directory(staging);
  GreeterProfile many;
  many.widgets = GreeterWidgetCollection{std::vector<GreeterWidgetConfig>{}};
  for (unsigned i = 0; i < 33; ++i) {
    const std::array<std::uint8_t, 4> pixel{static_cast<std::uint8_t>(i), 32, 64, 255};
    const auto encoded = encodePng(pixel.data(), 1, 1);
    const auto source = root / ("image" + std::to_string(i) + ".png");
    write(source, {reinterpret_cast<const char*>(encoded.data()), encoded.size()});
    GreeterWidgetConfig widget;
    widget.id = "image" + std::to_string(i);
    widget.type = "sticker";
    widget.settings["image_path"] = source.string();
    many.widgets->instances->push_back(widget);
  }
  const auto tooMany = greeter::detail::stageCustomization(staging, many);
  TEST_CHECK(!tooMany && tooMany.error().contains("32 images"));
  std::filesystem::remove_all(staging);
  std::filesystem::create_directory(staging);
  many.widgets->instances->pop_back();
  TEST_CHECK(greeter::detail::stageCustomization(staging, many));
  std::filesystem::remove_all(root);
  return 0;
}
