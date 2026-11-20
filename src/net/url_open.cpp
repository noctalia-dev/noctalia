#include "net/url_open.h"

#include "core/log.h"

#include <gio/gio.h>
#include <optional>
#include <thread>

namespace {
  constexpr Logger kLog("url-open");
  constexpr int kShowItemsTimeoutMs = 5000;

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

  // Blocking; callers run it on a detached thread.
  void launchDefaultForUri(const std::string& uri, const std::string& activationToken) {
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
    if (!g_app_info_launch_default_for_uri(uri.c_str(), launchContext, &error)) {
      kLog.warn("default handler launch failed: {}", error != nullptr ? error->message : "unknown error");
    }
    g_clear_error(&error);
    g_object_unref(context);
  }

  std::optional<std::string> fileUri(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute()) {
      return std::nullopt;
    }
    GError* error = nullptr;
    char* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    if (uri == nullptr) {
      kLog.warn("cannot build a file URI: {}", error != nullptr ? error->message : "unknown error");
      g_clear_error(&error);
      return std::nullopt;
    }
    std::string out(uri);
    g_free(uri);
    return out;
  }

  // Blocking; callers run it on a detached thread.
  bool showItemInFileManager(const std::string& uri, const std::string& activationToken) {
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (bus == nullptr) {
      kLog.debug("session bus unavailable: {}", error != nullptr ? error->message : "unknown error");
      g_clear_error(&error);
      return false;
    }

    const gchar* uris[] = {uri.c_str(), nullptr};
    // Default flags let D-Bus activate a file manager that is not running yet.
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1",
        "ShowItems", g_variant_new("(^ass)", uris, activationToken.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE,
        kShowItemsTimeoutMs, nullptr, &error
    );
    const bool ok = reply != nullptr;
    if (!ok) {
      kLog.debug("FileManager1.ShowItems failed: {}", error != nullptr ? error->message : "unknown error");
    }
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    g_clear_error(&error);
    g_object_unref(bus);
    return ok;
  }
} // namespace

namespace net {

  bool openInBrowser(const std::string& url, const std::string& activationToken) {
    if (url.empty()) {
      return false;
    }

    std::thread([url, activationToken] { launchDefaultForUri(url, activationToken); }).detach();
    return true;
  }

  bool openFile(const std::filesystem::path& path, const std::string& activationToken) {
    auto uri = fileUri(path);
    if (!uri.has_value()) {
      return false;
    }

    std::thread([uri = std::move(*uri), activationToken] { launchDefaultForUri(uri, activationToken); }).detach();
    return true;
  }

  bool revealFile(const std::filesystem::path& path, const std::string& activationToken) {
    auto uri = fileUri(path);
    auto parentUri = fileUri(path.parent_path());
    if (!uri.has_value() || !parentUri.has_value()) {
      return false;
    }

    std::thread([uri = std::move(*uri), parentUri = std::move(*parentUri), activationToken] {
      if (!showItemInFileManager(uri, activationToken)) {
        kLog.debug("opening the parent folder instead");
        launchDefaultForUri(parentUri, activationToken);
      }
    }).detach();
    return true;
  }

} // namespace net
