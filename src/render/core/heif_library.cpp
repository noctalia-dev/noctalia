#include "render/core/heif_library.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string_view>
#include <vector>

namespace {

  constexpr Logger kLog("heif");

  // Minimal hand-declared subset of the libheif C ABI, so libheif stays an optional runtime
  // dependency. Values and layouts are copied from libheif's public headers (heif_error.h,
  // heif_image.h, heif_decoding.h); they are part of the stable libheif.so.1 ABI.
  struct HeifContext;
  struct HeifImageHandle;
  struct HeifImage;

  struct HeifError {
    int code = 0;    // heif_error_code
    int subcode = 0; // heif_suberror_code
    const char* message = nullptr;
  };

  constexpr int kHeifErrorOk = 0;                // heif_error_Ok
  constexpr int kHeifColorspaceRgb = 1;          // heif_colorspace_RGB
  constexpr int kHeifChromaInterleavedRgba = 11; // heif_chroma_interleaved_RGBA (8 bits per component)
  constexpr int kHeifChannelInterleaved = 10;    // heif_channel_interleaved

  using HeifInitFn = HeifError (*)(void*);
  using HeifContextAllocFn = HeifContext* (*)();
  using HeifContextFreeFn = void (*)(HeifContext*);
  using HeifContextReadFromMemoryWithoutCopyFn = HeifError (*)(HeifContext*, const void*, std::size_t, const void*);
  using HeifContextGetPrimaryImageHandleFn = HeifError (*)(HeifContext*, HeifImageHandle**);
  using HeifImageHandleReleaseFn = void (*)(const HeifImageHandle*);
  using HeifDecodeImageFn = HeifError (*)(const HeifImageHandle*, HeifImage**, int, int, const void*);
  using HeifImageGetPlaneReadonlyFn = const std::uint8_t* (*)(const HeifImage*, int, int*);
  using HeifImageGetWidthFn = int (*)(const HeifImage*, int);
  using HeifImageGetHeightFn = int (*)(const HeifImage*, int);
  using HeifImageIsPremultipliedAlphaFn = int (*)(HeifImage*);
  using HeifImageReleaseFn = void (*)(const HeifImage*);

  struct HeifApi {
    HeifContextAllocFn contextAlloc = nullptr;
    HeifContextFreeFn contextFree = nullptr;
    HeifContextReadFromMemoryWithoutCopyFn contextReadFromMemoryWithoutCopy = nullptr;
    HeifContextGetPrimaryImageHandleFn contextGetPrimaryImageHandle = nullptr;
    HeifImageHandleReleaseFn imageHandleRelease = nullptr;
    HeifDecodeImageFn decodeImage = nullptr;
    HeifImageGetPlaneReadonlyFn imageGetPlaneReadonly = nullptr;
    HeifImageGetWidthFn imageGetWidth = nullptr;
    HeifImageGetHeightFn imageGetHeight = nullptr;
    HeifImageReleaseFn imageRelease = nullptr;
    // Optional: absent before libheif 1.12, where decoded images never carry premultiplied alpha.
    HeifImageIsPremultipliedAlphaFn imageIsPremultipliedAlpha = nullptr;
  };

  template <typename T> bool loadDlsymFunction(void* library, const char* name, T& out) {
    void* symbol = dlsym(library, name);
    if (symbol == nullptr) {
      return false;
    }
    std::memcpy(&out, &symbol, sizeof(out));
    return true;
  }

  // Loaded once and never unloaded: decoded images and plugin state live for the process.
  const HeifApi* loadHeifApi() {
    static std::once_flag once;
    static HeifApi api;
    static bool available = false;

    std::call_once(once, [] {
      void* library = dlopen("libheif.so.1", RTLD_LAZY | RTLD_LOCAL);
      if (library == nullptr) {
        kLog.info("libheif.so.1 not available, HEIF/AVIF images will not decode");
        return;
      }

      if (!loadDlsymFunction(library, "heif_context_alloc", api.contextAlloc)
          || !loadDlsymFunction(library, "heif_context_free", api.contextFree)
          || !loadDlsymFunction(
              library, "heif_context_read_from_memory_without_copy", api.contextReadFromMemoryWithoutCopy
          )
          || !loadDlsymFunction(library, "heif_context_get_primary_image_handle", api.contextGetPrimaryImageHandle)
          || !loadDlsymFunction(library, "heif_image_handle_release", api.imageHandleRelease)
          || !loadDlsymFunction(library, "heif_decode_image", api.decodeImage)
          || !loadDlsymFunction(library, "heif_image_get_plane_readonly", api.imageGetPlaneReadonly)
          || !loadDlsymFunction(library, "heif_image_get_width", api.imageGetWidth)
          || !loadDlsymFunction(library, "heif_image_get_height", api.imageGetHeight)
          || !loadDlsymFunction(library, "heif_image_release", api.imageRelease)) {
        kLog.info("libheif.so.1 lacks required symbols, HEIF/AVIF images will not decode");
        dlclose(library);
        return;
      }
      (void)loadDlsymFunction(library, "heif_image_is_premultiplied_alpha", api.imageIsPremultipliedAlpha);

      // heif_init() loads external codec plugins (distributions ship the AV1/HEVC decoders that
      // way). Libraries older than 1.13 lack it and only have built-in codecs.
      HeifInitFn init = nullptr;
      if (loadDlsymFunction(library, "heif_init", init)) {
        const HeifError err = init(nullptr);
        if (err.code != kHeifErrorOk) {
          kLog.debug("heif_init failed: {}", err.message != nullptr ? err.message : "unknown error");
        }
      }
      (void)dlerror();

      available = true;
      kLog.debug("loaded libheif.so.1");
    });

    return available ? &api : nullptr;
  }

  std::string heifErrorMessage(const HeifError& err, std::string_view fallback) {
    if (err.message != nullptr && err.message[0] != '\0') {
      return std::string("libheif: ") + err.message;
    }
    return std::string("libheif: ") + std::string(fallback);
  }

  void unpremultiplyRgba(std::vector<std::uint8_t>& pixels) {
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
      const unsigned alpha = pixels[i + 3];
      if (alpha == 0 || alpha == 255) {
        continue;
      }
      for (std::size_t c = 0; c < 3; ++c) {
        const unsigned value = (pixels[i + c] * 255U + alpha / 2U) / alpha;
        pixels[i + c] = static_cast<std::uint8_t>(value > 255U ? 255U : value);
      }
    }
  }

  std::uint32_t readU32BE(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24)
        | (static_cast<std::uint32_t>(p[1]) << 16)
        | (static_cast<std::uint32_t>(p[2]) << 8)
        | static_cast<std::uint32_t>(p[3]);
  }

  bool isHeifBrand(const std::uint8_t* brand) {
    // HEIF (ISO/IEC 23008-12) image and sequence brands plus the AVIF (AV1 Image File Format) brands.
    static constexpr std::array<std::string_view, 12> kBrands = {
        "heic", "heix", "hevc", "hevx", "heim", "heis", "hevm", "hevs", "mif1", "msf1", "avif", "avis",
    };
    const std::string_view candidate(reinterpret_cast<const char*>(brand), 4);
    return std::ranges::contains(kBrands, candidate);
  }

} // namespace

bool isHeifContainer(const std::uint8_t* data, std::size_t size) {
  // ftyp box: u32 size, "ftyp", major brand, u32 minor version, compatible brands up to the box end.
  if (data == nullptr || size < 16 || std::memcmp(data + 4, "ftyp", 4) != 0) {
    return false;
  }
  const std::uint32_t declared = readU32BE(data);
  if (declared != 0 && declared < 16) {
    return false;
  }
  const std::size_t boxEnd = declared == 0 ? size : std::min<std::size_t>(declared, size);

  if (isHeifBrand(data + 8)) {
    return true;
  }
  for (std::size_t offset = 16; offset + 4 <= boxEnd; offset += 4) {
    if (isHeifBrand(data + offset)) {
      return true;
    }
  }
  return false;
}

std::expected<DecodedRasterImage, std::string> decodeHeif(const std::uint8_t* data, std::size_t size) {
  const HeifApi* api = loadHeifApi();
  if (api == nullptr) {
    return std::unexpected("HEIF/AVIF decoding requires libheif");
  }

  HeifContext* context = api->contextAlloc();
  if (context == nullptr) {
    return std::unexpected("libheif: failed to allocate context");
  }

  // libheif enforces its default security limits (maximum image size, memory) while parsing and
  // decoding, so oversized images fail here instead of exhausting memory.
  HeifError err = api->contextReadFromMemoryWithoutCopy(context, data, size, nullptr);
  if (err.code != kHeifErrorOk) {
    api->contextFree(context);
    return std::unexpected(heifErrorMessage(err, "failed to parse HEIF container"));
  }

  HeifImageHandle* handle = nullptr;
  err = api->contextGetPrimaryImageHandle(context, &handle);
  if (err.code != kHeifErrorOk || handle == nullptr) {
    api->contextFree(context);
    return std::unexpected(heifErrorMessage(err, "no primary image"));
  }

  // Null decoding options keep libheif's defaults, which apply the irot/imir/clap transforms.
  HeifImage* image = nullptr;
  err = api->decodeImage(handle, &image, kHeifColorspaceRgb, kHeifChromaInterleavedRgba, nullptr);
  api->imageHandleRelease(handle);
  if (err.code != kHeifErrorOk || image == nullptr) {
    api->contextFree(context);
    return std::unexpected(heifErrorMessage(err, "failed to decode image"));
  }

  const int width = api->imageGetWidth(image, kHeifChannelInterleaved);
  const int height = api->imageGetHeight(image, kHeifChannelInterleaved);
  int stride = 0;
  const std::uint8_t* plane = api->imageGetPlaneReadonly(image, kHeifChannelInterleaved, &stride);
  const std::size_t rowBytes = static_cast<std::size_t>(width > 0 ? width : 0) * 4;
  if (plane == nullptr || width <= 0 || height <= 0 || stride < 0 || static_cast<std::size_t>(stride) < rowBytes) {
    api->imageRelease(image);
    api->contextFree(context);
    return std::unexpected("libheif: decoded image has no pixel data");
  }

  DecodedRasterImage decoded;
  decoded.width = width;
  decoded.height = height;
  decoded.pixels.resize(rowBytes * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(
        decoded.pixels.data() + static_cast<std::size_t>(y) * rowBytes,
        plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride), rowBytes
    );
  }

  // The render pipeline expects straight alpha, matching the other decoders.
  if (api->imageIsPremultipliedAlpha != nullptr && api->imageIsPremultipliedAlpha(image) != 0) {
    unpremultiplyRgba(decoded.pixels);
  }

  api->imageRelease(image);
  api->contextFree(context);
  return decoded;
}
