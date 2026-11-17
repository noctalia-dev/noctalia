#include "filemanager/file_transfer_payload.h"

#include <gio/gio.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace filemanager {
  namespace {
    constexpr auto uriListMime = "text/uri-list";
    constexpr auto gnomeMime = "x-special/gnome-copied-files";
    constexpr auto kdeMime = "application/x-kde-cutselection";

    [[nodiscard]] std::string_view nextLine(std::string_view& remaining) {
      const auto end = remaining.find('\n');
      auto line = remaining.substr(0, end);
      remaining = end == std::string_view::npos ? std::string_view{} : remaining.substr(end + 1);
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      return line;
    }

    [[nodiscard]] std::string toLocalPath(std::string_view uri) {
      if (uri.empty() || uri.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Empty or invalid file URI");

      const std::string uriString(uri);
      GError* error = nullptr;
      GUri* parsed = g_uri_parse(uriString.c_str(), G_URI_FLAGS_NONE, &error);
      if (!parsed) {
        const std::string message = error ? error->message : "Invalid file URI";
        g_clear_error(&error);
        throw std::invalid_argument(message);
      }

      const char* scheme = g_uri_get_scheme(parsed);
      const char* host = g_uri_get_host(parsed);
      const bool localAuthority = !host || !*host || g_ascii_strcasecmp(host, "localhost") == 0;
      const bool valid = scheme
          && g_ascii_strcasecmp(scheme, "file") == 0
          && localAuthority
          && !g_uri_get_userinfo(parsed)
          && g_uri_get_port(parsed) == -1
          && !g_uri_get_query(parsed)
          && !g_uri_get_fragment(parsed);
      g_uri_unref(parsed);
      if (!valid)
        throw std::invalid_argument("Only local file URIs are supported");

      gchar* hostname = nullptr;
      gchar* path = g_filename_from_uri(uriString.c_str(), &hostname, &error);
      if (!path || (hostname && *hostname && g_ascii_strcasecmp(hostname, "localhost") != 0)) {
        const std::string message = error ? error->message : "Invalid local file URI";
        g_free(path);
        g_free(hostname);
        g_clear_error(&error);
        throw std::invalid_argument(message);
      }
      std::string result(path);
      g_free(path);
      g_free(hostname);
      return result;
    }

    void appendUriList(std::string_view contents, FileTransferPayload& result) {
      while (!contents.empty()) {
        const auto line = nextLine(contents);
        if (line.empty() || line.front() == '#')
          continue;
        result.paths.push_back(toLocalPath(line));
      }
    }
  } // namespace

  std::map<std::string, std::string> encodeFileTransfer(std::vector<std::string> paths, bool cut) {
    std::string uriList;
    std::string gnomeList = cut ? "cut\n" : "copy\n";
    for (const auto& path : paths) {
      if (path.find('\0') != std::string::npos)
        throw std::invalid_argument("Path contains a NUL byte");
      GError* error = nullptr;
      gchar* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
      if (!uri) {
        const std::string message = error ? error->message : "Path cannot be represented as a file URI";
        g_clear_error(&error);
        throw std::invalid_argument(message);
      }
      uriList += uri;
      uriList += "\r\n";
      gnomeList += uri;
      gnomeList += '\n';
      g_free(uri);
    }

    return {{uriListMime, std::move(uriList)}, {gnomeMime, std::move(gnomeList)}, {kdeMime, cut ? "1" : "0"}};
  }

  FileTransferPayload decodeFileTransfer(std::string_view payload, bool gnome, bool kdeCut) {
    FileTransferPayload result{.cut = kdeCut};
    if (payload.find('\0') != std::string_view::npos)
      throw std::invalid_argument("Clipboard payload contains a NUL byte");

    if (gnome) {
      auto contents = payload;
      const auto action = nextLine(contents);
      if (action == "cut")
        result.cut = true;
      else if (action == "copy")
        result.cut = false;
      else
        throw std::invalid_argument("Invalid GNOME file transfer action");
      appendUriList(contents, result);
    } else {
      appendUriList(payload, result);
    }
    return result;
  }
} // namespace filemanager
