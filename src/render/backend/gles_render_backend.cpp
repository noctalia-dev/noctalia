#include "render/backend/gles_render_backend.h"

#include "core/log.h"
#include "render/backend/gles_framebuffer.h"
#include "render/core/shader_program.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_target.h"

#include <GLES2/gl2.h>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <wayland-egl.h>

#ifndef GL_GUILTY_CONTEXT_RESET
#define GL_GUILTY_CONTEXT_RESET 0x8253
#endif
#ifndef GL_INNOCENT_CONTEXT_RESET
#define GL_INNOCENT_CONTEXT_RESET 0x8254
#endif
#ifndef GL_UNKNOWN_CONTEXT_RESET
#define GL_UNKNOWN_CONTEXT_RESET 0x8255
#endif
#ifndef GL_PURGED_CONTEXT_RESET_NV
#define GL_PURGED_CONTEXT_RESET_NV 0x92BB
#endif

namespace {

  constexpr Logger kLog("render");
  constexpr float kSlowRenderOperationDebugMs = 50.0F;
  constexpr float kSlowRenderOperationWarnMs = 1000.0F;
  bool g_backendInfoLogged = false;
  bool g_capabilitiesVerified = false;

  constexpr char kFullscreenVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
varying vec2 v_texcoord;

void main() {
    v_texcoord = a_position;
    vec2 ndc = a_position * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

  constexpr char kFullscreenTextureFragmentShader[] = R"(
precision highp float;
uniform sampler2D u_texture;
uniform float u_flipY;
varying vec2 v_texcoord;

void main() {
    vec2 uv = v_texcoord;
    if (u_flipY > 0.5) {
        uv.y = 1.0 - uv.y;
    }
    gl_FragColor = texture2D(u_texture, uv);
}
)";

  constexpr char kFullscreenTintFragmentShader[] = R"(
precision mediump float;
uniform vec4 u_color;
varying vec2 v_texcoord;

void main() {
    gl_FragColor = u_color;
}
)";

  const char* safeCString(const char* value) { return value != nullptr ? value : "unknown"; }

  std::string eglErrorDetail(EGLint error) {
    if (error == EGL_SUCCESS) {
      return "no EGL error reported";
    }
    return std::format("EGL error 0x{:04x}", static_cast<unsigned>(error));
  }

  bool hasGlExtension(std::string_view name) {
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (extensions == nullptr || name.empty()) {
      return false;
    }

    const std::string_view list(extensions);
    std::size_t pos = 0;
    while (pos < list.size()) {
      while (pos < list.size() && list[pos] == ' ') {
        ++pos;
      }
      const std::size_t end = list.find(' ', pos);
      const std::string_view token = list.substr(pos, end == std::string_view::npos ? list.size() - pos : end - pos);
      if (token == name) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      pos = end + 1;
    }
    return false;
  }

  // Major version from a GLES GL_VERSION string ("OpenGL ES 3.2 Mesa 26.1.6"); 0 when unparseable.
  int glesMajorVersion() {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (version == nullptr) {
      return 0;
    }
    constexpr std::string_view kPrefix = "OpenGL ES ";
    const std::string_view text(version);
    const std::size_t start = text.find(kPrefix);
    if (start == std::string_view::npos) {
      return 0;
    }
    int major = 0;
    const auto parsed = std::from_chars(text.data() + start + kPrefix.size(), text.data() + text.size(), major);
    return parsed.ec == std::errc{} ? major : 0;
  }

  bool hasFragmentHighPrecision() {
    std::array<GLint, 2> range{};
    GLint precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range.data(), &precision);
    return precision != 0;
  }

  // The shaders are GLSL ES 1.00 but lean on features ES 2.0 leaves optional. Probe them up front
  // so an unsupported driver names the missing capability, instead of surfacing a raw compile log
  // from whichever program happens to draw first.
  void verifyRequiredGlCapabilities() {
    if (g_capabilitiesVerified) {
      return;
    }
    g_capabilitiesVerified = true;
    if (glesMajorVersion() >= 3) {
      return; // ES 3.0 makes all of the below core.
    }

    std::string missing;
    const auto require = [&missing](bool supported, std::string_view feature) {
      if (supported) {
        return;
      }
      if (!missing.empty()) {
        missing += "; ";
      }
      missing += feature;
    };
    require(hasGlExtension("GL_OES_standard_derivatives"), "GL_OES_standard_derivatives (rounded-rect antialiasing)");
    require(hasFragmentHighPrecision(), "highp float in fragment shaders (declared by every shader)");
    if (!missing.empty()) {
      throw std::runtime_error(
          std::format(
              R"(GPU driver is missing OpenGL ES features Noctalia requires: {} [renderer="{}" version="{}"])", missing,
              safeCString(reinterpret_cast<const char*>(glGetString(GL_RENDERER))),
              safeCString(reinterpret_cast<const char*>(glGetString(GL_VERSION)))
          )
      );
    }

    // Wallpapers upload with mipmaps, which ES 2.0 rejects on non-power-of-two textures without
    // this extension: the texture is incomplete and renders black.
    if (TextureManager::globalMipmapsEnabled() && !hasGlExtension("GL_OES_texture_npot")) {
      kLog.warn(
          "GPU driver lacks GL_OES_texture_npot; mipmapped non-power-of-two textures will not render. "
          "Set disable_mipmaps = true under [shell]"
      );
    }
  }

  bool isCurrentEglSurface(EGLDisplay display, EGLSurface surface) {
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE || eglGetCurrentDisplay() != display) {
      return false;
    }
    return eglGetCurrentSurface(EGL_DRAW) == surface || eglGetCurrentSurface(EGL_READ) == surface;
  }

  class GlesSurfaceTarget final : public RenderSurfaceTarget {
  public:
    GlesSurfaceTarget(EGLDisplay display, EGLConfig config, wl_surface* surface)
        : m_display(display), m_config(config), m_wlSurface(surface) {}
    ~GlesSurfaceTarget() override { destroy(); }

    GlesSurfaceTarget(const GlesSurfaceTarget&) = delete;
    GlesSurfaceTarget& operator=(const GlesSurfaceTarget&) = delete;

    void resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) override {
      if (bufferWidth == 0
          || bufferHeight == 0
          || m_wlSurface == nullptr
          || m_display == EGL_NO_DISPLAY
          || m_config == nullptr) {
        return;
      }

      if (m_window == nullptr) {
        m_window = wl_egl_window_create(m_wlSurface, static_cast<int>(bufferWidth), static_cast<int>(bufferHeight));
        if (m_window == nullptr) {
          return;
        }
      } else {
        wl_egl_window_resize(m_window, static_cast<int>(bufferWidth), static_cast<int>(bufferHeight), 0, 0);
      }

      if (m_surface == EGL_NO_SURFACE) {
        m_surface =
            eglCreateWindowSurface(m_display, m_config, reinterpret_cast<EGLNativeWindowType>(m_window), nullptr);
        if (m_surface == EGL_NO_SURFACE && !m_createFailureLogged) {
          const EGLint error = eglGetError();
          kLog.warn("eglCreateWindowSurface failed ({}); will retry before rendering", eglErrorDetail(error));
          m_createFailureLogged = true;
        } else if (m_surface != EGL_NO_SURFACE) {
          m_createFailureLogged = false;
        }
      }
    }

    void destroy() override {
      if (m_surface != EGL_NO_SURFACE) {
        if (isCurrentEglSurface(m_display, m_surface)) {
          const EGLContext currentContext = eglGetCurrentContext();
          if (currentContext != EGL_NO_CONTEXT
              && eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, currentContext) != EGL_TRUE) {
            const EGLint error = eglGetError();
            kLog.warn("eglMakeCurrent(EGL_NO_SURFACE) before surface destroy failed ({})", eglErrorDetail(error));
            if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
              const EGLint releaseError = eglGetError();
              kLog.warn(
                  "eglMakeCurrent(EGL_NO_CONTEXT) before surface destroy failed ({})", eglErrorDetail(releaseError)
              );
            }
          }
        }
        eglDestroySurface(m_display, m_surface);
        m_surface = EGL_NO_SURFACE;
      }

      if (m_window != nullptr) {
        wl_egl_window_destroy(m_window);
        m_window = nullptr;
      }

      m_wlSurface = nullptr;
    }

    [[nodiscard]] bool isReady() const noexcept override { return m_surface != EGL_NO_SURFACE; }
    [[nodiscard]] EGLSurface eglSurface() const noexcept { return m_surface; }

  private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    wl_surface* m_wlSurface = nullptr;
    wl_egl_window* m_window = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    bool m_createFailureLogged = false;
  };

  GlesSurfaceTarget& glesSurfaceTarget(RenderTarget& target) {
    auto* surface = dynamic_cast<GlesSurfaceTarget*>(target.surfaceTarget());
    if (surface == nullptr || !surface->isReady()) {
      throw std::runtime_error("GLES backend received an incompatible or unready surface target");
    }
    return *surface;
  }

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename... Args> void logSlowRenderOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowRenderOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowRenderOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

} // namespace

GlesRenderBackend::~GlesRenderBackend() { cleanup(); }

void GlesRenderBackend::initialize(GlSharedContext& shared) {
  m_display = shared.display();
  m_config = shared.config();

  m_context = shared.createContext(shared.rootContext(), "render");
  if (m_context == EGL_NO_CONTEXT) {
    throw std::runtime_error(
        std::format("eglCreateContext failed (EGL error 0x{:04x})", static_cast<unsigned>(eglGetError()))
    );
  }

  // Make context current (surfaceless) so GL resources can be created eagerly.
  if (!makeCurrentNoSurface()) {
    throw std::runtime_error("new render context could not be made current");
  }

  if (!g_backendInfoLogged) {
    kLog.info(
        R"(EGL vendor="{}" version="{}" APIs="{}")", safeCString(eglQueryString(m_display, EGL_VENDOR)),
        safeCString(eglQueryString(m_display, EGL_VERSION)), safeCString(eglQueryString(m_display, EGL_CLIENT_APIS))
    );
    kLog.info(
        R"(OpenGL ES vendor="{}" renderer="{}" version="{}")",
        safeCString(reinterpret_cast<const char*>(glGetString(GL_VENDOR))),
        safeCString(reinterpret_cast<const char*>(glGetString(GL_RENDERER))),
        safeCString(reinterpret_cast<const char*>(glGetString(GL_VERSION)))
    );
    g_backendInfoLogged = true;
  }

  verifyRequiredGlCapabilities();

  resolveGraphicsResetStatusProc();
  m_viewportValid = false;
  m_blendMode.reset();
  m_scissorEnabled = false;
  m_scissorValid = false;
}

bool GlesRenderBackend::makeCurrentNoSurface() {
  if (m_display == EGL_NO_DISPLAY || m_context == EGL_NO_CONTEXT) {
    return false;
  }

  // Texture work requested while rendering can use the context's current draw
  // surface. Rebinding it surfacelessly here would detach that surface before
  // the frame's trailing eglSwapBuffers.
  if (eglGetCurrentDisplay() == m_display && eglGetCurrentContext() == m_context) {
    return true;
  }

  if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) != EGL_TRUE) {
    // Genuine context loss (e.g. NVIDIA video-memory purge on resume) makes this fail. Skip the
    // GPU work rather than throwing across the C ABI; graphicsResetStatus() drives the rebuild.
    kLog.warn("eglMakeCurrent(EGL_NO_SURFACE) failed ({}); skipping GPU work", eglErrorDetail(eglGetError()));
    return false;
  }
  return true;
}

bool GlesRenderBackend::makeCurrent(RenderTarget& target) {
  auto& surface = glesSurfaceTarget(target);
  if (eglGetCurrentDisplay() == m_display
      && eglGetCurrentContext() == m_context
      && eglGetCurrentSurface(EGL_DRAW) == surface.eglSurface()
      && eglGetCurrentSurface(EGL_READ) == surface.eglSurface()) {
    return true;
  }

  const auto start = std::chrono::steady_clock::now();
  if (eglMakeCurrent(m_display, surface.eglSurface(), surface.eglSurface(), m_context) != EGL_TRUE) {
    // Same teardown hazard as endFrame's swap: the surface can be invalidated by
    // the compositor and eglMakeCurrent returns EGL_FALSE. Skip the frame rather
    // than killing the shell; genuine context loss is caught via graphicsResetStatus().
    kLog.warn("eglMakeCurrent failed ({}); skipping frame", eglErrorDetail(eglGetError()));
    return false;
  }
  float ms = elapsedSince(start);
  logSlowRenderOperation(ms, "eglMakeCurrent took {:.1F}ms", ms);

  // Non-blocking swap: pacing is driven by wl_surface.frame callbacks, not by
  // eglSwapBuffers. Default interval=1 can block indefinitely when the
  // compositor holds our buffer.
  const auto intervalStart = std::chrono::steady_clock::now();
  eglSwapInterval(m_display, 0);
  ms = elapsedSince(intervalStart);
  logSlowRenderOperation(ms, "eglSwapInterval(0) took {:.1F}ms", ms);
  return true;
}

bool GlesRenderBackend::beginFrame(RenderTarget& target) {
  if (!makeCurrent(target)) {
    return false;
  }

  setViewport(target.bufferWidth(), target.bufferHeight());
  setBlendMode(RenderBlendMode::PremultipliedAlpha);
  disableScissor();
  clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
  return true;
}

void GlesRenderBackend::endFrame(RenderTarget& target) {
  auto& surface = glesSurfaceTarget(target);
  const auto swapStart = std::chrono::steady_clock::now();
  if (eglSwapBuffers(m_display, surface.eglSurface()) != EGL_TRUE) {
    // A failed swap is not fatal: during compositor teardown (session logout,
    // output removal) the wl_egl_window backing buffer can be invalidated and
    // eglSwapBuffers returns EGL_FALSE, sometimes with EGL_SUCCESS. Genuine GPU
    // context loss is detected separately via graphicsResetStatus(). Skip this
    // frame instead of killing the shell.
    kLog.warn("eglSwapBuffers failed ({}); skipping frame", eglErrorDetail(eglGetError()));
    return;
  }
  const float ms = elapsedSince(swapStart);
  logSlowRenderOperation(
      ms, "eglSwapBuffers took {:.1F}ms ({}x{} logical, {}x{} buffer)", ms, target.logicalWidth(),
      target.logicalHeight(), target.bufferWidth(), target.bufferHeight()
  );
}

RenderGraphicsResetStatus GlesRenderBackend::graphicsResetStatus() {
  if (m_graphicsResetStatus == nullptr) {
    return RenderGraphicsResetStatus::NoError;
  }

  const GLenum status = m_graphicsResetStatus();
  switch (status) {
  case GL_NO_ERROR:
    return RenderGraphicsResetStatus::NoError;
  case GL_GUILTY_CONTEXT_RESET:
    return RenderGraphicsResetStatus::Guilty;
  case GL_INNOCENT_CONTEXT_RESET:
    return RenderGraphicsResetStatus::Innocent;
  case GL_UNKNOWN_CONTEXT_RESET:
    return RenderGraphicsResetStatus::Unknown;
  case GL_PURGED_CONTEXT_RESET_NV:
    return RenderGraphicsResetStatus::Purged;
  default:
    return RenderGraphicsResetStatus::Other;
  }
}

void GlesRenderBackend::invalidateGpuResources() {
  if (m_display == EGL_NO_DISPLAY || m_context == EGL_NO_CONTEXT) {
    return;
  }
  if (eglGetCurrentDisplay() != m_display || eglGetCurrentContext() != m_context) {
    if (!makeCurrentNoSurface()) {
      kLog.warn("skipping GPU resource invalidation: could not make context current");
      return;
    }
  }
  destroyGpuObjects();
}

void GlesRenderBackend::abandonAfterGraphicsReset() noexcept {
  // Every object in the dead share group is already invalid, and by the time the second
  // backend gets here the first one has left no context current. Forget the GL names
  // instead of issuing deletes that would run with no context bound.
  m_textureManager.abandonGpuResources();
  abandonGpuObjects();

  if (m_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (m_context != EGL_NO_CONTEXT && m_display != EGL_NO_DISPLAY) {
    eglDestroyContext(m_display, m_context);
  }

  m_display = EGL_NO_DISPLAY;
  m_config = nullptr;
  m_context = EGL_NO_CONTEXT;
  m_graphicsResetStatus = nullptr;
  m_maxTextureSize = 0;
}

std::unique_ptr<RenderSurfaceTarget> GlesRenderBackend::createSurfaceTarget(wl_surface* surface) {
  return std::make_unique<GlesSurfaceTarget>(m_display, m_config, surface);
}

std::unique_ptr<RenderFramebuffer> GlesRenderBackend::createFramebuffer(std::uint32_t width, std::uint32_t height) {
  auto framebuffer = std::make_unique<GlesFramebuffer>();
  if (!framebuffer->create(m_textureManager, width, height)) {
    return nullptr;
  }
  return framebuffer;
}

void GlesRenderBackend::bindFramebuffer(const RenderFramebuffer& framebuffer) {
  const auto* glesFramebuffer = dynamic_cast<const GlesFramebuffer*>(&framebuffer);
  if (glesFramebuffer == nullptr || !glesFramebuffer->valid()) {
    throw std::runtime_error("GLES backend received an incompatible framebuffer");
  }
  glesFramebuffer->bind();
}

void GlesRenderBackend::bindDefaultFramebuffer() { GlesFramebuffer::bindDefault(); }

void GlesRenderBackend::setViewport(std::uint32_t width, std::uint32_t height) {
  if (m_viewportValid && m_viewportWidth == width && m_viewportHeight == height) {
    return;
  }
  glViewport(0, 0, static_cast<GLint>(width), static_cast<GLint>(height));
  m_viewportWidth = width;
  m_viewportHeight = height;
  m_viewportValid = true;
}

void GlesRenderBackend::clear(Color color) {
  glClearColor(color.r, color.g, color.b, color.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void GlesRenderBackend::setBlendMode(RenderBlendMode mode) {
  if (m_blendMode == mode) {
    return;
  }
  switch (mode) {
  case RenderBlendMode::Disabled:
    glDisable(GL_BLEND);
    break;
  case RenderBlendMode::StraightAlpha:
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    break;
  case RenderBlendMode::PremultipliedAlpha:
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    break;
  }
  m_blendMode = mode;
}

int GlesRenderBackend::maxTextureSize() {
  if (m_maxTextureSize <= 0) {
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
    if (m_maxTextureSize <= 0) {
      m_maxTextureSize = 2048;
    }
  }
  return m_maxTextureSize;
}

void GlesRenderBackend::drawFullscreenQuad(const ShaderProgram& program) {
  const GLint posAttr = glGetAttribLocation(program.id(), "a_position");
  if (posAttr < 0) {
    return;
  }

  static constexpr float kQuad[] = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };
  glVertexAttribPointer(static_cast<GLuint>(posAttr), 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(static_cast<GLuint>(posAttr));
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(static_cast<GLuint>(posAttr));
}

void GlesRenderBackend::setScissor(RenderScissor scissor) {
  if (!m_scissorEnabled) {
    glEnable(GL_SCISSOR_TEST);
    m_scissorEnabled = true;
  }
  if (!m_scissorValid
      || m_scissor.x != scissor.x
      || m_scissor.y != scissor.y
      || m_scissor.width != scissor.width
      || m_scissor.height != scissor.height) {
    glScissor(
        static_cast<GLint>(scissor.x), static_cast<GLint>(scissor.y), static_cast<GLsizei>(scissor.width),
        static_cast<GLsizei>(scissor.height)
    );
    m_scissor = scissor;
    m_scissorValid = true;
  }
}

void GlesRenderBackend::disableScissor() {
  if (!m_scissorEnabled) {
    return;
  }
  glDisable(GL_SCISSOR_TEST);
  m_scissorEnabled = false;
}

void GlesRenderBackend::drawRect(
    float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
    const Mat3& transform
) {
  m_rectProgram.ensureInitialized();
  m_rectProgram.draw(surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawImage(const RenderImageDraw& draw) {
  m_imageProgram.ensureInitialized();
  m_imageProgram.draw(
      draw.texture, draw.surfaceWidth, draw.surfaceHeight, draw.width, draw.height, draw.tint, draw.monochromeTint,
      draw.alphaMaskTint, draw.opacity, draw.radius, draw.borderColor, draw.borderWidth, static_cast<int>(draw.fitMode),
      draw.textureWidth, draw.textureHeight, draw.transform, draw.scrim
  );
}

void GlesRenderBackend::drawGlyph(const RenderGlyphDraw& draw) {
  if (draw.texture == 0) {
    return;
  }

  m_glyphProgram.ensureInitialized();
  if (draw.tinted) {
    m_glyphProgram.drawTinted(
        draw.texture, draw.surfaceWidth, draw.surfaceHeight, draw.width, draw.height, draw.u0, draw.v0, draw.u1,
        draw.v1, draw.opacity, draw.tint, draw.transform
    );
    return;
  }

  m_glyphProgram.draw(
      draw.texture, draw.surfaceWidth, draw.surfaceHeight, draw.width, draw.height, draw.u0, draw.v0, draw.u1, draw.v1,
      draw.opacity, draw.transform
  );
}

void GlesRenderBackend::drawSpinner(
    float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style, const Mat3& transform
) {
  m_spinnerProgram.ensureInitialized();
  m_spinnerProgram.draw(surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawCountdownRing(
    float surfaceWidth, float surfaceHeight, float width, float height, const CountdownRingStyle& style,
    const Mat3& transform
) {
  m_countdownRingProgram.ensureInitialized();
  m_countdownRingProgram.draw(surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawScreenCorner(
    float surfaceWidth, float surfaceHeight, float width, float height, const ScreenCornerStyle& style,
    const Mat3& transform
) {
  m_screenCornerProgram.ensureInitialized();
  m_screenCornerProgram.draw(surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawAudioSpectrum(
    float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
    const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
) {
  m_audioSpectrumProgram.ensureInitialized();
  m_audioSpectrumProgram.draw(
      surfaceWidth, surfaceHeight, pixelScaleX, pixelScaleY, width, height, style, values, transform
  );
}

void GlesRenderBackend::drawFancyAudioVisualizer(
    TextureId audioTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
    const FancyAudioVisualizerStyle& style, const Mat3& transform
) {
  (void)textureWidth;
  m_fancyAudioVisualizerProgram.ensureInitialized();
  m_fancyAudioVisualizerProgram.draw(audioTexture, surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawEffect(
    float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style, const Mat3& transform
) {
  m_effectProgram.ensureInitialized();
  m_effectProgram.draw(surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawGraph(
    TextureId dataTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
    const GraphStyle& style, const Mat3& transform
) {
  m_graphProgram.ensureInitialized();
  m_graphProgram.draw(dataTexture, textureWidth, surfaceWidth, surfaceHeight, width, height, style, transform);
}

void GlesRenderBackend::drawWallpaper(const WallpaperDrawParams& params) {
  m_wallpaperProgram.ensureInitialized();
  m_wallpaperProgram.draw(params);
}

void GlesRenderBackend::drawFullscreenTexture(TextureId texture, bool flipY) {
  if (texture == 0) {
    return;
  }

  ensureFullscreenTextureProgram();
  glUseProgram(m_fullscreenTextureProgram.id());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.value()));

  const GLint textureLoc = glGetUniformLocation(m_fullscreenTextureProgram.id(), "u_texture");
  const GLint flipLoc = glGetUniformLocation(m_fullscreenTextureProgram.id(), "u_flipY");
  glUniform1i(textureLoc, 0);
  glUniform1f(flipLoc, flipY ? 1.0F : 0.0F);
  drawFullscreenQuad(m_fullscreenTextureProgram);
}

void GlesRenderBackend::drawFullscreenTint(Color color) {
  ensureFullscreenTintProgram();
  glUseProgram(m_fullscreenTintProgram.id());
  const GLint colorLoc = glGetUniformLocation(m_fullscreenTintProgram.id(), "u_color");
  glUniform4f(colorLoc, color.r, color.g, color.b, color.a);
  drawFullscreenQuad(m_fullscreenTintProgram);
}

void GlesRenderBackend::drawFramebufferBlur(
    TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY, float radius
) {
  if (sourceTexture == 0) {
    return;
  }

  m_blurProgram.ensureInitialized();
  m_blurProgram.draw(sourceTexture, width, height, directionX, directionY, radius);
}

void GlesRenderBackend::ensureFullscreenTextureProgram() {
  if (!m_fullscreenTextureProgram.isValid()) {
    m_fullscreenTextureProgram.create(kFullscreenVertexShader, kFullscreenTextureFragmentShader);
  }
}

void GlesRenderBackend::ensureFullscreenTintProgram() {
  if (!m_fullscreenTintProgram.isValid()) {
    m_fullscreenTintProgram.create(kFullscreenVertexShader, kFullscreenTintFragmentShader);
  }
}

void GlesRenderBackend::resolveGraphicsResetStatusProc() {
  m_graphicsResetStatus = reinterpret_cast<GraphicsResetStatusProc>(eglGetProcAddress("glGetGraphicsResetStatus"));
  if (m_graphicsResetStatus == nullptr) {
    m_graphicsResetStatus = reinterpret_cast<GraphicsResetStatusProc>(eglGetProcAddress("glGetGraphicsResetStatusKHR"));
  }
  if (m_graphicsResetStatus == nullptr) {
    m_graphicsResetStatus = reinterpret_cast<GraphicsResetStatusProc>(eglGetProcAddress("glGetGraphicsResetStatusEXT"));
  }

  if (!m_resetStatusLogged) {
    if (m_graphicsResetStatus != nullptr) {
      const bool purge = hasGlExtension("GL_NV_robustness_video_memory_purge");
      kLog.info("graphics reset status polling enabled{}", purge ? " with NVIDIA video-memory purge status" : "");
    } else {
      kLog.info("graphics reset status polling unavailable");
    }
    m_resetStatusLogged = true;
  }
}

void GlesRenderBackend::destroyGpuObjects() {
  m_rectProgram.destroy();
  m_imageProgram.destroy();
  m_glyphProgram.destroy();
  m_spinnerProgram.destroy();
  m_countdownRingProgram.destroy();
  m_screenCornerProgram.destroy();
  m_audioSpectrumProgram.destroy();
  m_fancyAudioVisualizerProgram.destroy();
  m_effectProgram.destroy();
  m_graphProgram.destroy();
  m_wallpaperProgram.destroy();
  m_blurProgram.destroy();
  m_fullscreenTextureProgram.destroy();
  m_fullscreenTintProgram.destroy();
  m_textureManager.cleanup();
}

void GlesRenderBackend::abandonGpuObjects() noexcept {
  m_rectProgram.abandon();
  m_imageProgram.abandon();
  m_glyphProgram.abandon();
  m_spinnerProgram.abandon();
  m_countdownRingProgram.abandon();
  m_screenCornerProgram.abandon();
  m_audioSpectrumProgram.abandon();
  m_fancyAudioVisualizerProgram.abandon();
  m_effectProgram.abandon();
  m_graphProgram.abandon();
  m_wallpaperProgram.abandon();
  m_blurProgram.abandon();
  m_fullscreenTextureProgram.abandon();
  m_fullscreenTintProgram.abandon();
}

void GlesRenderBackend::cleanup() {
  if (m_display != EGL_NO_DISPLAY && m_context != EGL_NO_CONTEXT) {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
  }

  destroyGpuObjects();

  if (m_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  if (m_context != EGL_NO_CONTEXT && m_display != EGL_NO_DISPLAY) {
    eglDestroyContext(m_display, m_context);
  }

  m_display = EGL_NO_DISPLAY;
  m_config = nullptr;
  m_context = EGL_NO_CONTEXT;
  m_graphicsResetStatus = nullptr;
  m_resetStatusLogged = false;
  m_maxTextureSize = 0;
}
