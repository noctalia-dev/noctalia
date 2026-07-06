#include "capture/screenshot_service.h"

#include "capture/screencopy_util.h"
#include "capture/screenshot_region_overlay.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "notification/notification.h"
#include "notification/notification_manager.h"
#include "render/core/image_encoder.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stb_image_resize2.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

  constexpr Logger kLog("screenshot");
  constexpr const char* kScreenshotPathEnv = "NOCTALIA_SCREENSHOT_PATH";

  [[nodiscard]] std::string defaultFilenamePattern() { return "screenshot_%Y%m%d_%H%M%S"; }

  [[nodiscard]] std::string formatFilenameStem(std::string_view pattern, const std::string& labelBase, int suffix) {
    const auto now = std::chrono::system_clock::now();
    const auto unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::string resolvedPattern = pattern.empty() ? defaultFilenamePattern() : std::string(pattern);
    std::string stem = formatLocalUnixTime(unixSeconds, resolvedPattern);
    if (stem.empty()) {
      stem = "screenshot";
    }

    if (suffix > 0) {
      stem += '-';
      stem += std::to_string(suffix);
    }
    if (labelBase != "screenshot") {
      stem += '-';
      stem += labelBase;
    }
    return stem;
  }

  [[nodiscard]] bool hasAnyOutput(const ScreenshotService::OutputOptions& options) {
    return options.saveToFile || options.copyToClipboard || (options.pipeToCommand && !options.pipeCommand.empty());
  }

  [[nodiscard]] bool needsScreenshotPath(const ScreenshotService::OutputOptions& options) {
    return options.saveToFile || (options.pipeToCommand && !options.pipeCommand.empty());
  }

  [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
    for (const auto& entry : wayland.outputs()) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool resampleRgbaImage(ScreencopyImage& image, int targetWidth, int targetHeight) {
    if (targetWidth <= 0 || targetHeight <= 0 || image.width <= 0 || image.height <= 0) {
      return false;
    }
    if (image.width == targetWidth && image.height == targetHeight) {
      return true;
    }

    std::vector<std::uint8_t> resized(
        static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight) * 4U
    );
    if (stbir_resize_uint8_srgb(
            image.rgba.data(), image.width, image.height, 0, resized.data(), targetWidth, targetHeight, 0, STBIR_RGBA
        )
        == nullptr) {
      return false;
    }

    image.width = targetWidth;
    image.height = targetHeight;
    image.rgba = std::move(resized);
    return true;
  }

  [[nodiscard]] std::optional<ScreencopyImage>
  cropFrozenRegion(const ScreencopyImage& source, int logicalOutputWidth, int logicalOutputHeight, LogicalRect region) {
    if (logicalOutputWidth <= 0 || logicalOutputHeight <= 0 || region.width <= 0 || region.height <= 0) {
      return std::nullopt;
    }

    const double scaleX = static_cast<double>(source.width) / static_cast<double>(logicalOutputWidth);
    const double scaleY = static_cast<double>(source.height) / static_cast<double>(logicalOutputHeight);

    LogicalRect clipped = region;
    clipped.x = std::clamp(region.x, 0, logicalOutputWidth);
    clipped.y = std::clamp(region.y, 0, logicalOutputHeight);
    clipped.width = std::clamp(region.width, 0, logicalOutputWidth - clipped.x);
    clipped.height = std::clamp(region.height, 0, logicalOutputHeight - clipped.y);
    if (clipped.width <= 0 || clipped.height <= 0) {
      return std::nullopt;
    }

    const int srcX0 = std::clamp(static_cast<int>(std::floor(clipped.x * scaleX)), 0, source.width);
    const int srcY0 = std::clamp(static_cast<int>(std::floor(clipped.y * scaleY)), 0, source.height);
    const int srcX1 = std::clamp(static_cast<int>(std::ceil((clipped.x + clipped.width) * scaleX)), 0, source.width);
    const int srcY1 = std::clamp(static_cast<int>(std::ceil((clipped.y + clipped.height) * scaleY)), 0, source.height);
    const int outWidth = srcX1 - srcX0;
    const int outHeight = srcY1 - srcY0;
    if (outWidth <= 0 || outHeight <= 0) {
      return std::nullopt;
    }

    ScreencopyImage cropped;
    cropped.width = outWidth;
    cropped.height = outHeight;
    cropped.rgba.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4U);

    for (int y = 0; y < outHeight; ++y) {
      const int srcY = srcY0 + y;
      const auto* srcRow = source.rgba.data()
          + (static_cast<std::size_t>(srcY) * static_cast<std::size_t>(source.width) + static_cast<std::size_t>(srcX0))
              * 4U;
      auto* dstRow = cropped.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) * 4U;
      std::memcpy(dstRow, srcRow, static_cast<std::size_t>(outWidth) * 4U);
    }

    return cropped;
  }

  [[nodiscard]] capture::FrozenScreenshot*
  findFrozenScreenshot(std::vector<capture::FrozenScreenshot>& screenshots, wl_output* output) {
    for (auto& entry : screenshots) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  void attachStdioToDevNull() {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }
  }

  bool writeAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t written = ::write(fd, data + offset, size - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (written == 0) {
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

  void pipePngToCommandAsync(
      std::string command, std::vector<std::uint8_t> png, const std::optional<std::filesystem::path>& screenshotPath
  ) {
    if (command.empty() || png.empty()) {
      return;
    }
    std::string screenshotPathString = screenshotPath.has_value() ? screenshotPath->string() : std::string{};

    std::thread([command = std::move(command), png = std::move(png),
                 screenshotPathString = std::move(screenshotPathString)]() {
      // Block SIGPIPE on this thread so a command that stops reading stdin makes
      // write() fail with EPIPE instead of terminating the whole process.
      sigset_t pipeMask;
      sigemptyset(&pipeMask);
      sigaddset(&pipeMask, SIGPIPE);
      pthread_sigmask(SIG_BLOCK, &pipeMask, nullptr);

      int stdinPipe[2] = {-1, -1};
      if (::pipe(stdinPipe) != 0) {
        kLog.warn("screenshot pipe: failed to create stdin pipe");
        return;
      }

      const pid_t child = ::fork();
      if (child < 0) {
        kLog.warn("screenshot pipe: fork failed");
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        return;
      }

      if (child == 0) {
        ::close(stdinPipe[1]);
        if (::dup2(stdinPipe[0], STDIN_FILENO) < 0) {
          ::_exit(126);
        }
        ::close(stdinPipe[0]);
        attachStdioToDevNull();
        if (screenshotPathString.empty()) {
          ::unsetenv(kScreenshotPathEnv);
        } else if (::setenv(kScreenshotPathEnv, screenshotPathString.c_str(), 1) != 0) {
          ::_exit(126);
        }
        // Restore default SIGPIPE handling for the spawned command.
        ::signal(SIGPIPE, SIG_DFL);
        pthread_sigmask(SIG_UNBLOCK, &pipeMask, nullptr);
        const char* argv[] = {"/bin/sh", "-lc", command.c_str(), nullptr};
        ::execv("/bin/sh", const_cast<char* const*>(argv));
        ::_exit(127);
      }

      ::close(stdinPipe[0]);
      const bool wrote = writeAll(stdinPipe[1], png.data(), png.size());
      ::close(stdinPipe[1]);
      if (!wrote) {
        kLog.warn("screenshot pipe: failed to write PNG to command stdin");
      }

      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        kLog.warn("screenshot pipe: command exited with status {}", status);
      }
    }).detach();
  }

  [[nodiscard]] std::vector<wl_output*> validOutputs(const WaylandConnection& wayland) {
    std::vector<wl_output*> outputs;
    for (const auto& output : wayland.outputs()) {
      if (output.output != nullptr && output.logicalWidth > 0 && output.logicalHeight > 0) {
        outputs.push_back(output.output);
      }
    }
    return outputs;
  }

  [[nodiscard]] std::expected<wl_output*, std::string>
  resolveOutputSelector(const WaylandConnection& wayland, std::string_view selector) {
    const std::string token = StringUtils::trim(selector);
    if (token.empty()) {
      return std::unexpected("error: empty monitor selector\n");
    }

    std::vector<wl_output*> matches;
    std::vector<std::string> knownOutputs;
    for (const auto& output : wayland.outputs()) {
      if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }
      if (!output.connectorName.empty()) {
        knownOutputs.push_back(output.connectorName);
      }
      if (outputMatchesSelector(token, output)) {
        matches.push_back(output.output);
      }
    }

    std::ranges::sort(knownOutputs);
    knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
    std::ranges::sort(matches, [](wl_output* a, wl_output* b) {
      return reinterpret_cast<std::uintptr_t>(a) < reinterpret_cast<std::uintptr_t>(b);
    });
    matches.erase(std::ranges::unique(matches).begin(), matches.end());

    if (matches.empty()) {
      std::string error = "error: unknown monitor selector \"" + token + "\"";
      if (!knownOutputs.empty()) {
        error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
      }
      error += "\n";
      return std::unexpected(std::move(error));
    }
    if (matches.size() > 1) {
      std::vector<std::string> matchNames;
      matchNames.reserve(matches.size());
      for (wl_output* output : matches) {
        if (const auto* entry = findOutput(wayland, output); entry != nullptr && !entry->connectorName.empty()) {
          matchNames.push_back(entry->connectorName);
        }
      }
      return std::unexpected(
          "error: monitor selector \""
          + token
          + "\" matched multiple outputs: "
          + StringUtils::join(matchNames, ", ")
          + "\n"
      );
    }

    return matches.front();
  }

  struct CapturedOutputFrame {
    ScreencopyImage image;
    const WaylandOutput* output = nullptr;
  };

  struct RegionIntersectTarget {
    wl_output* output = nullptr;
    LogicalRect localRegion{};
  };

  struct GlobalRegionPiece {
    const WaylandOutput* output = nullptr;
    LogicalRect localRegion{};
    ScreencopyImage image;
  };

  void blitOpaqueRgba(ScreencopyImage& canvas, int destX, int destY, const ScreencopyImage& source) {
    if (destX < 0 || destY < 0 || source.width <= 0 || source.height <= 0) {
      return;
    }
    const int copyWidth = std::min(source.width, canvas.width - destX);
    const int copyHeight = std::min(source.height, canvas.height - destY);
    if (copyWidth <= 0 || copyHeight <= 0) {
      return;
    }

    for (int y = 0; y < copyHeight; ++y) {
      const auto* srcRow =
          source.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) * 4U;
      auto* dstRow = canvas.rgba.data()
          + (static_cast<std::size_t>(destY + y) * static_cast<std::size_t>(canvas.width)
             + static_cast<std::size_t>(destX))
              * 4U;
      std::memcpy(dstRow, srcRow, static_cast<std::size_t>(copyWidth) * 4U);
    }
  }

  [[nodiscard]] std::vector<RegionIntersectTarget>
  intersectGlobalRegion(const WaylandConnection& wayland, LogicalRect globalRegion) {
    const int globalX0 = globalRegion.x;
    const int globalY0 = globalRegion.y;
    const int globalX1 = globalRegion.x + globalRegion.width;
    const int globalY1 = globalRegion.y + globalRegion.height;

    std::vector<RegionIntersectTarget> targets;
    for (const auto& out : wayland.outputs()) {
      if (out.output == nullptr || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
        continue;
      }
      const int ix0 = std::max(globalX0, out.logicalX);
      const int iy0 = std::max(globalY0, out.logicalY);
      const int ix1 = std::min(globalX1, out.logicalX + out.logicalWidth);
      const int iy1 = std::min(globalY1, out.logicalY + out.logicalHeight);
      if (ix1 <= ix0 || iy1 <= iy0) {
        continue;
      }
      targets.push_back(
          RegionIntersectTarget{
              .output = out.output,
              .localRegion = {
                  .x = ix0 - out.logicalX,
                  .y = iy0 - out.logicalY,
                  .width = ix1 - ix0,
                  .height = iy1 - iy0,
              },
          }
      );
    }
    return targets;
  }

  [[nodiscard]] std::optional<ScreencopyImage>
  composeGlobalRegion(LogicalRect globalRegion, std::vector<GlobalRegionPiece> pieces) {
    if (globalRegion.width <= 0 || globalRegion.height <= 0 || pieces.empty()) {
      return std::nullopt;
    }

    if (pieces.size() == 1) {
      return std::move(pieces.front().image);
    }

    double canvasScale = 1.0;
    for (const auto& piece : pieces) {
      if (piece.output == nullptr || piece.localRegion.width <= 0 || piece.localRegion.height <= 0) {
        return std::nullopt;
      }
      canvasScale = std::max({
          canvasScale,
          static_cast<double>(piece.image.width) / static_cast<double>(piece.localRegion.width),
          static_cast<double>(piece.image.height) / static_cast<double>(piece.localRegion.height),
      });
    }

    const auto scaled = [canvasScale](int logical) { return static_cast<int>(std::lround(logical * canvasScale)); };

    const int canvasWidth = scaled(globalRegion.width);
    const int canvasHeight = scaled(globalRegion.height);
    if (canvasWidth <= 0 || canvasHeight <= 0) {
      return std::nullopt;
    }

    ScreencopyImage canvas;
    canvas.width = canvasWidth;
    canvas.height = canvasHeight;
    canvas.rgba.assign(static_cast<std::size_t>(canvasWidth) * static_cast<std::size_t>(canvasHeight) * 4U, 0);

    for (auto& piece : pieces) {
      const int globalPieceX = piece.output->logicalX + piece.localRegion.x;
      const int globalPieceY = piece.output->logicalY + piece.localRegion.y;
      const int destX = scaled(globalPieceX - globalRegion.x);
      const int destY = scaled(globalPieceY - globalRegion.y);
      const int targetWidth = scaled(piece.localRegion.width);
      const int targetHeight = scaled(piece.localRegion.height);
      if (!resampleRgbaImage(piece.image, targetWidth, targetHeight)) {
        return std::nullopt;
      }
      blitOpaqueRgba(canvas, destX, destY, piece.image);
    }

    return canvas;
  }

  [[nodiscard]] std::optional<ScreencopyImage> stitchOutputFrames(std::vector<CapturedOutputFrame> frames) {
    if (frames.empty()) {
      return std::nullopt;
    }

    for (const auto& frame : frames) {
      if (frame.output == nullptr || frame.output->logicalWidth <= 0 || frame.output->logicalHeight <= 0) {
        return std::nullopt;
      }
    }

    if (frames.size() == 1) {
      return std::move(frames.front().image);
    }

    // Stitch in physical pixels: pick a uniform canvas density equal to the highest captured
    // scale so the sharpest monitor keeps its full resolution. Each logical layout coordinate is
    // multiplied by this density; lower-density outputs are upscaled to keep the layout aligned.
    double canvasScale = 1.0;
    for (const auto& frame : frames) {
      const double scaleX = static_cast<double>(frame.image.width) / static_cast<double>(frame.output->logicalWidth);
      const double scaleY = static_cast<double>(frame.image.height) / static_cast<double>(frame.output->logicalHeight);
      canvasScale = std::max({canvasScale, scaleX, scaleY});
    }

    int minLogicalX = frames.front().output->logicalX;
    int minLogicalY = frames.front().output->logicalY;
    for (const auto& frame : frames) {
      minLogicalX = std::min(minLogicalX, frame.output->logicalX);
      minLogicalY = std::min(minLogicalY, frame.output->logicalY);
    }

    const auto scaled = [canvasScale](int logical) { return static_cast<int>(std::lround(logical * canvasScale)); };

    int canvasWidth = 0;
    int canvasHeight = 0;
    for (auto& frame : frames) {
      const auto* out = frame.output;
      const int targetWidth = scaled(out->logicalWidth);
      const int targetHeight = scaled(out->logicalHeight);
      if (!resampleRgbaImage(frame.image, targetWidth, targetHeight)) {
        return std::nullopt;
      }
      canvasWidth = std::max(canvasWidth, scaled(out->logicalX - minLogicalX) + targetWidth);
      canvasHeight = std::max(canvasHeight, scaled(out->logicalY - minLogicalY) + targetHeight);
    }

    if (canvasWidth <= 0 || canvasHeight <= 0) {
      return std::nullopt;
    }

    ScreencopyImage canvas;
    canvas.width = canvasWidth;
    canvas.height = canvasHeight;
    canvas.rgba.assign(static_cast<std::size_t>(canvasWidth) * static_cast<std::size_t>(canvasHeight) * 4U, 0);

    for (const auto& frame : frames) {
      const auto* out = frame.output;
      blitOpaqueRgba(canvas, scaled(out->logicalX - minLogicalX), scaled(out->logicalY - minLogicalY), frame.image);
    }

    return canvas;
  }

} // namespace

ScreenshotService::ScreenshotService(
    WaylandConnection& wayland, CompositorPlatform& platform, NotificationManager& notifications,
    ClipboardService* clipboard
)
    : m_wayland(wayland), m_platform(platform), m_notifications(notifications), m_clipboard(clipboard),
      m_capture(wayland) {}

ScreenshotService::~ScreenshotService() = default;

bool ScreenshotService::available() const noexcept { return m_capture.available(); }

void ScreenshotService::onOutputChange() {
  if (m_regionOverlay != nullptr) {
    m_regionOverlay->onOutputChange();
  }
}

bool ScreenshotService::onPointerEvent(const PointerEvent& event) {
  if (m_regionOverlay == nullptr || !m_regionOverlay->isActive()) {
    return false;
  }
  return m_regionOverlay->onPointerEvent(event);
}

bool ScreenshotService::onKeyboardEvent(const KeyboardEvent& event) {
  if (!event.pressed) {
    return false;
  }
  const bool regionActive = m_regionOverlay != nullptr && m_regionOverlay->isActive();
  if (!m_freezeCaptureActive && !regionActive) {
    return false;
  }
  if (regionActive && m_regionOverlay->onKeyboardEvent(event)) {
    return true;
  }
  if (!KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    return false;
  }
  cancelRegionCapture();
  return true;
}

ScreenshotService::OutputOptions ScreenshotService::outputOptionsFromConfig(const Config& config) {
  const auto& screenshot = config.shell.screenshot;
  OutputOptions options;
  options.saveToFile = screenshot.saveToFile;
  options.copyToClipboard = screenshot.copyToClipboard;
  options.pipeToCommand = screenshot.pipeToCommand;
  options.freezeScreen = screenshot.freezeScreen;
  options.confirmRegion = screenshot.confirmRegion;
  options.pipeCommand = screenshot.pipeCommand;
  options.directory = screenshot.directory;
  options.filenamePattern = screenshot.filenamePattern;
  return options;
}

void ScreenshotService::registerIpc(IpcService& ipc, const ConfigService& configService) {
  ipc.registerHandler(
      "screenshot-region",
      [this, &configService](const std::string& /*args*/) -> std::string {
        if (!available()) {
          return "error: screen capture is not available on this compositor\n";
        }
        auto* renderContext = PanelManager::instance().renderContext();
        if (renderContext == nullptr) {
          return "error: render context unavailable\n";
        }
        beginRegionCapture(*renderContext, outputOptionsFromConfig(configService.config()));
        return "ok\n";
      },
      "screenshot-region", "Start an interactive region screenshot"
  );

  ipc.registerHandler(
      "screenshot-fullscreen",
      [this, &configService](const std::string& args) -> std::string {
        if (!available()) {
          return "error: screen capture is not available on this compositor\n";
        }
        const std::string token = StringUtils::trim(args);
        const auto options = outputOptionsFromConfig(configService.config());
        if (token == "all" || token == "*") {
          captureAllOutputs(options);
          return "ok\n";
        }
        if (token == "pick") {
          const auto outputs = validOutputs(m_wayland);
          if (outputs.size() <= 1) {
            captureFullscreen(options, outputs.empty() ? nullptr : outputs.front());
            return "ok\n";
          }
          auto* renderContext = PanelManager::instance().renderContext();
          if (renderContext == nullptr) {
            return "error: render context unavailable\n";
          }
          beginFullscreenCapture(*renderContext, options);
          return "ok\n";
        }
        if (!token.empty() && token != "pick") {
          auto output = resolveOutputSelector(m_wayland, token);
          if (!output) {
            return output.error();
          }
          captureFullscreen(options, *output);
          return "ok\n";
        }

        captureFullscreen(options);
        return "ok\n";
      },
      "screenshot-fullscreen [pick|monitor|all]",
      "Capture the focused monitor by default, pick interactively with pick, or all outputs with all"
  );
}

wl_output* ScreenshotService::preferredCaptureOutput() const {
  if (wl_output* output = m_platform.preferredInteractiveOutput(); output != nullptr) {
    return output;
  }
  const auto outputs = validOutputs(m_wayland);
  return outputs.empty() ? nullptr : outputs.front();
}

void ScreenshotService::captureFullscreen(const OutputOptions& options, wl_output* output) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (output == nullptr) {
    output = preferredCaptureOutput();
  }
  if (output == nullptr) {
    notifyError("No outputs available");
    return;
  }
  captureOutput(output, std::nullopt, "screenshot", options);
}

void ScreenshotService::captureFullscreenInteractive(RenderContext& renderContext, const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (validOutputs(m_wayland).size() <= 1) {
    captureFullscreen(options);
    return;
  }
  beginFullscreenCapture(renderContext, options);
}

void ScreenshotService::beginRegionCapture(RenderContext& renderContext, const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancel();
  }
  if (m_freezeCaptureActive) {
    abortFreezeCapture("Screenshot cancelled");
  }

  m_regionOutputOptions = options;
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = false;

  if (options.freezeScreen) {
    DeferredCall::callLater([this]() { beginFreezeCapture(); });
    return;
  }

  startRegionOverlay(renderContext);
}

void ScreenshotService::beginFullscreenCapture(RenderContext& renderContext, const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancel();
  }
  if (m_freezeCaptureActive) {
    abortFreezeCapture("Screenshot cancelled");
  }

  m_regionOutputOptions = options;
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = true;

  if (options.freezeScreen) {
    DeferredCall::callLater([this]() { beginFreezeCapture(); });
    return;
  }

  startFullscreenOverlay(renderContext);
}

void ScreenshotService::ensureRegionOverlay() {
  if (m_regionRenderContext == nullptr) {
    return;
  }
  if (m_regionOverlay == nullptr) {
    m_regionOverlay = std::make_unique<capture::ScreenshotRegionOverlay>();
  }
  m_regionOverlay->initialize(m_wayland, m_regionRenderContext);
  m_regionOverlay->setFailureCallback([this](const std::string& message) {
    m_frozenScreenshots.clear();
    m_regionFullscreenPick = false;
    notifyError(message);
  });
  m_regionOverlay->setCompleteCallback([this](std::optional<LogicalRect> region, wl_output* output) {
    if (!region.has_value()) {
      m_frozenScreenshots.clear();
      if (m_regionOverlay != nullptr) {
        m_regionOverlay->setFrozenScreenshots({});
      }
      m_regionFullscreenPick = false;
      return;
    }
    if (m_regionFullscreenPick) {
      if (output == nullptr) {
        m_frozenScreenshots.clear();
        if (m_regionOverlay != nullptr) {
          m_regionOverlay->setFrozenScreenshots({});
        }
        m_regionFullscreenPick = false;
        return;
      }
      if (m_regionOutputOptions.freezeScreen && m_regionOverlay != nullptr) {
        m_frozenScreenshots = m_regionOverlay->takeFrozenScreenshots();
      }
      completeFullscreenSelection(output, m_regionOutputOptions);
      m_regionFullscreenPick = false;
      return;
    }
    if (m_regionOutputOptions.freezeScreen && m_regionOverlay != nullptr) {
      m_frozenScreenshots = m_regionOverlay->takeFrozenScreenshots();
    }
    if (m_regionOutputOptions.freezeScreen && !m_frozenScreenshots.empty()) {
      deliverFrozenGlobalRegion(*region, m_regionOutputOptions);
      return;
    }
    captureGlobalRegion(*region, m_regionOutputOptions);
  });
}

void ScreenshotService::startRegionOverlay(RenderContext& renderContext) {
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = false;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  m_regionOverlay->begin(false, false, m_regionOutputOptions.confirmRegion);
}

void ScreenshotService::startFullscreenOverlay(RenderContext& renderContext) {
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = true;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  m_regionOverlay->begin(false, true, false);
}

void ScreenshotService::beginFreezeCapture() {
  if (m_regionRenderContext == nullptr) {
    notifyError("Render context unavailable");
    return;
  }

  m_frozenScreenshots.clear();
  m_pendingFreezeOutputs.clear();
  for (const auto& output : m_wayland.outputs()) {
    if (output.output != nullptr && output.logicalWidth > 0 && output.logicalHeight > 0) {
      m_pendingFreezeOutputs.push_back(output.output);
    }
  }
  if (m_pendingFreezeOutputs.empty()) {
    notifyError("No outputs available");
    return;
  }

  m_freezeCaptureActive = true;

  while (!m_pendingFreezeOutputs.empty()) {
    if (!m_freezeCaptureActive) {
      m_frozenScreenshots.clear();
      return;
    }

    wl_output* output = m_pendingFreezeOutputs.front();
    m_pendingFreezeOutputs.erase(m_pendingFreezeOutputs.begin());

    if (m_capture.busy()) {
      m_capture.cancelInFlight();
    }

    ScreencopyImage image;
    std::string error;
    if (!screencopy::captureOutputBlocking(m_capture, m_wayland, output, image, error)) {
      if (!m_freezeCaptureActive) {
        m_frozenScreenshots.clear();
        return;
      }
      abortFreezeCapture(error.empty() ? "Failed to freeze screen" : error);
      return;
    }
    if (!m_freezeCaptureActive) {
      m_frozenScreenshots.clear();
      return;
    }
    if (!screencopy::orientCaptureNative(image, m_wayland, output)) {
      abortFreezeCapture("Failed to orient frozen screenshot");
      return;
    }
    m_frozenScreenshots.push_back(capture::FrozenScreenshot{.output = output, .image = std::move(image)});
  }

  m_freezeCaptureActive = false;
  finishFreezeCapture();
}

void ScreenshotService::finishFreezeCapture() {
  if (m_regionRenderContext == nullptr) {
    notifyError("Render context unavailable");
    m_frozenScreenshots.clear();
    return;
  }
  if (m_frozenScreenshots.empty()) {
    notifyError("Failed to freeze screen");
    return;
  }

  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots(std::move(m_frozenScreenshots));
  m_regionOverlay->begin(true, m_regionFullscreenPick, !m_regionFullscreenPick && m_regionOutputOptions.confirmRegion);
}

void ScreenshotService::abortFreezeCapture(const std::string& message) {
  cancelAllOutputsBatch();
  m_freezeCaptureActive = false;
  m_pendingFreezeOutputs.clear();
  m_frozenScreenshots.clear();
  m_capture.cancelInFlight();
  if (!message.empty()) {
    notifyError(message);
  }
}

void ScreenshotService::cancelRegionCapture() {
  cancelAllOutputsBatch();
  if (m_freezeCaptureActive) {
    abortFreezeCapture({});
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancelSelection();
  }
}

void ScreenshotService::deliverFrozenGlobalRegion(LogicalRect globalRegion, const OutputOptions& options) {
  const auto targets = intersectGlobalRegion(m_wayland, globalRegion);
  if (targets.empty()) {
    notifyError("Failed to crop frozen screenshot");
    m_frozenScreenshots.clear();
    return;
  }

  std::vector<GlobalRegionPiece> pieces;
  pieces.reserve(targets.size());
  for (const auto& target : targets) {
    auto* frozen = findFrozenScreenshot(m_frozenScreenshots, target.output);
    const auto* out = findOutput(m_wayland, target.output);
    if (frozen == nullptr || out == nullptr) {
      notifyError("Failed to crop frozen screenshot");
      m_frozenScreenshots.clear();
      return;
    }
    auto cropped = cropFrozenRegion(frozen->image, out->logicalWidth, out->logicalHeight, target.localRegion);
    if (!cropped.has_value()) {
      notifyError("Failed to crop frozen screenshot");
      m_frozenScreenshots.clear();
      return;
    }
    pieces.push_back(
        GlobalRegionPiece{
            .output = out,
            .localRegion = target.localRegion,
            .image = std::move(*cropped),
        }
    );
  }

  m_frozenScreenshots.clear();
  auto composed = composeGlobalRegion(globalRegion, std::move(pieces));
  if (!composed.has_value()) {
    notifyError("Failed to crop frozen screenshot");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, "region")) : std::nullopt;
  deliverCaptureResult(std::move(*composed), options, destPath);
}

void ScreenshotService::captureGlobalRegion(LogicalRect globalRegion, const OutputOptions& options) {
  cancelAllOutputsBatch();
  cancelGlobalRegionBatch();
  m_captureQueue.clear();
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  const auto targets = intersectGlobalRegion(m_wayland, globalRegion);
  if (targets.empty()) {
    notifyError("No outputs available");
    return;
  }
  if (targets.size() == 1) {
    captureOutput(targets.front().output, targets.front().localRegion, "region", options);
    return;
  }

  std::vector<GlobalRegionCaptureTarget> batchTargets;
  batchTargets.reserve(targets.size());
  for (const auto& target : targets) {
    batchTargets.push_back(
        GlobalRegionCaptureTarget{
            .output = target.output,
            .localRegion = target.localRegion,
        }
    );
  }

  m_globalRegionBatch = std::make_unique<GlobalRegionBatch>(GlobalRegionBatch{
      .options = options,
      .globalRegion = globalRegion,
      .targets = std::move(batchTargets),
      .pieces = {},
      .next = 0,
  });
  startNextGlobalRegionCapture();
}

void ScreenshotService::startNextGlobalRegionCapture() {
  if (!m_globalRegionBatch) {
    return;
  }

  auto& batch = *m_globalRegionBatch;
  while (batch.next < batch.targets.size() && batch.targets[batch.next].output == nullptr) {
    ++batch.next;
  }
  if (batch.next >= batch.targets.size()) {
    finishGlobalRegionBatch();
    return;
  }

  const GlobalRegionCaptureTarget target = batch.targets[batch.next];
  ++batch.next;
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  m_capture.capture(
      target.output, target.localRegion, false,
      [this, output = target.output,
       localRegion = target.localRegion](std::optional<ScreencopyImage> image, const std::string& error) {
        onGlobalRegionFrameCaptured(output, localRegion, std::move(image), error);
      }
  );
}

void ScreenshotService::onGlobalRegionFrameCaptured(
    wl_output* output, LogicalRect localRegion, std::optional<ScreencopyImage> image, const std::string& error
) {
  if (!m_globalRegionBatch) {
    return;
  }
  if (!error.empty() || !image.has_value()) {
    kLog.warn("region screenshot failed: {}", error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    cancelGlobalRegionBatch();
    return;
  }
  if (!screencopy::orientCaptureNative(*image, m_wayland, output)) {
    notifyError("Failed to scale screenshot");
    cancelGlobalRegionBatch();
    return;
  }

  m_globalRegionBatch->pieces.push_back(
      GlobalRegionBatch::Piece{
          .output = output,
          .localRegion = localRegion,
          .image = std::move(*image),
      }
  );
  DeferredCall::callLater([this]() { startNextGlobalRegionCapture(); });
}

void ScreenshotService::finishGlobalRegionBatch() {
  if (!m_globalRegionBatch) {
    return;
  }

  GlobalRegionBatch batch = std::move(*m_globalRegionBatch);
  m_globalRegionBatch.reset();
  if (batch.pieces.empty()) {
    notifyError("Screenshot failed");
    return;
  }

  std::vector<GlobalRegionPiece> pieces;
  pieces.reserve(batch.pieces.size());
  for (auto& piece : batch.pieces) {
    const auto* out = findOutput(m_wayland, piece.output);
    if (out == nullptr) {
      notifyError("Failed to combine screenshots");
      return;
    }
    pieces.push_back(
        GlobalRegionPiece{
            .output = out,
            .localRegion = piece.localRegion,
            .image = std::move(piece.image),
        }
    );
  }

  auto composed = composeGlobalRegion(batch.globalRegion, std::move(pieces));
  if (!composed.has_value()) {
    notifyError("Failed to combine screenshots");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(batch.options) ? std::optional(makeScreenshotPath(batch.options, "region")) : std::nullopt;
  deliverCaptureResult(std::move(*composed), batch.options, destPath);
}

void ScreenshotService::cancelGlobalRegionBatch() { m_globalRegionBatch.reset(); }

void ScreenshotService::deliverFrozenRegion(LogicalRect region, wl_output* output, const OutputOptions& options) {
  auto* frozen = findFrozenScreenshot(m_frozenScreenshots, output);
  const auto* out = findOutput(m_wayland, output);
  if (frozen == nullptr || out == nullptr) {
    notifyError("Failed to crop frozen screenshot");
    m_frozenScreenshots.clear();
    return;
  }

  auto cropped = cropFrozenRegion(frozen->image, out->logicalWidth, out->logicalHeight, region);
  m_frozenScreenshots.clear();
  if (!cropped.has_value()) {
    notifyError("Failed to crop frozen screenshot");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, "region")) : std::nullopt;
  deliverCaptureResult(std::move(*cropped), options, destPath);
}

void ScreenshotService::completeFullscreenSelection(wl_output* output, const OutputOptions& options) {
  if (output == nullptr) {
    m_frozenScreenshots.clear();
    return;
  }
  if (options.freezeScreen && !m_frozenScreenshots.empty()) {
    const auto* out = findOutput(m_wayland, output);
    if (out == nullptr) {
      notifyError("Failed to crop frozen screenshot");
      m_frozenScreenshots.clear();
      return;
    }
    deliverFrozenRegion(
        LogicalRect{
            .x = 0,
            .y = 0,
            .width = out->logicalWidth,
            .height = out->logicalHeight,
        },
        output, options
    );
    return;
  }
  m_frozenScreenshots.clear();
  captureOutput(output, std::nullopt, "screenshot", options);
}

void ScreenshotService::captureOutput(
    wl_output* output, std::optional<LogicalRect> region, const std::string& labelBase, const OutputOptions& options,
    int pathSuffix
) {
  if (output == nullptr) {
    notifyError("No output for capture");
    return;
  }

  PendingCapture pending{
      .output = output,
      .region = region,
      .outputOptions = options,
      .destPath = needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, labelBase, pathSuffix))
                                               : std::nullopt,
  };
  if (m_capture.busy()) {
    m_captureQueue.push_back(std::move(pending));
    return;
  }

  m_capture.capture(
      pending.output, pending.region, false,
      [this, options = pending.outputOptions, destPath = pending.destPath,
       output = pending.output](std::optional<ScreencopyImage> image, const std::string& error) {
        onCaptureComplete(std::move(image), error, options, destPath, output);
      }
  );
}

void ScreenshotService::startNextQueuedCapture() {
  if (m_captureQueue.empty() || m_capture.busy()) {
    return;
  }
  DeferredCall::callLater([this]() {
    if (m_captureQueue.empty() || m_capture.busy()) {
      return;
    }
    PendingCapture pending = std::move(m_captureQueue.front());
    m_captureQueue.erase(m_captureQueue.begin());
    m_capture.capture(
        pending.output, pending.region, false,
        [this, options = pending.outputOptions, destPath = pending.destPath,
         output = pending.output](std::optional<ScreencopyImage> image, const std::string& error) {
          onCaptureComplete(std::move(image), error, options, destPath, output);
        }
    );
  });
}

void ScreenshotService::captureAllOutputs(const OutputOptions& options) {
  cancelAllOutputsBatch();
  cancelGlobalRegionBatch();
  m_captureQueue.clear();
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  std::vector<AllOutputCaptureTarget> targets;
  int index = 0;
  for (const auto& output : m_wayland.outputs()) {
    if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      continue;
    }
    ++index;
    AllOutputCaptureTarget target{
        .output = output.output,
        .label = output.connectorName.empty() ? ("monitor-" + std::to_string(index)) : output.connectorName,
    };
    targets.push_back(std::move(target));
  }
  if (targets.empty()) {
    notifyError("No outputs available");
    return;
  }
  if (targets.size() == 1) {
    captureOutput(targets.front().output, std::nullopt, targets.front().label, options);
    return;
  }

  m_allOutputsBatch = std::make_unique<AllOutputsBatch>(AllOutputsBatch{
      .options = options,
      .targets = std::move(targets),
      .frames = {},
      .next = 0,
  });
  startNextAllOutputsCapture();
}

void ScreenshotService::startNextAllOutputsCapture() {
  if (!m_allOutputsBatch) {
    return;
  }

  auto& batch = *m_allOutputsBatch;
  while (batch.next < batch.targets.size() && batch.targets[batch.next].output == nullptr) {
    ++batch.next;
  }
  if (batch.next >= batch.targets.size()) {
    finishAllOutputsBatch();
    return;
  }

  const AllOutputCaptureTarget target = batch.targets[batch.next];
  ++batch.next;
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  m_capture.capture(
      target.output, std::nullopt, false,
      [this, output = target.output,
       label = target.label](std::optional<ScreencopyImage> image, const std::string& error) {
        onAllOutputsFrameCaptured(output, label, std::move(image), error);
      }
  );
}

void ScreenshotService::onAllOutputsFrameCaptured(
    wl_output* output, const std::string& label, std::optional<ScreencopyImage> image, const std::string& error
) {
  if (!m_allOutputsBatch) {
    return;
  }
  if (!error.empty() || !image.has_value()) {
    kLog.warn("screenshot failed for {}: {}", label, error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    cancelAllOutputsBatch();
    return;
  }
  if (!screencopy::orientCaptureNative(*image, m_wayland, output)) {
    notifyError("Failed to scale screenshot");
    cancelAllOutputsBatch();
    return;
  }

  m_allOutputsBatch->frames.push_back(capture::FrozenScreenshot{.output = output, .image = std::move(*image)});
  DeferredCall::callLater([this]() { startNextAllOutputsCapture(); });
}

void ScreenshotService::finishAllOutputsBatch() {
  if (!m_allOutputsBatch) {
    return;
  }

  AllOutputsBatch batch = std::move(*m_allOutputsBatch);
  m_allOutputsBatch.reset();
  if (batch.frames.empty()) {
    notifyError("Screenshot failed");
    return;
  }

  std::vector<CapturedOutputFrame> frames;
  frames.reserve(batch.frames.size());
  for (auto& frame : batch.frames) {
    const auto* out = findOutput(m_wayland, frame.output);
    if (out == nullptr) {
      notifyError("Failed to combine screenshots");
      return;
    }
    frames.push_back(CapturedOutputFrame{.image = std::move(frame.image), .output = out});
  }

  auto stitched = stitchOutputFrames(std::move(frames));
  if (!stitched.has_value()) {
    notifyError("Failed to combine screenshots");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(batch.options) ? std::optional(makeScreenshotPath(batch.options, "desktop")) : std::nullopt;
  deliverCaptureResult(std::move(*stitched), batch.options, destPath);
}

void ScreenshotService::cancelAllOutputsBatch() {
  m_allOutputsBatch.reset();
  cancelGlobalRegionBatch();
}

void ScreenshotService::deliverCaptureResult(
    ScreencopyImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
) {
  std::string encodeError;
  std::vector<std::uint8_t> png = encodePng(image.rgba.data(), image.width, image.height, &encodeError);
  if (png.empty()) {
    kLog.warn("screenshot encode failed: {}", encodeError);
    notifyError(encodeError.empty() ? "Failed to encode screenshot" : encodeError);
    return;
  }

  bool delivered = false;
  std::string failureMessage;

  if (destPath.has_value()) {
    std::error_code ec;
    std::filesystem::create_directories(destPath->parent_path(), ec);
    if (ec) {
      kLog.warn("screenshot directory create failed: {}", destPath->parent_path().string());
    }
  }

  if (options.saveToFile && destPath.has_value()) {
    std::ofstream out(*destPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
      kLog.warn("screenshot write failed: {}", destPath->string());
      failureMessage = "Failed to save screenshot";
    } else {
      notifySaved(*destPath);
      delivered = true;
    }
  }

  if (options.copyToClipboard) {
    if (m_clipboard == nullptr || !m_clipboard->isAvailable()) {
      kLog.warn("screenshot clipboard copy skipped: clipboard unavailable");
      if (failureMessage.empty()) {
        failureMessage = "Clipboard is not available";
      }
    } else if (m_clipboard->copyImagePng(png)) {
      delivered = true;
    } else {
      kLog.warn("screenshot clipboard copy failed");
      if (failureMessage.empty()) {
        failureMessage = "Failed to copy screenshot to clipboard";
      }
    }
  }

  if (options.pipeToCommand && !options.pipeCommand.empty()) {
    pipePngToCommandAsync(options.pipeCommand, png, destPath);
    delivered = true;
  }

  if (!delivered) {
    notifyError(failureMessage.empty() ? "No screenshot output enabled" : failureMessage);
  }
}

void ScreenshotService::onCaptureComplete(
    std::optional<ScreencopyImage> image, const std::string& error, OutputOptions options,
    std::optional<std::filesystem::path> destPath, wl_output* output
) {
  if (!error.empty() || !image.has_value()) {
    kLog.warn("screenshot failed: {}", error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    startNextQueuedCapture();
    return;
  }

  if (!screencopy::orientCaptureNative(*image, m_wayland, output)) {
    notifyError("Failed to scale screenshot");
    startNextQueuedCapture();
    return;
  }

  deliverCaptureResult(std::move(*image), options, std::move(destPath));
  startNextQueuedCapture();
}

std::filesystem::path ScreenshotService::defaultPicturesDirectory() const {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / "Pictures";
  }
  return std::filesystem::path("/tmp");
}

std::filesystem::path ScreenshotService::outputDirectory(const OutputOptions& options) const {
  if (options.directory.empty()) {
    return defaultPicturesDirectory();
  }
  return FileUtils::expandUserPath(options.directory);
}

std::filesystem::path
ScreenshotService::makeScreenshotPath(const OutputOptions& options, const std::string& labelBase, int suffix) const {
  const auto dir = outputDirectory(options);
  const std::string stem = formatFilenameStem(options.filenamePattern, labelBase, suffix);
  return dir / (stem + ".png");
}

void ScreenshotService::notifySaved(const std::filesystem::path& path) {
  m_notifications.addInternal("Noctalia", "Screenshot saved", path.string());
}

void ScreenshotService::notifyError(const std::string& message) {
  m_notifications.addInternal("Noctalia", "Screenshot failed", message, Urgency::Critical);
}
