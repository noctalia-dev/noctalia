#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  using AdminRequestId = std::uint64_t;
  inline constexpr AdminRequestId kInvalidAdminRequestId = 0;

  inline constexpr std::uint32_t kAdminProtocolVersion = 5;
  inline constexpr std::size_t kAdminMaxFrameBytes = 65536;
  inline constexpr std::size_t kAdminMaxMessageBytes = 4096;
  inline constexpr std::size_t kAdminMaxNonceBytes = 128;
  inline constexpr std::size_t kAdminMaxCopySources = 256;
  inline constexpr std::size_t kAdminMaxPathBytes = 4096;
  inline constexpr std::uint32_t kAdminDefaultDirBatchEntries = 32;
  inline constexpr std::uint32_t kAdminMaxDirBatchEntries = 64;
  inline constexpr std::uint64_t kAdminMaxDirectoryEntries = 100000;

  inline constexpr int kDefaultAdminInactivityTimeoutSeconds = 600; // 10 minutes
  inline constexpr int kMinAdminInactivityTimeoutSeconds = 60;      // 1 minute
  inline constexpr int kMaxAdminInactivityTimeoutSeconds = 3600;    // 60 minutes

  enum class AdminStatusCode : std::uint32_t {
    Ok = 0,
    Cancelled = 1,
    VersionMismatch = 2,
    BadRequest = 3,
    HandshakeRequired = 4,
    Unsupported = 5,
    Unauthorized = 6,
    ShuttingDown = 7,
    InternalError = 8,
    NotFound = 9,
    NotDirectory = 10,
    AccessDenied = 11,
    AlreadyExists = 12,
    ReadOnlyFilesystem = 13,
    IdentityMismatch = 14,
    NotSupported = 15,
  };

  [[nodiscard]] std::string_view adminStatusCodeLabel(AdminStatusCode code) noexcept;
  [[nodiscard]] bool parseAdminStatusCode(std::uint32_t raw, AdminStatusCode& out) noexcept;

  enum class AdminClientCommand : std::uint8_t {
    Hello,
    Ping,
    RenewActivity,
    Cancel,
    Shutdown,
    ListDir,
    StatInfo,
    StatVfs,
    CountDir,
    CreateFile,
    CreateDir,
    RenameEntry,
    CopyFile,
    CopyItems,
    // Reserved for future conflict resolution responses on in-flight operations.
    ConflictReply,
  };

  struct AdminCopySource {
    std::string path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    bool directory = false;
  };

  struct AdminClientRequest {
    AdminClientCommand command = AdminClientCommand::Ping;
    AdminRequestId requestId = kInvalidAdminRequestId;
    std::uint32_t protocolVersion = kAdminProtocolVersion;
    std::uint32_t inactivityTimeoutMs = 0;
    std::string nonce;
    AdminRequestId targetRequestId = kInvalidAdminRequestId;
    std::string path;
    std::string entryName;
    std::string sourceName;
    std::string sourcePath;
    AdminRequestId directoryListingId = kInvalidAdminRequestId;
    std::string destName;
    std::uint32_t maxBatchEntries = kAdminDefaultDirBatchEntries;
    bool userInitiated = false;
    bool autoNumber = false;
    std::uint64_t expectedDevice = 0;
    std::uint64_t expectedInode = 0;
    bool expectedIsDir = false;
    bool expectedIsSymlink = false;
    std::vector<AdminCopySource> copySources;
    // Reserved fields for future ConflictReply frames.
    std::uint64_t conflictId = 0;
    std::uint32_t conflictDecision = 0;
    std::string alternativeName;
  };

  struct AdminDirEntryRecord {
    std::string name;
    bool isDir = false;
    bool isSymlink = false;
    bool brokenSymlink = false;
    bool hasIdentity = false;
    std::uint64_t size = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t mtimeSec = 0;
    bool hasCreated = false;
    std::uint64_t createdSec = 0;
    bool hasAccessed = false;
    std::uint64_t accessedSec = 0;
    std::string contentType;
  };

  struct AdminStatRecord {
    bool exists = false;
    bool isDir = false;
    bool isSymlink = false;
    bool brokenSymlink = false;
    bool writable = false;
    bool readable = false;
    bool hasSize = false;
    std::uint64_t size = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t mtimeSec = 0;
    bool hasCreated = false;
    std::uint64_t createdSec = 0;
    bool hasAccessed = false;
    std::uint64_t accessedSec = 0;
    std::string contentType;
    std::string symlinkTarget;
  };

  struct AdminStatVfsRecord {
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t availableBytes = 0;
    bool readOnly = false;
  };

  struct AdminMutationRecord {
    std::string name;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
  };

  enum class AdminServerFrameKind : std::uint8_t {
    Ready,
    Status,
    Response,
    DirBatch,
    DirEnd,
    StatResult,
    StatVfsResult,
    CountResult,
    MutationResult,
    Progress,
    CopyItem,
    // Reserved for future conflict frames.
    Conflict,
  };

  struct AdminServerFrame {
    AdminServerFrameKind kind = AdminServerFrameKind::Response;
    AdminRequestId requestId = kInvalidAdminRequestId;
    std::uint32_t protocolVersion = kAdminProtocolVersion;
    std::uint32_t helperPid = 0;
    std::uint32_t invokingUid = 0;
    std::uint64_t uptimeMs = 0;
    AdminRequestId activeRequestId = kInvalidAdminRequestId;
    std::string nonce;
    AdminStatusCode status = AdminStatusCode::Ok;
    std::string detail;
    // Stage 2 read-only browsing payloads.
    std::vector<AdminDirEntryRecord> entries;
    bool directoryWritable = false;
    std::uint64_t totalEntries = 0;
    AdminStatRecord statInfo;
    AdminStatVfsRecord statVfsInfo;
    std::uint64_t itemCount = 0;
    // Stage 3 controlled write result payload.
    AdminMutationRecord mutationInfo;
    // Transfer progress.
    std::uint64_t completedItems = 0;
    std::uint64_t totalItems = 0;
    std::uint64_t completedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::string currentItem;
    // Reserved conflict payload.
    std::uint64_t conflictId = 0;
    std::string sourcePath;
    std::string destinationPath;
  };

  enum class AdminParseStatus : std::uint8_t {
    Ok,
    EmptyFrame,
    OversizedFrame,
    ControlCharacter,
    UnknownFrameKind,
    MalformedFields,
    UnsupportedCommand,
  };

  struct AdminParseError {
    AdminParseStatus status = AdminParseStatus::Ok;
    AdminRequestId requestId = kInvalidAdminRequestId;
    std::string message;
  };

  // Encodes/decodes a single line frame (returned string always ends with '\n').
  [[nodiscard]] std::optional<std::string> encodeAdminClientRequest(const AdminClientRequest& request);
  [[nodiscard]] bool
  decodeAdminClientRequest(std::string_view line, AdminClientRequest& out, AdminParseError* errorOut = nullptr);

  [[nodiscard]] std::optional<std::string> encodeAdminServerFrame(const AdminServerFrame& frame);
  [[nodiscard]] bool
  decodeAdminServerFrame(std::string_view line, AdminServerFrame& out, AdminParseError* errorOut = nullptr);

  enum class AdminFrameReadStatus : std::uint8_t {
    FrameReady,
    NeedMoreData,
    OversizedFrame,
    MalformedFrame,
    EndOfFile,
    IoError,
  };

  // Bounded line accumulator handling partial reads, coalesced frames, and oversized frame rejection.
  class AdminLineBuffer {
  public:
    explicit AdminLineBuffer(std::size_t maxFrameBytes = kAdminMaxFrameBytes) noexcept;

    void reset() noexcept;

    // Appends raw bytes from transport. If an unterminated line exceeds maxFrameBytes,
    // latches an oversized condition immediately without unbounded allocation.
    void append(std::string_view chunk);

    // Extracts the next complete frame line (excluding the trailing '\n').
    [[nodiscard]] AdminFrameReadStatus popFrame(std::string& lineOut);

    [[nodiscard]] bool hasBufferedBytes() const noexcept { return !m_buffer.empty(); }
    [[nodiscard]] std::size_t maxFrameBytes() const noexcept { return m_maxFrameBytes; }

  private:
    std::size_t m_maxFrameBytes = kAdminMaxFrameBytes;
    std::string m_buffer;
    bool m_oversizedLatched = false;
  };

  // Writes an entire encoded frame across a file descriptor, handling partial writes and EINTR.
  [[nodiscard]] bool writeAdminFrameFd(int fd, std::string_view encodedFrame, int& savedErrno) noexcept;

  // Reads from a file descriptor into `buffer` until a frame is ready, EOF, or an error occurs.
  [[nodiscard]] AdminFrameReadStatus
  readAdminFrameFd(int fd, AdminLineBuffer& buffer, std::string& lineOut, int& savedErrno);

} // namespace filemanager
