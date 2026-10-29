#pragma once

#include "core/toml.h" // IWYU pragma: keep

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::theme {

  // Community templates can ship app-scoped GTK CSS by declaring `gtk_extension = "gtk3"` (or
  // "gtk4") on an entry. Noctalia owns where that CSS lands and how GTK loads it: the rendered
  // file goes to <gtk dir>/noctalia-extensions/, and noctalia-extensions.css, imported by the
  // built-in GTK template's noctalia.css, lists the extensions of the enabled templates. Templates
  // never edit gtk.css themselves.

  enum class GtkToolkit { Gtk3, Gtk4 };

  struct GtkToolkits {
    bool gtk3 = false;
    bool gtk4 = false;

    [[nodiscard]] bool enabled(GtkToolkit toolkit) const { return toolkit == GtkToolkit::Gtk3 ? gtk3 : gtk4; }
    [[nodiscard]] bool any() const { return gtk3 || gtk4; }
  };

  struct GtkExtensionFile {
    GtkToolkit toolkit;
    std::string fileName;
  };

  inline constexpr std::string_view kGtkExtensionIndexFile = "noctalia-extensions.css";
  inline constexpr std::string_view kGtkExtensionDir = "noctalia-extensions";

  // $XDG_CONFIG_HOME/gtk-3.0 or $XDG_CONFIG_HOME/gtk-4.0.
  [[nodiscard]] std::filesystem::path gtkConfigDir(GtkToolkit toolkit);

  // Rewrites the `gtk_extension` entries of one community template manifest in place: a valid
  // entry for an enabled toolkit gets its Noctalia-owned output_path; an invalid entry, or one
  // whose toolkit's built-in GTK template is disabled, is removed. `gtkDir` maps a toolkit to its
  // GTK config directory. Returns the extension files the remaining entries render.
  [[nodiscard]] std::vector<GtkExtensionFile> prepareGtkExtensions(
      toml::table& root, std::string_view templateId, const GtkToolkits& toolkits, const std::filesystem::path& gtk3Dir,
      const std::filesystem::path& gtk4Dir
  );

  // Creates an empty extension index when none exists, so the @import in noctalia.css always
  // resolves (GTK fails the whole load on a missing import).
  bool ensureGtkExtensionIndex(const std::filesystem::path& gtkDir);

  // Rewrites the extension index to import exactly `fileNames` that exist on disk, then deletes
  // every other CSS file in the extension directory.
  bool syncGtkExtensions(const std::filesystem::path& gtkDir, std::vector<std::string> fileNames);

} // namespace noctalia::theme
