#include "net/url_open.h"

#include "core/log.h"

#include <gio/gio.h>
#include <thread>

namespace {
  constexpr Logger kLog("url-open");

  struct BrowserLaunchContext {
    GAppLaunchContext parent;
    char* activationToken;
  };

  struct BrowserLaunchContextClass {
    GAppLaunchContextClass parent;
  };

  G_DEFINE_TYPE(BrowserLaunchContext, browser_launch_context, G_TYPE_APP_LAUNCH_CONTEXT)

  void browser_launch_context_init(BrowserLaunchContext* self) { self->activationToken = nullptr; }

  void browser_launch_context_class_init(BrowserLaunchContextClass* klass) {
    G_APP_LAUNCH_CONTEXT_CLASS(klass)->get_startup_notify_id = [](GAppLaunchContext* context, GAppInfo*,
                                                                  GList*) -> char* {
      return g_strdup(reinterpret_cast<BrowserLaunchContext*>(context)->activationToken);
    };
    G_OBJECT_CLASS(klass)->finalize = [](GObject* object) {
      g_free(reinterpret_cast<BrowserLaunchContext*>(object)->activationToken);
      G_OBJECT_CLASS(browser_launch_context_parent_class)->finalize(object);
    };
  }
} // namespace

namespace net {

  bool openInBrowser(const std::string& url, const std::string& activationToken) {
    if (url.empty()) {
      return false;
    }

    std::thread([url, activationToken] {
      auto* context = static_cast<BrowserLaunchContext*>(g_object_new(browser_launch_context_get_type(), nullptr));
      auto* launchContext = G_APP_LAUNCH_CONTEXT(context);
      g_app_launch_context_unsetenv(launchContext, "XDG_ACTIVATION_TOKEN");
      g_app_launch_context_unsetenv(launchContext, "DESKTOP_STARTUP_ID");
      if (!activationToken.empty()) {
        context->activationToken = g_strdup(activationToken.c_str());
        g_app_launch_context_setenv(launchContext, "XDG_ACTIVATION_TOKEN", activationToken.c_str());
        g_app_launch_context_setenv(launchContext, "DESKTOP_STARTUP_ID", activationToken.c_str());
      }

      // GIO passes the context token to portal and D-Bus launches as well as child processes.
      GError* error = nullptr;
      if (!g_app_info_launch_default_for_uri(url.c_str(), launchContext, &error)) {
        kLog.warn("default browser launch failed: {}", error != nullptr ? error->message : "unknown error");
      }
      g_clear_error(&error);
      g_object_unref(context);
    }).detach();
    return true;
  }

} // namespace net
