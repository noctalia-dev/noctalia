#include "filemanager/admin_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <glib.h>
#include <mutex>
#include <unistd.h>
#include <vector>

namespace filemanager {
  namespace {

    template <typename T> bool parseUnsigned(std::string_view token, T& value) noexcept {
      if (token.empty() || token.front() == '+' || token.front() == '-')
        return false;
      T parsed = 0;
      const auto res = std::from_chars(token.data(), token.data() + token.size(), parsed, 10);
      if (res.ec != std::errc{} || res.ptr != token.data() + token.size())
        return false;
      value = parsed;
      return true;
    }

    bool parseBoolFlag(std::string_view token, bool& value) noexcept {
      if (token == "0") {
        value = false;
        return true;
      }
      if (token == "1") {
        value = true;
        return true;
      }
      return false;
    }

    bool hasDisallowedLineChars(std::string_view text) noexcept {
      for (const unsigned char ch : text) {
        if (ch < 0x20U || ch == 0x7FU)
          return true;
      }
      return false;
    }

    bool splitTokensExact(std::string_view line, std::vector<std::string_view>& tokens) {
      tokens.clear();
      if (line.empty() || line.front() == ' ' || line.back() == ' ')
        return false;
      std::size_t pos = 0;
      while (pos < line.size()) {
        const std::size_t space = line.find(' ', pos);
        if (space == pos)
          return false; // Consecutive spaces are rejected.
        if (space == std::string_view::npos) {
          tokens.push_back(line.substr(pos));
          break;
        }
        tokens.push_back(line.substr(pos, space - pos));
        pos = space + 1;
      }
      return !tokens.empty();
    }

    std::string encodeBase64OrDash(std::string_view raw) {
      if (raw.empty())
        return "-";
      char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(raw.data()), raw.size());
      std::string out(encoded != nullptr ? encoded : "-");
      g_free(encoded);
      return out.empty() ? std::string("-") : out;
    }

    bool decodeBase64OrDash(std::string_view token, std::size_t maxDecodedBytes, std::string& out) {
      out.clear();
      if (token.empty())
        return false;
      if (token == "-")
        return true;
      if (token.size() > maxDecodedBytes * 2U + 16U || (token.size() % 4U) != 0U)
        return false;
      for (const char ch : token) {
        const bool validChar = (ch >= 'A' && ch <= 'Z')
            || (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9')
            || ch == '+'
            || ch == '/'
            || ch == '=';
        if (!validChar)
          return false;
      }
      gsize decodedLen = 0;
      guchar* decoded = g_base64_decode(std::string(token).c_str(), &decodedLen);
      if (decoded == nullptr)
        return false;
      char* canonical = g_base64_encode(decoded, decodedLen);
      const bool matchesCanonical = canonical != nullptr && token == std::string_view(canonical);
      const bool withinBounds = decodedLen <= maxDecodedBytes;
      bool noEmbeddedNull = true;
      for (gsize i = 0; i < decodedLen; ++i) {
        if (decoded[i] == '\0') {
          noEmbeddedNull = false;
          break;
        }
      }
      if (matchesCanonical && withinBounds && noEmbeddedNull)
        out.assign(reinterpret_cast<const char*>(decoded), decodedLen);
      g_free(canonical);
      g_free(decoded);
      return matchesCanonical && withinBounds && noEmbeddedNull;
    }

    bool validAbsoluteRequestPath(std::string_view rawPath) {
      if (rawPath.empty() || rawPath.size() > kAdminMaxPathBytes || rawPath.find('\0') != std::string_view::npos)
        return false;
      if (rawPath.starts_with("trash:"))
        return false;
      const std::filesystem::path p(rawPath);
      if (!p.is_absolute())
        return false;
      return (p == p.root_path() || p.has_filename()) && p.lexically_normal().string() == rawPath;
    }

    bool validSingleEntryName(std::string_view name) {
      return !name.empty()
          && name.size() <= 1024
          && name != "."
          && name != ".."
          && name.find('/') == std::string_view::npos
          && name.find('\0') == std::string_view::npos;
    }

    bool validOperationBegin(const AdminClientRequest& request) {
      using Kind = file_operation_engine::Kind;
      if (request.operationKind == Kind::ExportProtectedFile || request.operationKind == Kind::SaveProtectedFile)
        return !request.properties && !request.keepBoth && !request.expectedDevice && !request.expectedInode
            && request.sourceCount == 1 && request.path != "/" && validAbsoluteRequestPath(request.path)
            && request.contentRevision.size() <= 256
            && (request.operationKind == Kind::ExportProtectedFile
                    ? (request.contentRevision.empty() || request.contentRevision == "thumbnail")
                    : (!request.contentRevision.empty() && request.contentRevision != "thumbnail"));
      if (request.operationKind == Kind::ProtectedChecksum || request.operationKind == Kind::ProtectedFolderSize) {
        if (request.properties || request.keepBoth || request.sourceCount != 0 || !validAbsoluteRequestPath(request.path))
          return false;
        if (request.operationKind == Kind::ProtectedChecksum)
          return !request.expectedDevice && !request.expectedInode
              && (request.contentRevision == "0" || request.contentRevision == "1" || request.contentRevision == "2");
        return request.expectedInode != 0 && request.contentRevision.empty();
      }
      if (!request.contentRevision.empty())
        return false;
      if (request.operationKind == Kind::ModifyProperties)
        return request.properties && request.sourceCount == 0 && !request.keepBoth
            && !request.expectedDevice && !request.expectedInode
            && request.properties->path == request.path && validatePropertyRequest(*request.properties).empty();
      if (request.properties)
        return false;
      if (request.sourceCount > kAdminMaxOperationSources
          || (request.keepBoth && request.operationKind != Kind::CreateSymlink
              && request.operationKind != Kind::CreateFile && request.operationKind != Kind::CreateFolder)
          || ((request.expectedDevice != 0 || request.expectedInode != 0)
              && (request.operationKind != Kind::Rename || request.expectedInode == 0)))
        return false;
      if (request.operationKind == Kind::Delete)
        return request.path.empty() && request.sourceCount > 0;
      if (!validAbsoluteRequestPath(request.path))
        return false;
      switch (request.operationKind) {
      case Kind::Copy:
      case Kind::Duplicate:
      case Kind::Move:
        return request.sourceCount > 0;
      case Kind::Rename:
      case Kind::CreateSymlink:
        return request.path != "/" && request.sourceCount == 1;
      case Kind::CreateFolder:
        return request.path != "/" && request.sourceCount == 0;
      case Kind::CreateFile:
        return request.path != "/" && request.sourceCount <= 1;
      case Kind::Delete:
      case Kind::ModifyProperties:
      case Kind::ExportProtectedFile:
      case Kind::SaveProtectedFile:
      case Kind::ProtectedChecksum:
      case Kind::ProtectedFolderSize:
        break;
      }
      return false;
    }

    bool validOperationDecision(const AdminClientRequest& request) {
      using Action = file_operation_engine::ConflictAction;
      return request.targetRequestId != 0
          && request.targetRequestId != request.requestId
          && request.conflictId != 0
          && request.conflictDecision <= static_cast<unsigned>(Action::Merge)
          && (request.alternativeName.empty()
              || (request.conflictDecision == static_cast<unsigned>(Action::Rename)
                  && validSingleEntryName(request.alternativeName)));
    }

    std::string encodeConflictItem(const file_operation_engine::ConflictItemMetadata& item) {
      return std::to_string(item.type)
          + " "
          + (item.inspected ? "1 " : "0 ")
          + (item.size ? std::to_string(*item.size) : "-")
          + " "
          + (item.modified ? std::to_string(*item.modified) : "-")
          + " "
          + encodeBase64OrDash(item.contentType);
    }

    bool decodeConflictItem(const std::string_view* fields, file_operation_engine::ConflictItemMetadata& item) {
      unsigned type = 0;
      if (!parseUnsigned(fields[0], type)
          || type > G_FILE_TYPE_MOUNTABLE
          || !parseBoolFlag(fields[1], item.inspected)
          || !decodeBase64OrDash(fields[4], kAdminMaxMessageBytes, item.contentType))
        return false;
      item.type = static_cast<GFileType>(type);
      if (fields[2] != "-") {
        const auto token = fields[2];
        std::int64_t size = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), size);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
          return false;
        item.size = size;
      }
      if (fields[3] != "-") {
        std::uint64_t modified = 0;
        if (!parseUnsigned(fields[3], modified))
          return false;
        item.modified = modified;
      }
      return item.inspected || (!item.size && !item.modified && item.contentType.empty());
    }

    void setParseError(
        AdminParseError* errorOut, AdminParseStatus status, AdminRequestId requestId, std::string_view message
    ) {
      if (errorOut != nullptr) {
        errorOut->status = status;
        errorOut->requestId = requestId;
        errorOut->message = std::string(message);
      }
    }

    bool stripTrailingNewline(std::string_view& line, AdminParseError* errorOut) {
      if (!line.empty() && line.back() == '\n')
        line.remove_suffix(1);
      if (line.empty()) {
        setParseError(errorOut, AdminParseStatus::EmptyFrame, kInvalidAdminRequestId, "Empty protocol frame.");
        return false;
      }
      if (line.size() + 1U > kAdminMaxFrameBytes) {
        setParseError(
            errorOut, AdminParseStatus::OversizedFrame, kInvalidAdminRequestId, "Protocol frame exceeds size limit."
        );
        return false;
      }
      if (hasDisallowedLineChars(line)) {
        setParseError(
            errorOut, AdminParseStatus::ControlCharacter, kInvalidAdminRequestId,
            "Protocol frame contains disallowed control characters."
        );
        return false;
      }
      return true;
    }

    constexpr std::size_t kDirBatchTokensPerEntry = 14;

  } // namespace

  std::string_view adminStatusCodeLabel(AdminStatusCode code) noexcept {
    switch (code) {
    case AdminStatusCode::Ok:
      return "Ok";
    case AdminStatusCode::Cancelled:
      return "Cancelled";
    case AdminStatusCode::VersionMismatch:
      return "VersionMismatch";
    case AdminStatusCode::BadRequest:
      return "BadRequest";
    case AdminStatusCode::HandshakeRequired:
      return "HandshakeRequired";
    case AdminStatusCode::Unsupported:
      return "Unsupported";
    case AdminStatusCode::Unauthorized:
      return "Unauthorized";
    case AdminStatusCode::ShuttingDown:
      return "ShuttingDown";
    case AdminStatusCode::InternalError:
      return "InternalError";
    case AdminStatusCode::NotFound:
      return "NotFound";
    case AdminStatusCode::NotDirectory:
      return "NotDirectory";
    case AdminStatusCode::AccessDenied:
      return "AccessDenied";
    case AdminStatusCode::AlreadyExists:
      return "AlreadyExists";
    case AdminStatusCode::ReadOnlyFilesystem:
      return "ReadOnlyFilesystem";
    case AdminStatusCode::NotSupported:
      return "NotSupported";
    }
    return "Unknown";
  }

  bool parseAdminStatusCode(std::uint32_t raw, AdminStatusCode& out) noexcept {
    switch (raw) {
    case static_cast<std::uint32_t>(AdminStatusCode::Ok):
    case static_cast<std::uint32_t>(AdminStatusCode::Cancelled):
    case static_cast<std::uint32_t>(AdminStatusCode::VersionMismatch):
    case static_cast<std::uint32_t>(AdminStatusCode::BadRequest):
    case static_cast<std::uint32_t>(AdminStatusCode::HandshakeRequired):
    case static_cast<std::uint32_t>(AdminStatusCode::Unsupported):
    case static_cast<std::uint32_t>(AdminStatusCode::Unauthorized):
    case static_cast<std::uint32_t>(AdminStatusCode::ShuttingDown):
    case static_cast<std::uint32_t>(AdminStatusCode::InternalError):
    case static_cast<std::uint32_t>(AdminStatusCode::NotFound):
    case static_cast<std::uint32_t>(AdminStatusCode::NotDirectory):
    case static_cast<std::uint32_t>(AdminStatusCode::AccessDenied):
    case static_cast<std::uint32_t>(AdminStatusCode::AlreadyExists):
    case static_cast<std::uint32_t>(AdminStatusCode::ReadOnlyFilesystem):
    case static_cast<std::uint32_t>(AdminStatusCode::NotSupported):
      out = static_cast<AdminStatusCode>(raw);
      return true;
    default:
      return false;
    }
  }

  std::string encodePropertyInfo(GFileInfo* info) {
    GVariantBuilder attributes;
    g_variant_builder_init(&attributes, G_VARIANT_TYPE_VARDICT);
    g_auto(GStrv) names = g_file_info_list_attributes(info, nullptr);
    for (const auto* const* name = names; name && *name; ++name) {
      GVariant* value = nullptr;
      switch (g_file_info_get_attribute_type(info, *name)) {
      case G_FILE_ATTRIBUTE_TYPE_STRING: {
        const auto* str = g_file_info_get_attribute_string(info, *name);
        if (g_utf8_validate(str, -1, nullptr))
          value = g_variant_new_string(str);
        break;
      }
      case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
        value = g_variant_new_bytestring(g_file_info_get_attribute_byte_string(info, *name));
        break;
      case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
        value = g_variant_new_boolean(g_file_info_get_attribute_boolean(info, *name));
        break;
      case G_FILE_ATTRIBUTE_TYPE_UINT32:
        value = g_variant_new_uint32(g_file_info_get_attribute_uint32(info, *name));
        break;
      case G_FILE_ATTRIBUTE_TYPE_UINT64:
        value = g_variant_new_uint64(g_file_info_get_attribute_uint64(info, *name));
        break;
      case G_FILE_ATTRIBUTE_TYPE_INT64:
        value = g_variant_new_int64(g_file_info_get_attribute_int64(info, *name));
        break;
      case G_FILE_ATTRIBUTE_TYPE_OBJECT:
        if (std::string_view(*name) == G_FILE_ATTRIBUTE_STANDARD_ICON)
          value = g_icon_serialize(g_file_info_get_icon(info));
        break;
      default:
        break;
      }
      if (value) {
        g_autoptr(GVariant) owned = g_variant_ref_sink(value);
        g_variant_builder_add(&attributes, "{sv}", *name, owned);
      }
    }
    g_autoptr(GVariant) dictionary = g_variant_ref_sink(g_variant_builder_end(&attributes));
    g_autofree char* encoded = g_variant_print(dictionary, TRUE);
    return encoded;
  }

  GFileInfo* decodePropertyInfo(const std::string& encoded) {
    g_autoptr(GVariant) attributes =
        g_variant_parse(G_VARIANT_TYPE_VARDICT, encoded.c_str(), nullptr, nullptr, nullptr);
    if (!attributes)
      return nullptr;
    GFileInfo* info = g_file_info_new();
    auto* matcher = g_file_attribute_matcher_new(kPropertyInfoAttributes);
    GVariantIter iter;
    g_variant_iter_init(&iter, attributes);
    const char* name = nullptr;
    GVariant* raw = nullptr;
    while (g_variant_iter_next(&iter, "{&sv}", &name, &raw)) {
      g_autoptr(GVariant) value = raw;
      if (!g_file_attribute_matcher_matches(matcher, name))
        continue;
      if (std::string_view(name) == G_FILE_ATTRIBUTE_STANDARD_ICON) {
        g_autoptr(GIcon) icon = g_icon_deserialize(value);
        if (icon)
          g_file_info_set_icon(info, icon);
      } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        g_file_info_set_attribute_string(info, name, g_variant_get_string(value, nullptr));
      else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING))
        g_file_info_set_attribute_byte_string(info, name, g_variant_get_bytestring(value));
      else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
        g_file_info_set_attribute_boolean(info, name, g_variant_get_boolean(value));
      else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
        g_file_info_set_attribute_uint32(info, name, g_variant_get_uint32(value));
      else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64))
        g_file_info_set_attribute_uint64(info, name, g_variant_get_uint64(value));
      else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
        g_file_info_set_attribute_int64(info, name, g_variant_get_int64(value));
    }
    g_file_attribute_matcher_unref(matcher);
    return info;
  }

  std::optional<std::string> encodeAdminClientRequest(const AdminClientRequest& request) {
    if (request.requestId == kInvalidAdminRequestId)
      return std::nullopt;
    std::string line;
    switch (request.command) {
    case AdminClientCommand::Hello:
      if (request.nonce.size() > kAdminMaxNonceBytes || request.nonce.find('\0') != std::string::npos)
        return std::nullopt;
      if (request.inactivityTimeoutMs > 0) {
        line = "HELLO "
            + std::to_string(request.requestId)
            + " "
            + std::to_string(request.protocolVersion)
            + " "
            + std::to_string(request.inactivityTimeoutMs)
            + " "
            + encodeBase64OrDash(request.nonce);
      } else {
        line = "HELLO "
            + std::to_string(request.requestId)
            + " "
            + std::to_string(request.protocolVersion)
            + " "
            + encodeBase64OrDash(request.nonce);
      }
      break;
    case AdminClientCommand::Ping:
      line = "PING " + std::to_string(request.requestId);
      break;
    case AdminClientCommand::RenewActivity:
      line = "RENEW_ACTIVITY " + std::to_string(request.requestId);
      break;
    case AdminClientCommand::Cancel:
      if (request.targetRequestId == kInvalidAdminRequestId)
        return std::nullopt;
      line = "CANCEL " + std::to_string(request.requestId) + " " + std::to_string(request.targetRequestId);
      break;
    case AdminClientCommand::Shutdown:
      line = "SHUTDOWN " + std::to_string(request.requestId);
      break;
    case AdminClientCommand::ListDir:
      if (!validAbsoluteRequestPath(request.path)
          || request.maxBatchEntries == 0
          || request.maxBatchEntries > kAdminMaxDirBatchEntries) {
        return std::nullopt;
      }
      line = "LIST_DIR "
          + std::to_string(request.requestId)
          + " "
          + std::to_string(request.maxBatchEntries)
          + " "
          + (request.userInitiated ? "1 " : "0 ")
          + encodeBase64OrDash(request.path);
      break;
    case AdminClientCommand::StatInfo:
      if (!validAbsoluteRequestPath(request.path))
        return std::nullopt;
      line = "STAT_INFO "
          + std::to_string(request.requestId)
          + " "
          + (request.userInitiated ? "1 " : "0 ")
          + encodeBase64OrDash(request.path)
          + (request.statDetail != AdminStatDetail::Basic
                 ? " " + std::to_string(static_cast<unsigned>(request.statDetail)) : "");
      break;
    case AdminClientCommand::StatVfs:
      if (!validAbsoluteRequestPath(request.path))
        return std::nullopt;
      line = "STATVFS_INFO " + std::to_string(request.requestId) + " " + encodeBase64OrDash(request.path);
      break;
    case AdminClientCommand::CountDir:
      if (!validAbsoluteRequestPath(request.path))
        return std::nullopt;
      line = "COUNT_DIR " + std::to_string(request.requestId) + " " + encodeBase64OrDash(request.path);
      break;
    case AdminClientCommand::OpBegin:
      if (!validOperationBegin(request))
        return std::nullopt;
      line = "OP_BEGIN "
          + std::to_string(request.requestId)
          + " "
          + std::to_string(static_cast<unsigned>(request.operationKind))
          + " "
          + std::to_string(request.sourceCount)
          + " "
          + (request.keepBoth ? "1 " : "0 ")
          + std::to_string(request.expectedDevice)
          + " "
          + std::to_string(request.expectedInode)
          + " "
          + encodeBase64OrDash(request.path);
      if (const auto& properties = request.properties) {
        const auto args = propertyArguments(*properties);
        line += " " + args[1] + " " + args[2] + " " + args[3] + " " + args[4]
            + " " + encodeBase64OrDash(args[5]) + " " + encodeBase64OrDash(args[6]) + " " + args[7];
      }
      if (!request.contentRevision.empty())
        line += " " + encodeBase64OrDash(request.contentRevision);
      break;
    case AdminClientCommand::OpSource:
      if (request.path.empty()
          || request.path.size() > kAdminMaxPathBytes
          || request.path.find('\0') != std::string::npos)
        return std::nullopt;
      line = "OP_SOURCE " + std::to_string(request.requestId) + " " + encodeBase64OrDash(request.path);
      break;
    case AdminClientCommand::OpExec:
      line = "OP_EXEC " + std::to_string(request.requestId);
      break;
    case AdminClientCommand::OpPause:
    case AdminClientCommand::OpResume:
      if (request.targetRequestId == 0 || request.targetRequestId == request.requestId)
        return std::nullopt;
      line = std::string(request.command == AdminClientCommand::OpPause ? "OP_PAUSE " : "OP_RESUME ")
          + std::to_string(request.requestId)
          + " "
          + std::to_string(request.targetRequestId);
      break;
    case AdminClientCommand::OpConflictReply:
      if (!validOperationDecision(request))
        return std::nullopt;
      line = "OP_CONFLICT_REPLY "
          + std::to_string(request.requestId)
          + " "
          + std::to_string(request.targetRequestId)
          + " "
          + std::to_string(request.conflictId)
          + " "
          + std::to_string(request.conflictDecision)
          + " "
          + (request.applyToAll ? "1 " : "0 ")
          + encodeBase64OrDash(request.alternativeName);
      break;
    }
    if (hasDisallowedLineChars(line))
      return std::nullopt;
    line.push_back('\n');
    if (line.size() > kAdminMaxFrameBytes)
      return std::nullopt;
    return line;
  }

  bool decodeAdminClientRequest(std::string_view line, AdminClientRequest& out, AdminParseError* errorOut) {
    out = {};
    if (!stripTrailingNewline(line, errorOut))
      return false;

    std::vector<std::string_view> tokens;
    if (!splitTokensExact(line, tokens) || tokens.size() < 2) {
      setParseError(errorOut, AdminParseStatus::MalformedFields, kInvalidAdminRequestId, "Malformed client request.");
      return false;
    }

    AdminRequestId reqId = kInvalidAdminRequestId;
    if (!parseUnsigned(tokens[1], reqId) || reqId == kInvalidAdminRequestId) {
      setParseError(errorOut, AdminParseStatus::MalformedFields, kInvalidAdminRequestId, "Invalid request identifier.");
      return false;
    }

    const std::string_view cmd = tokens[0];
    if (cmd == "HELLO") {
      if (tokens.size() == 4) {
        if (!parseUnsigned(tokens[2], out.protocolVersion)
            || !decodeBase64OrDash(tokens[3], kAdminMaxNonceBytes, out.nonce)) {
          setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed HELLO request.");
          return false;
        }
      } else if (tokens.size() == 5) {
        if (!parseUnsigned(tokens[2], out.protocolVersion)
            || !parseUnsigned(tokens[3], out.inactivityTimeoutMs)
            || out.inactivityTimeoutMs == 0
            || !decodeBase64OrDash(tokens[4], kAdminMaxNonceBytes, out.nonce)) {
          setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed HELLO request.");
          return false;
        }
      } else {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed HELLO request.");
        return false;
      }
      out.command = AdminClientCommand::Hello;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "PING") {
      if (tokens.size() != 2) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed PING request.");
        return false;
      }
      out.command = AdminClientCommand::Ping;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "RENEW_ACTIVITY") {
      if (tokens.size() != 2) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed RENEW_ACTIVITY request.");
        return false;
      }
      out.command = AdminClientCommand::RenewActivity;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "CANCEL") {
      if (tokens.size() != 3
          || !parseUnsigned(tokens[2], out.targetRequestId)
          || out.targetRequestId == kInvalidAdminRequestId) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed CANCEL request.");
        return false;
      }
      out.command = AdminClientCommand::Cancel;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "SHUTDOWN") {
      if (tokens.size() != 2) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed SHUTDOWN request.");
        return false;
      }
      out.command = AdminClientCommand::Shutdown;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "LIST_DIR") {
      if (tokens.size() != 5
          || !parseUnsigned(tokens[2], out.maxBatchEntries)
          || out.maxBatchEntries == 0
          || out.maxBatchEntries > kAdminMaxDirBatchEntries
          || !parseBoolFlag(tokens[3], out.userInitiated)
          || !decodeBase64OrDash(tokens[4], kAdminMaxPathBytes, out.path)
          || !validAbsoluteRequestPath(out.path)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed LIST_DIR request.");
        return false;
      }
      out.command = AdminClientCommand::ListDir;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "STAT_INFO") {
      unsigned detail = 0;
      if ((tokens.size() != 4 && tokens.size() != 5)
          || (tokens.size() == 5 && !parseUnsigned(tokens[4], detail))
          || detail > static_cast<unsigned>(AdminStatDetail::DropTarget)
          || !parseBoolFlag(tokens[2], out.userInitiated)
          || !decodeBase64OrDash(tokens[3], kAdminMaxPathBytes, out.path)
          || !validAbsoluteRequestPath(out.path)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed STAT_INFO request.");
        return false;
      }
      out.command = AdminClientCommand::StatInfo;
      out.statDetail = static_cast<AdminStatDetail>(detail);
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "STATVFS_INFO") {
      if (tokens.size() != 3
          || !decodeBase64OrDash(tokens[2], kAdminMaxPathBytes, out.path)
          || !validAbsoluteRequestPath(out.path)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed STATVFS_INFO request.");
        return false;
      }
      out.command = AdminClientCommand::StatVfs;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd == "COUNT_DIR") {
      if (tokens.size() != 3
          || !decodeBase64OrDash(tokens[2], kAdminMaxPathBytes, out.path)
          || !validAbsoluteRequestPath(out.path)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed COUNT_DIR request.");
        return false;
      }
      out.command = AdminClientCommand::CountDir;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (cmd.starts_with("OP_")) {
      out.requestId = reqId;
      using Command = AdminClientCommand;
      const auto operation = [&]() -> std::optional<Command> {
        if (cmd == "OP_BEGIN") {
          unsigned kind = 0;
          if ((tokens.size() != 8 && tokens.size() != 9 && tokens.size() != 15)
              || !parseUnsigned(tokens[2], kind)
              || kind > static_cast<unsigned>(file_operation_engine::Kind::ProtectedFolderSize)
              || !parseUnsigned(tokens[3], out.sourceCount)
              || !parseBoolFlag(tokens[4], out.keepBoth)
              || !parseUnsigned(tokens[5], out.expectedDevice)
              || !parseUnsigned(tokens[6], out.expectedInode)
              || !decodeBase64OrDash(tokens[7], kAdminMaxPathBytes, out.path))
            return std::nullopt;
          out.operationKind = static_cast<file_operation_engine::Kind>(kind);
          if (tokens.size() == 9 && !decodeBase64OrDash(tokens[8], 256, out.contentRevision))
            return std::nullopt;
          if (tokens.size() == 15) {
            std::vector<std::string> args{
                std::string(tokens[7]), std::string(tokens[8]), std::string(tokens[9]), std::string(tokens[10]),
                std::string(tokens[11]), {}, {}, std::string(tokens[14])
            };
            PropertyRequest properties;
            if (!decodeBase64OrDash(tokens[12], 1024, args[5])
                || !decodeBase64OrDash(tokens[13], 1024, args[6])
                || !parsePropertyArguments(args, properties))
              return std::nullopt;
            out.properties = std::move(properties);
          }
          if (validOperationBegin(out))
            return Command::OpBegin;
        } else if (
            cmd == "OP_SOURCE"
            && tokens.size() == 3
            && decodeBase64OrDash(tokens[2], kAdminMaxPathBytes, out.path)
            && !out.path.empty()
        ) {
          return Command::OpSource;
        } else if (cmd == "OP_EXEC" && tokens.size() == 2) {
          return Command::OpExec;
        } else if (
            (cmd == "OP_PAUSE" || cmd == "OP_RESUME")
            && tokens.size() == 3
            && parseUnsigned(tokens[2], out.targetRequestId)
            && out.targetRequestId != 0
            && out.targetRequestId != reqId
        ) {
          return cmd == "OP_PAUSE" ? Command::OpPause : Command::OpResume;
        } else if (
            cmd == "OP_CONFLICT_REPLY"
            && tokens.size() == 7
            && parseUnsigned(tokens[2], out.targetRequestId)
            && parseUnsigned(tokens[3], out.conflictId)
            && parseUnsigned(tokens[4], out.conflictDecision)
            && parseBoolFlag(tokens[5], out.applyToAll)
            && decodeBase64OrDash(tokens[6], 1024, out.alternativeName)
            && validOperationDecision(out)
        ) {
          return Command::OpConflictReply;
        }
        return std::nullopt;
      }();
      if (!operation) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed shared operation request.");
        return false;
      }
      out.command = *operation;
      out.requestId = reqId;
      return true;
    }
    setParseError(errorOut, AdminParseStatus::UnsupportedCommand, reqId, "Unknown or unsupported client command.");
    return false;
  }

  std::optional<std::string> encodeAdminServerFrame(const AdminServerFrame& frame) {
    if (frame.requestId == kInvalidAdminRequestId)
      return std::nullopt;
    std::string line;
    switch (frame.kind) {
    case AdminServerFrameKind::Ready:
      if (frame.nonce.size() > kAdminMaxNonceBytes || frame.nonce.find('\0') != std::string::npos)
        return std::nullopt;
      line = "READY "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(frame.protocolVersion)
          + " "
          + std::to_string(frame.helperPid)
          + " "
          + std::to_string(frame.invokingUid)
          + " "
          + encodeBase64OrDash(frame.nonce);
      break;
    case AdminServerFrameKind::Status:
      line = "STATUS "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(frame.protocolVersion)
          + " "
          + std::to_string(frame.helperPid)
          + " "
          + std::to_string(frame.invokingUid)
          + " "
          + std::to_string(frame.uptimeMs)
          + " "
          + std::to_string(frame.activeRequestId);
      break;
    case AdminServerFrameKind::Response: {
      const std::string_view truncatedDetail =
          std::string_view(frame.detail).substr(0, std::min(frame.detail.size(), kAdminMaxMessageBytes));
      if (truncatedDetail.find('\0') != std::string_view::npos)
        return std::nullopt;
      line = "RESP "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(static_cast<std::uint32_t>(frame.status))
          + " "
          + encodeBase64OrDash(truncatedDetail);
      break;
    }
    case AdminServerFrameKind::DirBatch:
      if (frame.entries.empty() || frame.entries.size() > kAdminMaxDirBatchEntries)
        return std::nullopt;
      line = "DIR_BATCH " + std::to_string(frame.requestId) + " " + std::to_string(frame.entries.size());
      for (const auto& entry : frame.entries) {
        if (!validSingleEntryName(entry.name)
            || entry.contentType.size() > 256
            || entry.contentType.find('\0') != std::string::npos) {
          return std::nullopt;
        }
        line += " "
            + encodeBase64OrDash(entry.name)
            + " "
            + (entry.isDir ? "1" : "0")
            + " "
            + (entry.isSymlink ? "1" : "0")
            + " "
            + (entry.brokenSymlink ? "1" : "0")
            + " "
            + (entry.hasIdentity ? "1" : "0")
            + " "
            + std::to_string(entry.size)
            + " "
            + std::to_string(entry.device)
            + " "
            + std::to_string(entry.inode)
            + " "
            + std::to_string(entry.mtimeSec)
            + " "
            + (entry.hasCreated ? "1" : "0")
            + " "
            + std::to_string(entry.createdSec)
            + " "
            + (entry.hasAccessed ? "1" : "0")
            + " "
            + std::to_string(entry.accessedSec)
            + " "
            + encodeBase64OrDash(entry.contentType);
      }
      break;
    case AdminServerFrameKind::DirEnd:
      line = "DIR_END "
          + std::to_string(frame.requestId)
          + " "
          + (frame.directoryWritable ? "1" : "0")
          + " "
          + std::to_string(frame.totalEntries);
      break;
    case AdminServerFrameKind::StatResult:
      if (frame.statInfo.contentType.size() > 256
          || frame.statInfo.contentType.find('\0') != std::string::npos
          || frame.statInfo.symlinkTarget.size() > kAdminMaxPathBytes
          || frame.statInfo.symlinkTarget.find('\0') != std::string::npos) {
        return std::nullopt;
      }
      line = "STAT_RESULT "
          + std::to_string(frame.requestId)
          + " "
          + (frame.statInfo.exists ? "1" : "0")
          + " "
          + (frame.statInfo.isDir ? "1" : "0")
          + " "
          + (frame.statInfo.isSymlink ? "1" : "0")
          + " "
          + (frame.statInfo.brokenSymlink ? "1" : "0")
          + " "
          + (frame.statInfo.writable ? "1" : "0")
          + " "
          + (frame.statInfo.readable ? "1" : "0")
          + " "
          + (frame.statInfo.hasSize ? "1" : "0")
          + " "
          + std::to_string(frame.statInfo.size)
          + " "
          + std::to_string(frame.statInfo.device)
          + " "
          + std::to_string(frame.statInfo.inode)
          + " "
          + std::to_string(frame.statInfo.mtimeSec)
          + " "
          + (frame.statInfo.hasCreated ? "1" : "0")
          + " "
          + std::to_string(frame.statInfo.createdSec)
          + " "
          + (frame.statInfo.hasAccessed ? "1" : "0")
          + " "
          + std::to_string(frame.statInfo.accessedSec)
          + " "
          + encodeBase64OrDash(frame.statInfo.contentType)
          + " "
          + encodeBase64OrDash(frame.statInfo.symlinkTarget);
      if (!frame.statInfo.properties.empty() || !frame.statInfo.resolvedPath.empty())
        line += " " + encodeBase64OrDash(frame.statInfo.properties);
      if (!frame.statInfo.resolvedPath.empty())
        line += " " + encodeBase64OrDash(frame.statInfo.resolvedPath);
      break;
    case AdminServerFrameKind::StatVfsResult:
      line = "STATVFS_RESULT "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(frame.statVfsInfo.totalBytes)
          + " "
          + std::to_string(frame.statVfsInfo.freeBytes)
          + " "
          + std::to_string(frame.statVfsInfo.availableBytes)
          + " "
          + (frame.statVfsInfo.readOnly ? "1" : "0");
      break;
    case AdminServerFrameKind::CountResult:
      line = "COUNT_RESULT " + std::to_string(frame.requestId) + " " + std::to_string(frame.itemCount);
      break;
    case AdminServerFrameKind::OpConflict:
      if (!frame.conflictId
          || !validAbsoluteRequestPath(frame.sourcePath)
          || !validAbsoluteRequestPath(frame.destinationPath))
        return std::nullopt;
      line = "OP_CONFLICT "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(frame.conflictId)
          + " "
          + (frame.hasRemaining ? "1 " : "0 ")
          + (frame.conflictMetadata.sameItem ? "1 " : "0 ")
          + encodeBase64OrDash(frame.sourcePath)
          + " "
          + encodeBase64OrDash(frame.destinationPath)
          + " "
          + encodeConflictItem(frame.conflictMetadata.incoming)
          + " "
          + encodeConflictItem(frame.conflictMetadata.existing);
      break;
    case AdminServerFrameKind::OpProgress:
      line = "OP_PROGRESS "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(frame.completedItems)
          + " "
          + std::to_string(frame.totalItems)
          + " "
          + std::to_string(frame.completedBytes)
          + " "
          + std::to_string(frame.totalBytes)
          + " "
          + std::to_string(frame.speed);
      break;
    case AdminServerFrameKind::OpPaused:
      line = "OP_PAUSED " + std::to_string(frame.requestId) + (frame.paused ? " 1" : " 0");
      break;
    case AdminServerFrameKind::OpCompletedEntry:
      if (!validAbsoluteRequestPath(frame.completedEntry.path) || !frame.completedEntry.inode)
        return std::nullopt;
      line = "OP_COMPLETED_ENTRY "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(frame.completedEntry.device)
          + " "
          + std::to_string(frame.completedEntry.inode)
          + " "
          + encodeBase64OrDash(frame.completedEntry.path);
      break;
    case AdminServerFrameKind::OpMovedSource:
      if (!validAbsoluteRequestPath(frame.sourcePath))
        return std::nullopt;
      line = "OP_MOVED_SOURCE " + std::to_string(frame.requestId) + " " + encodeBase64OrDash(frame.sourcePath);
      break;
    case AdminServerFrameKind::OpResult:
      if (!frame.destinationPath.empty() && !validAbsoluteRequestPath(frame.destinationPath))
        return std::nullopt;
      line = "OP_RESULT "
          + std::to_string(frame.requestId)
          + " "
          + std::to_string(static_cast<unsigned>(frame.status))
          + " "
          + (frame.mutated ? "1 " : "0 ")
          + std::to_string(frame.completedItems)
          + " "
          + std::to_string(frame.totalItems)
          + " "
          + std::to_string(frame.completedBytes)
          + " "
          + std::to_string(frame.totalBytes)
          + " "
          + encodeBase64OrDash(frame.destinationPath)
          + " "
          + encodeBase64OrDash(frame.detail.substr(0, kAdminMaxMessageBytes))
          + " "
          + encodeBase64OrDash(frame.customDescription.substr(0, kAdminMaxMessageBytes));
      if (const auto& properties = frame.properties) {
        line += " " + std::to_string(properties->succeeded)
            + " " + std::to_string(properties->failed)
            + " " + std::to_string(properties->skipped)
            + (properties->ownershipEffects ? " 1 " : " 0 ")
            + encodeBase64OrDash(properties->error.substr(0, kAdminMaxMessageBytes));
      }
      break;
    }
    if (hasDisallowedLineChars(line))
      return std::nullopt;
    line.push_back('\n');
    if (line.size() > kAdminMaxFrameBytes)
      return std::nullopt;
    return line;
  }

  bool decodeAdminServerFrame(std::string_view line, AdminServerFrame& out, AdminParseError* errorOut) {
    out = {};
    if (!stripTrailingNewline(line, errorOut))
      return false;

    std::vector<std::string_view> tokens;
    if (!splitTokensExact(line, tokens) || tokens.size() < 2) {
      setParseError(errorOut, AdminParseStatus::MalformedFields, kInvalidAdminRequestId, "Malformed server frame.");
      return false;
    }

    AdminRequestId reqId = kInvalidAdminRequestId;
    if (!parseUnsigned(tokens[1], reqId) || reqId == kInvalidAdminRequestId) {
      setParseError(errorOut, AdminParseStatus::MalformedFields, kInvalidAdminRequestId, "Invalid frame request ID.");
      return false;
    }

    const std::string_view kind = tokens[0];
    if (kind == "READY") {
      if (tokens.size() != 6
          || !parseUnsigned(tokens[2], out.protocolVersion)
          || !parseUnsigned(tokens[3], out.helperPid)
          || !parseUnsigned(tokens[4], out.invokingUid)
          || !decodeBase64OrDash(tokens[5], kAdminMaxNonceBytes, out.nonce)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed READY frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::Ready;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "STATUS") {
      if (tokens.size() != 7
          || !parseUnsigned(tokens[2], out.protocolVersion)
          || !parseUnsigned(tokens[3], out.helperPid)
          || !parseUnsigned(tokens[4], out.invokingUid)
          || !parseUnsigned(tokens[5], out.uptimeMs)
          || !parseUnsigned(tokens[6], out.activeRequestId)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed STATUS frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::Status;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "RESP") {
      std::uint32_t rawStatus = 0;
      if (tokens.size() != 4
          || !parseUnsigned(tokens[2], rawStatus)
          || !parseAdminStatusCode(rawStatus, out.status)
          || !decodeBase64OrDash(tokens[3], kAdminMaxMessageBytes, out.detail)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed RESP frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::Response;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "DIR_BATCH") {
      std::uint32_t count = 0;
      if (tokens.size() < 3
          || !parseUnsigned(tokens[2], count)
          || count == 0
          || count > kAdminMaxDirBatchEntries
          || tokens.size() != 3U + static_cast<std::size_t>(count) * kDirBatchTokensPerEntry) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed DIR_BATCH frame.");
        return false;
      }
      out.entries.reserve(count);
      std::size_t idx = 3;
      for (std::uint32_t i = 0; i < count; ++i) {
        AdminDirEntryRecord rec;
        if (!decodeBase64OrDash(tokens[idx], 1024, rec.name)
            || !validSingleEntryName(rec.name)
            || !parseBoolFlag(tokens[idx + 1], rec.isDir)
            || !parseBoolFlag(tokens[idx + 2], rec.isSymlink)
            || !parseBoolFlag(tokens[idx + 3], rec.brokenSymlink)
            || !parseBoolFlag(tokens[idx + 4], rec.hasIdentity)
            || !parseUnsigned(tokens[idx + 5], rec.size)
            || !parseUnsigned(tokens[idx + 6], rec.device)
            || !parseUnsigned(tokens[idx + 7], rec.inode)
            || !parseUnsigned(tokens[idx + 8], rec.mtimeSec)
            || !parseBoolFlag(tokens[idx + 9], rec.hasCreated)
            || !parseUnsigned(tokens[idx + 10], rec.createdSec)
            || !parseBoolFlag(tokens[idx + 11], rec.hasAccessed)
            || !parseUnsigned(tokens[idx + 12], rec.accessedSec)
            || !decodeBase64OrDash(tokens[idx + 13], 256, rec.contentType)) {
          setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed entry in DIR_BATCH frame.");
          return false;
        }
        out.entries.push_back(std::move(rec));
        idx += kDirBatchTokensPerEntry;
      }
      out.kind = AdminServerFrameKind::DirBatch;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "DIR_END") {
      if (tokens.size() != 4
          || !parseBoolFlag(tokens[2], out.directoryWritable)
          || !parseUnsigned(tokens[3], out.totalEntries)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed DIR_END frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::DirEnd;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "STAT_RESULT") {
      if ((tokens.size() < 19 || tokens.size() > 21)
          || (tokens.size() >= 20 && !decodeBase64OrDash(tokens[19], kAdminMaxFrameBytes / 2, out.statInfo.properties))
          || (tokens.size() == 21 && (!decodeBase64OrDash(tokens[20], kAdminMaxPathBytes, out.statInfo.resolvedPath)
                                     || !validAbsoluteRequestPath(out.statInfo.resolvedPath)))
          || !parseBoolFlag(tokens[2], out.statInfo.exists)
          || !parseBoolFlag(tokens[3], out.statInfo.isDir)
          || !parseBoolFlag(tokens[4], out.statInfo.isSymlink)
          || !parseBoolFlag(tokens[5], out.statInfo.brokenSymlink)
          || !parseBoolFlag(tokens[6], out.statInfo.writable)
          || !parseBoolFlag(tokens[7], out.statInfo.readable)
          || !parseBoolFlag(tokens[8], out.statInfo.hasSize)
          || !parseUnsigned(tokens[9], out.statInfo.size)
          || !parseUnsigned(tokens[10], out.statInfo.device)
          || !parseUnsigned(tokens[11], out.statInfo.inode)
          || !parseUnsigned(tokens[12], out.statInfo.mtimeSec)
          || !parseBoolFlag(tokens[13], out.statInfo.hasCreated)
          || !parseUnsigned(tokens[14], out.statInfo.createdSec)
          || !parseBoolFlag(tokens[15], out.statInfo.hasAccessed)
          || !parseUnsigned(tokens[16], out.statInfo.accessedSec)
          || !decodeBase64OrDash(tokens[17], 256, out.statInfo.contentType)
          || !decodeBase64OrDash(tokens[18], kAdminMaxPathBytes, out.statInfo.symlinkTarget)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed STAT_RESULT frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::StatResult;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "STATVFS_RESULT") {
      if (tokens.size() != 6
          || !parseUnsigned(tokens[2], out.statVfsInfo.totalBytes)
          || !parseUnsigned(tokens[3], out.statVfsInfo.freeBytes)
          || !parseUnsigned(tokens[4], out.statVfsInfo.availableBytes)
          || !parseBoolFlag(tokens[5], out.statVfsInfo.readOnly)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed STATVFS_RESULT frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::StatVfsResult;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind == "COUNT_RESULT") {
      if (tokens.size() != 3 || !parseUnsigned(tokens[2], out.itemCount)) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed COUNT_RESULT frame.");
        return false;
      }
      out.kind = AdminServerFrameKind::CountResult;
      out.requestId = reqId;
      setParseError(errorOut, AdminParseStatus::Ok, reqId, {});
      return true;
    }
    if (kind.starts_with("OP_")) {
      using Kind = AdminServerFrameKind;
      const auto operation = [&]() -> std::optional<Kind> {
        if (kind == "OP_CONFLICT"
            && tokens.size() == 17
            && parseUnsigned(tokens[2], out.conflictId)
            && out.conflictId != 0
            && parseBoolFlag(tokens[3], out.hasRemaining)
            && parseBoolFlag(tokens[4], out.conflictMetadata.sameItem)
            && decodeBase64OrDash(tokens[5], kAdminMaxPathBytes, out.sourcePath)
            && validAbsoluteRequestPath(out.sourcePath)
            && decodeBase64OrDash(tokens[6], kAdminMaxPathBytes, out.destinationPath)
            && validAbsoluteRequestPath(out.destinationPath)
            && decodeConflictItem(tokens.data() + 7, out.conflictMetadata.incoming)
            && decodeConflictItem(tokens.data() + 12, out.conflictMetadata.existing))
          return Kind::OpConflict;
        if (kind == "OP_PROGRESS"
            && tokens.size() == 7
            && parseUnsigned(tokens[2], out.completedItems)
            && parseUnsigned(tokens[3], out.totalItems)
            && parseUnsigned(tokens[4], out.completedBytes)
            && parseUnsigned(tokens[5], out.totalBytes)
            && parseUnsigned(tokens[6], out.speed))
          return Kind::OpProgress;
        if (kind == "OP_PAUSED" && tokens.size() == 3 && parseBoolFlag(tokens[2], out.paused))
          return Kind::OpPaused;
        if (kind == "OP_COMPLETED_ENTRY"
            && tokens.size() == 5
            && parseUnsigned(tokens[2], out.completedEntry.device)
            && parseUnsigned(tokens[3], out.completedEntry.inode)
            && out.completedEntry.inode != 0
            && decodeBase64OrDash(tokens[4], kAdminMaxPathBytes, out.completedEntry.path)
            && validAbsoluteRequestPath(out.completedEntry.path))
          return Kind::OpCompletedEntry;
        if (kind == "OP_MOVED_SOURCE"
            && tokens.size() == 3
            && decodeBase64OrDash(tokens[2], kAdminMaxPathBytes, out.sourcePath)
            && validAbsoluteRequestPath(out.sourcePath))
          return Kind::OpMovedSource;
        unsigned status = 0;
        if (kind == "OP_RESULT"
            && (tokens.size() == 11 || tokens.size() == 16)
            && parseUnsigned(tokens[2], status)
            && parseAdminStatusCode(status, out.status)
            && parseBoolFlag(tokens[3], out.mutated)
            && parseUnsigned(tokens[4], out.completedItems)
            && parseUnsigned(tokens[5], out.totalItems)
            && parseUnsigned(tokens[6], out.completedBytes)
            && parseUnsigned(tokens[7], out.totalBytes)
            && decodeBase64OrDash(tokens[8], kAdminMaxPathBytes, out.destinationPath)
            && (out.destinationPath.empty() || validAbsoluteRequestPath(out.destinationPath))
            && decodeBase64OrDash(tokens[9], kAdminMaxMessageBytes, out.detail)
            && decodeBase64OrDash(tokens[10], kAdminMaxMessageBytes, out.customDescription)) {
          if (tokens.size() == 16) {
            PropertyResult properties{.cancelled = out.status == AdminStatusCode::Cancelled};
            if (!parseUnsigned(tokens[11], properties.succeeded)
                || !parseUnsigned(tokens[12], properties.failed)
                || !parseUnsigned(tokens[13], properties.skipped)
                || !parseBoolFlag(tokens[14], properties.ownershipEffects)
                || !decodeBase64OrDash(tokens[15], kAdminMaxMessageBytes, properties.error))
              return std::nullopt;
            out.properties = std::move(properties);
          }
          return Kind::OpResult;
        }
        return std::nullopt;
      }();
      if (!operation) {
        setParseError(errorOut, AdminParseStatus::MalformedFields, reqId, "Malformed shared operation frame.");
        return false;
      }
      out.kind = *operation;
      out.requestId = reqId;
      return true;
    }
    setParseError(errorOut, AdminParseStatus::UnknownFrameKind, reqId, "Unknown server frame kind.");
    return false;
  }

  AdminLineBuffer::AdminLineBuffer(std::size_t maxFrameBytes) noexcept
      : m_maxFrameBytes(maxFrameBytes > 1 ? maxFrameBytes : kAdminMaxFrameBytes) {}

  void AdminLineBuffer::reset() noexcept {
    m_buffer.clear();
    m_oversizedLatched = false;
  }

  void AdminLineBuffer::append(std::string_view chunk) {
    if (chunk.empty() || m_oversizedLatched)
      return;
    m_buffer.append(chunk.data(), chunk.size());
    const std::size_t newline = m_buffer.find('\n');
    if (newline == std::string::npos) {
      if (m_buffer.size() >= m_maxFrameBytes) {
        m_oversizedLatched = true;
        m_buffer.clear();
      }
    } else if (newline + 1U > m_maxFrameBytes) {
      m_oversizedLatched = true;
      m_buffer.clear();
    }
  }

  AdminFrameReadStatus AdminLineBuffer::popFrame(std::string& lineOut) {
    lineOut.clear();
    if (m_oversizedLatched) {
      m_oversizedLatched = false;
      m_buffer.clear();
      return AdminFrameReadStatus::OversizedFrame;
    }
    const std::size_t newline = m_buffer.find('\n');
    if (newline == std::string::npos) {
      if (m_buffer.size() >= m_maxFrameBytes) {
        m_buffer.clear();
        return AdminFrameReadStatus::OversizedFrame;
      }
      return AdminFrameReadStatus::NeedMoreData;
    }
    if (newline + 1U > m_maxFrameBytes) {
      m_buffer.erase(0, newline + 1U);
      return AdminFrameReadStatus::OversizedFrame;
    }
    lineOut.assign(m_buffer.data(), newline);
    m_buffer.erase(0, newline + 1U);
    if (lineOut.empty() || hasDisallowedLineChars(lineOut))
      return AdminFrameReadStatus::MalformedFrame;
    return AdminFrameReadStatus::FrameReady;
  }

  bool writeAdminFrameFd(int fd, std::string_view encodedFrame, int& savedErrno) noexcept {
    static std::once_flag ignoreSigpipeOnce;
    std::call_once(ignoreSigpipeOnce, [] {
      struct sigaction currentAction{};
      if (sigaction(SIGPIPE, nullptr, &currentAction) == 0 && currentAction.sa_handler == SIG_DFL) {
        struct sigaction ignoreAction{};
        ignoreAction.sa_handler = SIG_IGN;
        sigemptyset(&ignoreAction.sa_mask);
        (void)sigaction(SIGPIPE, &ignoreAction, nullptr);
      }
    });
    savedErrno = 0;
    if (fd < 0 || encodedFrame.empty() || encodedFrame.size() > kAdminMaxFrameBytes) {
      savedErrno = EINVAL;
      return false;
    }
    std::size_t offset = 0;
    while (offset < encodedFrame.size()) {
      const ssize_t written = ::write(fd, encodedFrame.data() + offset, encodedFrame.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR)
        continue;
      savedErrno = written < 0 ? errno : EIO;
      return false;
    }
    return true;
  }

  AdminFrameReadStatus readAdminFrameFd(int fd, AdminLineBuffer& buffer, std::string& lineOut, int& savedErrno) {
    savedErrno = 0;
    if (fd < 0) {
      savedErrno = EBADF;
      return AdminFrameReadStatus::IoError;
    }
    while (true) {
      const auto bufferedStatus = buffer.popFrame(lineOut);
      if (bufferedStatus != AdminFrameReadStatus::NeedMoreData)
        return bufferedStatus;

      std::array<char, 4096> chunk{};
      const ssize_t bytesRead = ::read(fd, chunk.data(), chunk.size());
      if (bytesRead > 0) {
        buffer.append(std::string_view(chunk.data(), static_cast<std::size_t>(bytesRead)));
        continue;
      }
      if (bytesRead == 0) {
        if (buffer.hasBufferedBytes()) {
          buffer.reset();
          return AdminFrameReadStatus::MalformedFrame;
        }
        return AdminFrameReadStatus::EndOfFile;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return AdminFrameReadStatus::NeedMoreData;
      savedErrno = errno;
      return AdminFrameReadStatus::IoError;
    }
  }

} // namespace filemanager
