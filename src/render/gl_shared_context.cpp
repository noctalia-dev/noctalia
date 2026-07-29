#include "render/gl_shared_context.h"

#include "core/log.h"

#include <EGL/eglext.h>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifndef EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT 0x3138
#endif
#ifndef EGL_LOSE_CONTEXT_ON_RESET_EXT
#define EGL_LOSE_CONTEXT_ON_RESET_EXT 0x31BF
#endif
#ifndef EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV
#define EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV 0x334C
#endif

namespace {

  constexpr Logger kLog("gl");

  constexpr EGLint kConfigAttributes[] = {
      EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_NONE,
  };

  constexpr EGLint kPlainContextAttributes[] = {
      EGL_CONTEXT_CLIENT_VERSION,
      2,
      EGL_NONE,
  };

  bool hasExtension(const char* extensions, std::string_view name) {
    if (extensions == nullptr || name.empty()) {
      return false;
    }

    std::string_view list(extensions);
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

} // namespace

GlSharedContext::~GlSharedContext() { cleanup(); }

void GlSharedContext::initialize(wl_display* display, bool createSharedContext) {
  if (display == nullptr) {
    throw std::runtime_error("GlSharedContext requires a valid Wayland display");
  }

  m_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));
  if (m_display == EGL_NO_DISPLAY) {
    throw std::runtime_error("eglGetDisplay failed");
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (eglInitialize(m_display, &major, &minor) != EGL_TRUE) {
    throw std::runtime_error("eglInitialize failed");
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    throw std::runtime_error("eglBindAPI failed");
  }

  EGLint configCount = 0;
  if (eglChooseConfig(m_display, kConfigAttributes, &m_config, 1, &configCount) != EGL_TRUE || configCount != 1) {
    throw std::runtime_error("eglChooseConfig failed");
  }

  buildContextAttributes();

  m_sharedContextEnabled = createSharedContext;
  if (createSharedContext) {
    m_rootContext = createContext(EGL_NO_CONTEXT, "root");
    kLog.info("initialized EGL {}.{} with shared root context", major, minor);
  } else {
    kLog.info("initialized EGL {}.{} without shared context (isolated GPU contexts)", major, minor);
  }
}

void GlSharedContext::recreateRootContext() {
  if (m_display == EGL_NO_DISPLAY) {
    throw std::runtime_error("cannot recreate EGL root context before initialization");
  }

  eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (m_rootContext != EGL_NO_CONTEXT) {
    eglDestroyContext(m_display, m_rootContext);
    m_rootContext = EGL_NO_CONTEXT;
  }
  if (m_sharedContextEnabled) {
    m_rootContext = createContext(EGL_NO_CONTEXT, "root recovery");
  }
}

void GlSharedContext::buildContextAttributes() {
  const char* extensions = eglQueryString(m_display, EGL_EXTENSIONS);
  const bool hasRobustness = hasExtension(extensions, "EGL_EXT_create_context_robustness");
  const bool hasVideoMemoryPurge = hasExtension(extensions, "EGL_NV_robustness_video_memory_purge");

  m_contextAttributes.clear();
  m_contextAttributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
  m_contextAttributes.push_back(2);
  if (hasRobustness) {
    m_contextAttributes.push_back(EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT);
    m_contextAttributes.push_back(EGL_LOSE_CONTEXT_ON_RESET_EXT);
  }
  if (hasRobustness && hasVideoMemoryPurge) {
    m_contextAttributes.push_back(EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV);
    m_contextAttributes.push_back(EGL_TRUE);
  }
  m_contextAttributes.push_back(EGL_NONE);

  m_contextAttributesRobust = hasRobustness;
  m_resetNotificationEnabled = hasRobustness;
  m_videoMemoryPurgeNotificationEnabled = hasRobustness && hasVideoMemoryPurge;

  if (m_videoMemoryPurgeNotificationEnabled) {
    kLog.info("requesting robust EGL contexts with NVIDIA video-memory purge reset notification");
  } else if (m_resetNotificationEnabled) {
    kLog.info("requesting robust EGL contexts with graphics reset notification");
  } else {
    kLog.info("EGL robustness context extension unavailable; using plain GLES contexts");
  }
}

EGLContext GlSharedContext::createContext(EGLContext shareContext, std::string_view label) {
  EGLContext context = createContextWithCurrentAttributes(shareContext);
  if (context != EGL_NO_CONTEXT) {
    return context;
  }

  const EGLint robustError = eglGetError();
  if (!m_contextAttributesRobust || shareContext != EGL_NO_CONTEXT) {
    throw std::runtime_error(
        std::format("eglCreateContext ({}) failed (EGL error 0x{:04x})", label, static_cast<unsigned>(robustError))
    );
  }

  kLog.warn(
      "robust eglCreateContext ({}) failed (EGL error 0x{:04x}); retrying with plain GLES context", label,
      static_cast<unsigned>(robustError)
  );
  usePlainContextAttributes();
  context = createContextWithCurrentAttributes(shareContext);
  if (context == EGL_NO_CONTEXT) {
    throw std::runtime_error(
        std::format("eglCreateContext ({}) failed (EGL error 0x{:04x})", label, static_cast<unsigned>(eglGetError()))
    );
  }
  return context;
}

EGLContext GlSharedContext::createContextWithCurrentAttributes(EGLContext shareContext) const {
  return eglCreateContext(m_display, m_config, shareContext, m_contextAttributes.data());
}

void GlSharedContext::usePlainContextAttributes() noexcept {
  m_contextAttributes.assign(
      kPlainContextAttributes, kPlainContextAttributes + (sizeof(kPlainContextAttributes) / sizeof(EGLint))
  );
  m_contextAttributesRobust = false;
  m_resetNotificationEnabled = false;
  m_videoMemoryPurgeNotificationEnabled = false;
}

bool GlSharedContext::makeCurrentSurfaceless() const {
  if (m_display == EGL_NO_DISPLAY || m_rootContext == EGL_NO_CONTEXT) {
    return true;
  }
  if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_rootContext) != EGL_TRUE) {
    // Genuine context loss (e.g. NVIDIA video-memory purge on resume) makes this fail. Skip the
    // GPU work rather than throwing across the C ABI; graphicsResetStatus() drives the rebuild.
    kLog.warn(
        "eglMakeCurrent (root, surfaceless) failed (EGL error 0x{:04x}); skipping GPU work",
        static_cast<unsigned>(eglGetError())
    );
    return false;
  }
  return true;
}

void GlSharedContext::cleanup() {
  if (m_display == EGL_NO_DISPLAY) {
    return;
  }

  eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (m_rootContext != EGL_NO_CONTEXT) {
    eglDestroyContext(m_display, m_rootContext);
    m_rootContext = EGL_NO_CONTEXT;
  }

  eglTerminate(m_display);
  m_display = EGL_NO_DISPLAY;
  m_config = nullptr;
  m_contextAttributes.clear();
  m_contextAttributesRobust = false;
  m_resetNotificationEnabled = false;
  m_videoMemoryPurgeNotificationEnabled = false;
}
