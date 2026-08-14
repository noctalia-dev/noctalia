#include "net/default_app_open.h"

#include "core/log.h"

#include <gio/gio.h>

namespace {
  constexpr Logger kLog("default-app-open");
}

namespace net {

  bool openDefaultAppForMimeType(const std::string& mimeType) {
    GAppInfo* appInfo = g_app_info_get_default_for_type(mimeType.c_str(), FALSE);
    if (appInfo == nullptr) {
      kLog.warn("no default application set for mime type: {}", mimeType);
      return false;
    }

    GError* error = nullptr;
    const gboolean launched = g_app_info_launch(appInfo, nullptr, nullptr, &error);
    if (error != nullptr) {
      kLog.warn("failed to launch default application for {}: {}", mimeType, error->message);
      g_error_free(error);
    }
    g_object_unref(appInfo);
    return launched == TRUE;
  }

} // namespace net
