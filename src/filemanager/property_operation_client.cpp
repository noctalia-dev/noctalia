#include "filemanager/polkit_helper_launch.h"
#include "filemanager/property_operations.h"
#include "property_helper_paths.h"

#include <charconv>
#include <sstream>
#include <unistd.h>

namespace filemanager {
  namespace {
    constexpr PolkitHelperTarget kPropertiesHelperTarget{
        .helperPath = NYTHERA_PROPERTIES_HELPER_PATH,
        .policyPath = NYTHERA_PROPERTIES_POLICY_PATH,
        .actionId = "dev.nythera.Files.modify-properties",
        .displayName = "Properties",
    };

    PropertyResult privileged(const PropertyRequest& request, GCancellable* cancellable, PropertyProgress progress) {
      PropertyResult result;
      if (!verifyPolkitHelperReady(kPropertiesHelperTarget, cancellable, result.cancelled, result.error))
        return result;

      auto arguments = propertyArguments(request);
      auto* process = spawnPolkitHelperProcess(kPropertiesHelperTarget.helperPath, arguments, result.error);
      if (!process)
        return result;
      GError* error = nullptr;
      auto* input = g_subprocess_get_stdin_pipe(process);
      gsize written = 0;
      const bool ready = g_output_stream_write_all(input, "G", 1, &written, nullptr, &error);
      g_clear_error(&error);
      // EOF is the helper's cancellation/death signal, including cancellation during auth.
      const auto connection = g_cancellable_connect(
          cancellable, G_CALLBACK(+[](GCancellable*, gpointer data) {
            g_output_stream_close(G_OUTPUT_STREAM(data), nullptr, nullptr);
          }),
          input, nullptr
      );
      if (!ready)
        g_output_stream_close(input, nullptr, nullptr);
      auto* output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
      bool final = false;
      while (char* line = g_data_input_stream_read_line(output, nullptr, nullptr, &error)) {
        std::istringstream fields(line);
        char kind = 0;
        unsigned cancelled = 0, effects = 0;
        PropertyResult update;
        fields >> kind >> update.succeeded >> update.failed >> update.skipped;
        if (fields && kind == 'P') {
          result = update;
          if (progress)
            progress(update);
        } else if (fields && kind == 'R' && (fields >> cancelled >> effects) && cancelled <= 1 && effects <= 1) {
          std::string encoded;
          fields >> encoded;
          if (encoded != "-" && !encoded.empty()) {
            gsize size = 0;
            guchar* raw = g_base64_decode(encoded.c_str(), &size);
            update.error.assign(reinterpret_cast<char*>(raw), size);
            g_free(raw);
          }
          update.cancelled = cancelled != 0;
          update.ownershipEffects = effects != 0;
          result = std::move(update);
          final = true;
        }
        g_free(line);
      }
      g_clear_error(&error);
      g_subprocess_wait(process, nullptr, nullptr);
      g_cancellable_disconnect(cancellable, connection);
      g_output_stream_close(input, nullptr, nullptr);
      if (!final) {
        const auto code = g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : -1;
        if (code == 126 || code == 127)
          result = propertyAuthorizationFailure(code, g_cancellable_is_cancelled(cancellable));
        else {
          result.cancelled = g_cancellable_is_cancelled(cancellable);
          result.error = "The helper stopped without a final result. Counts may be incomplete; changes may already "
                         "have been applied.";
          if (!result.cancelled)
            ++result.failed;
        }
      }
      g_object_unref(output);
      g_object_unref(process);
      return result;
    }
  } // namespace
  bool savePropertyDefaultApplication(GAppInfo* app, const std::string& mime, GError** error) {
    if (!app || mime.empty()) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No application or MIME type selected.");
      return false;
    }
    GList* apps = g_app_info_get_all_for_type(mime.c_str());
    bool compatible = false;
    for (GList* entry = apps; entry; entry = entry->next)
      if (g_app_info_get_id(G_APP_INFO(entry->data)) && g_app_info_equal(app, G_APP_INFO(entry->data)))
        compatible = true;
    g_list_free_full(apps, g_object_unref);
    if (!compatible) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "The selected application is no longer installed or compatible with this MIME type."
      );
      return false;
    }
    return g_app_info_set_as_default_for_type(app, mime.c_str(), error);
  }

  PropertyResult runPropertyRequest(
      const PropertyRequest& request, std::uint32_t uid, GCancellable* cancellable, PropertyProgress progress,
      PropertyAuthorizer authorizer
  ) {
    PropertyResult result;
    if (const auto error = validatePropertyRequest(request); !error.empty()) {
      result.error = error;
      return result;
    }
    if (g_cancellable_is_cancelled(cancellable)) {
      result.cancelled = true;
      return result;
    }
    bool needsAuthorization = propertyNeedsAuthorization(request, uid);
    if (request.recursive && !needsAuthorization) {
      auto inspected = executePropertyRequest(
          request, [cancellable] { return g_cancellable_is_cancelled(cancellable); }, progress,
          {.depth = 128, .entries = 1000000, .inspectOnly = true}
      );
      if (inspected.cancelled)
        return inspected;
      needsAuthorization = inspected.authorizationRequired;
      if (!needsAuthorization && !inspected.ok())
        return inspected;
    }
    if (needsAuthorization)
      return (authorizer ? authorizer : privileged)(request, cancellable, std::move(progress));
    return executePropertyRequest(
        request, [cancellable] { return g_cancellable_is_cancelled(cancellable); }, std::move(progress)
    );
  }
} // namespace filemanager
