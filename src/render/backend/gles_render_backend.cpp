#include "render/backend/gles_render_backend.h"

#include "core/log.h"
#include "render/backend/gles_framebuffer.h"
#include "render/core/shader_program.h"
#include "render/gl_shared_context.h"
#include "render/render_target.h"

#include <GLES2/gl2.h>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>
#include <wayland-egl.h>

namespace {

  constexpr Logger kLog("render");
  constexpr float kSlowRenderOperationDebugMs = 50.0f;
  constexpr float kSlowRenderOperationWarnMs = 1000.0f;
  bool g_backendInfoLogged = false;

  constexpr EGLint kContextAttributes[] = {
      EGL_CONTEXT_CLIENT_VERSION,
      2,
      EGL_NONE,
  };

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
      if (bufferWidth == 0 || bufferHeight == 0 || m_wlSurface == nullptr || m_display == EGL_NO_DISPLAY ||
          m_config == nullptr) {
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
          kLog.warn(
              "eglCreateWindowSurface failed (EGL error 0x{:04x}); will retry before rendering",
              static_cast<unsigned>(error)
          );
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
          if (currentContext != EGL_NO_CONTEXT &&
              eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, currentContext) != EGL_TRUE) {
            const EGLint error = eglGetError();
            kLog.warn(
                "eglMakeCurrent(EGL_NO_SURFACE) before surface destroy failed (EGL error 0x{:04x})",
                static_cast<unsigned>(error)
            );
            if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
              const EGLint releaseError = eglGetError();
              kLog.warn(
                  "eglMakeCurrent(EGL_NO_CONTEXT) before surface destroy failed (EGL error 0x{:04x})",
                  static_cast<unsigned>(releaseError)
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

  m_context = eglCreateContext(m_display, m_config, shared.rootContext(), kContextAttributes);
  if (m_context == EGL_NO_CONTEXT) {
    throw std::runtime_error("eglCreateContext failed");
  }

  // Make context current (surfaceless) so GL resources can be created eagerly.
  makeCurrentNoSurface();

  if (!g_backendInfoLogged) {
    kLog.info(
        "EGL vendor=\"{}\" version=\"{}\" APIs=\"{}\"", safeCString(eglQueryString(m_display, EGL_VENDOR)),
        safeCString(eglQueryString(m_display, EGL_VERSION)), safeCString(eglQueryString(m_display, EGL_CLIENT_APIS))
    );
    kLog.info(
        "OpenGL ES vendor=\"{}\" renderer=\"{}\" version=\"{}\"",
        safeCString(reinterpret_cast<const char*>(glGetString(GL_VENDOR))),
        safeCString(reinterpret_cast<const char*>(glGetString(GL_RENDERER))),
        safeCString(reinterpret_cast<const char*>(glGetString(GL_VERSION)))
    );
    g_backendInfoLogged = true;
  }
}

void GlesRenderBackend::makeCurrentNoSurface() {
  if (m_display == EGL_NO_DISPLAY || m_context == EGL_NO_CONTEXT) {
    return;
  }

  if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) != EGL_TRUE) {
    throw std::runtime_error("eglMakeCurrent(EGL_NO_SURFACE) failed");
  }
}

void GlesRenderBackend::makeCurrent(RenderTarget& target) {
  auto& surface = glesSurfaceTarget(target);
  const auto start = std::chrono::steady_clock::now();
  if (eglMakeCurrent(m_display, surface.eglSurface(), surface.eglSurface(), m_context) != EGL_TRUE) {
    throw std::runtime_error("eglMakeCurrent failed");
  }
  float ms = elapsedSince(start);
  logSlowRenderOperation(ms, "eglMakeCurrent took {:.1f}ms", ms);

  // Non-blocking swap: pacing is driven by wl_surface.frame callbacks, not by
  // eglSwapBuffers. Default interval=1 can block indefinitely when the
  // compositor holds our buffer.
  const auto intervalStart = std::chrono::steady_clock::now();
  eglSwapInterval(m_display, 0);
  ms = elapsedSince(intervalStart);
  logSlowRenderOperation(ms, "eglSwapInterval(0) took {:.1f}ms", ms);
}

void GlesRenderBackend::beginFrame(RenderTarget& target) {
  makeCurrent(target);

  setViewport(target.bufferWidth(), target.bufferHeight());
  setBlendMode(RenderBlendMode::PremultipliedAlpha);
  disableScissor();
  clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
}

void GlesRenderBackend::endFrame(RenderTarget& target) {
  auto& surface = glesSurfaceTarget(target);
  const auto swapStart = std::chrono::steady_clock::now();
  if (eglSwapBuffers(m_display, surface.eglSurface()) != EGL_TRUE) {
    throw std::runtime_error("eglSwapBuffers failed");
  }
  const float ms = elapsedSince(swapStart);
  logSlowRenderOperation(
      ms, "eglSwapBuffers took {:.1f}ms ({}x{} logical, {}x{} buffer)", ms, target.logicalWidth(),
      target.logicalHeight(), target.bufferWidth(), target.bufferHeight()
  );
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
  glViewport(0, 0, static_cast<GLint>(width), static_cast<GLint>(height));
}

void GlesRenderBackend::clear(Color color) {
  glClearColor(color.r, color.g, color.b, color.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void GlesRenderBackend::setBlendMode(RenderBlendMode mode) {
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
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };
  glVertexAttribPointer(static_cast<GLuint>(posAttr), 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(static_cast<GLuint>(posAttr));
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(static_cast<GLuint>(posAttr));
}

void GlesRenderBackend::setScissor(RenderScissor scissor) {
  glEnable(GL_SCISSOR_TEST);
  glScissor(
      static_cast<GLint>(scissor.x), static_cast<GLint>(scissor.y), static_cast<GLsizei>(scissor.width),
      static_cast<GLsizei>(scissor.height)
  );
}

void GlesRenderBackend::disableScissor() { glDisable(GL_SCISSOR_TEST); }

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
      draw.texture, draw.surfaceWidth, draw.surfaceHeight, draw.width, draw.height, draw.tint, draw.opacity,
      draw.radius, draw.borderColor, draw.borderWidth, static_cast<int>(draw.fitMode), draw.textureWidth,
      draw.textureHeight, draw.transform
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

void GlesRenderBackend::drawScreenCorner(
    float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
    const ScreenCornerStyle& style, const Mat3& transform
) {
  m_screenCornerProgram.ensureInitialized();
  m_screenCornerProgram.draw(surfaceWidth, surfaceHeight, pixelScaleX, pixelScaleY, width, height, style, transform);
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

void GlesRenderBackend::drawWallpaper(
    WallpaperTransition transition, WallpaperSourceKind sourceKind1, TextureId texture1, const Color& sourceColor1,
    WallpaperSourceKind sourceKind2, TextureId texture2, const Color& sourceColor2, float surfaceWidth,
    float surfaceHeight, float width, float height, float imageWidth1, float imageHeight1, float imageWidth2,
    float imageHeight2, float progress, float fillMode, const TransitionParams& params, const Color& fillColor,
    const Mat3& transform
) {
  m_wallpaperProgram.ensureInitialized();
  m_wallpaperProgram.draw(
      transition, sourceKind1, texture1, sourceColor1, sourceKind2, texture2, sourceColor2, surfaceWidth, surfaceHeight,
      width, height, imageWidth1, imageHeight1, imageWidth2, imageHeight2, progress, fillMode, params, fillColor,
      transform
  );
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
  glUniform1f(flipLoc, flipY ? 1.0f : 0.0f);
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

void GlesRenderBackend::cleanup() {
  if (m_display != EGL_NO_DISPLAY && m_context != EGL_NO_CONTEXT) {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
  }

  m_rectProgram.destroy();
  m_imageProgram.destroy();
  m_glyphProgram.destroy();
  m_spinnerProgram.destroy();
  m_screenCornerProgram.destroy();
  m_audioSpectrumProgram.destroy();
  m_effectProgram.destroy();
  m_graphProgram.destroy();
  m_wallpaperProgram.destroy();
  m_blurProgram.destroy();
  m_fullscreenTextureProgram.destroy();
  m_fullscreenTintProgram.destroy();
  m_textureManager.cleanup();

  if (m_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  if (m_context != EGL_NO_CONTEXT && m_display != EGL_NO_DISPLAY) {
    eglDestroyContext(m_display, m_context);
  }

  m_display = EGL_NO_DISPLAY;
  m_config = nullptr;
  m_context = EGL_NO_CONTEXT;
  m_maxTextureSize = 0;
}
