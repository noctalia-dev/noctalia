#include "render/codecs/icon_color_scheme.h"

#include "ui/palette.h"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <utility>

namespace codecs {

  namespace {

    struct IconColorScheme {
      std::vector<std::string> roots;
      std::shared_ptr<const std::string> stylesheet;
    };

    struct IconColorSchemeState {
      std::mutex mutex;
      std::shared_ptr<const IconColorScheme> scheme;
    };

    IconColorSchemeState& iconColorSchemeState() {
      static IconColorSchemeState state;
      return state;
    }

  } // namespace

  void setIconColorScheme(std::vector<std::string> roots, const Palette& colors) {
    std::shared_ptr<const IconColorScheme> scheme;
    if (!roots.empty()) {
      scheme = std::make_shared<const IconColorScheme>(IconColorScheme{
          .roots = std::move(roots),
          .stylesheet = std::make_shared<const std::string>(iconColorSchemeStylesheet(colors)),
      });
    }
    auto& state = iconColorSchemeState();
    const std::scoped_lock lock(state.mutex);
    state.scheme = std::move(scheme);
  }

  std::shared_ptr<const std::string>
  iconColorSchemeStylesheetFor(const std::string& path, std::span<const std::uint8_t> data) {
    std::shared_ptr<const IconColorScheme> scheme;
    {
      auto& state = iconColorSchemeState();
      const std::scoped_lock lock(state.mutex);
      scheme = state.scheme;
    }
    if (scheme == nullptr) {
      return nullptr;
    }
    const bool inTheme = std::ranges::any_of(scheme->roots, [&path](const std::string& root) {
      return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
    });
    const std::string_view content(reinterpret_cast<const char*>(data.data()), data.size());
    if (!inTheme || !content.contains("current-color-scheme")) {
      return nullptr;
    }
    return scheme->stylesheet;
  }

} // namespace codecs
