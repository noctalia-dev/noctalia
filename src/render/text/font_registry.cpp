#include "render/text/font_registry.h"

#include "core/log.h"

#include <atomic>
#include <fontconfig/fontconfig.h>
#include <unordered_set>

namespace text {

  namespace {

    constexpr Logger kLog("font-registry");

    std::unordered_set<std::string>& registeredFontFiles() {
      static std::unordered_set<std::string> s;
      return s;
    }

    std::atomic<std::uint64_t>& generation() {
      static std::atomic<std::uint64_t> g{0};
      return g;
    }

  } // namespace

  std::string registerFontFile(const std::filesystem::path& path) {
    const auto pathStr = path.string();
    const bool firstTime = !registeredFontFiles().contains(pathStr);
    if (firstTime) {
      if (!FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8*>(pathStr.c_str()))) {
        kLog.warn("failed to register font file: {}", pathStr);
        return {};
      }
      registeredFontFiles().insert(pathStr);
      generation().fetch_add(1, std::memory_order_relaxed);
    }

    FcFontSet* fontSet = FcFontSetCreate();
    FcStrSet* dirs = FcStrSetCreate();
    if (!fontSet || !dirs) {
      if (dirs)
        FcStrSetDestroy(dirs);
      if (fontSet)
        FcFontSetDestroy(fontSet);
      kLog.warn("failed to allocate font scan state for: {}", pathStr);
      return {};
    }

    if (!FcFileScan(fontSet, dirs, nullptr, nullptr, reinterpret_cast<const FcChar8*>(pathStr.c_str()), FcTrue)
        || fontSet->nfont <= 0) {
      kLog.warn("failed to query font family from: {}", pathStr);
      FcStrSetDestroy(dirs);
      FcFontSetDestroy(fontSet);
      return {};
    }

    FcChar8* family = nullptr;
    FcPatternGetString(fontSet->fonts[0], FC_FAMILY, 0, &family);
    std::string result = family ? reinterpret_cast<const char*>(family) : "";
    FcStrSetDestroy(dirs);
    FcFontSetDestroy(fontSet);
    return result;
  }

  std::uint64_t fontConfigGeneration() { return generation().load(std::memory_order_relaxed); }

} // namespace text
