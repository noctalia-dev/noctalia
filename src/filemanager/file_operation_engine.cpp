#include "filemanager/file_operation_engine.h"

#include "filemanager/file_interaction.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace filemanager::file_operation_engine {
  namespace {
    ConflictItemMetadata queryConflictItem(GFile* file, GFileType type, GCancellable* cancellable) {
      ConflictItemMetadata item{.type = type};
      GFileInfo* info = g_file_query_info(
          file, "standard::size,standard::content-type,time::modified", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
          cancellable, nullptr
      );
      if (info != nullptr) {
        item.inspected = true;
        if (const char* contentType = g_file_info_get_content_type(info))
          item.contentType = contentType;
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
          item.size = g_file_info_get_size(info);
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
          item.modified = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        g_object_unref(info);
      }
      return item;
    }

    bool isCopy(Kind kind) { return kind == Kind::Copy || kind == Kind::Duplicate; }
    bool isTransfer(Kind kind) { return isCopy(kind) || kind == Kind::Move; }

    std::string numberedName(const std::string& stem, const std::string& extension, unsigned suffix, bool copyStyle) {
      if (!copyStyle)
        return stem + (suffix == 1 ? std::string{} : "-" + std::to_string(suffix)) + extension;
      return stem + " (copy" + (suffix == 1 ? std::string{} : " " + std::to_string(suffix)) + ")" + extension;
    }

    bool isSameOrDescendant(const std::filesystem::path& parent, const std::filesystem::path& child) {
      std::error_code ec;
      auto p = std::filesystem::weakly_canonical(parent, ec);
      if (ec)
        return true;
      auto c = std::filesystem::weakly_canonical(child, ec);
      if (ec)
        return true;
      auto pi = p.begin();
      auto ci = c.begin();
      for (; pi != p.end(); ++pi, ++ci)
        if (ci == c.end() || *pi != *ci)
          return false;
      return true;
    }

    void ensureWritableDirectoriesForCleanup(const std::string& path) {
      int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        if (errno == EACCES) {
          struct stat state{};
          if (lstat(path.c_str(), &state) == 0 && S_ISDIR(state.st_mode))
            (void)chmod(path.c_str(), (state.st_mode & 07777) | S_IRWXU);
          fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (fd < 0)
          return;
      }
      struct stat state{};
      if (fstat(fd, &state) != 0 || !S_ISDIR(state.st_mode)) {
        close(fd);
        return;
      }
      if ((state.st_mode & S_IRWXU) != S_IRWXU)
        (void)fchmod(fd, (state.st_mode & 07777) | S_IRWXU);
      DIR* dir = fdopendir(fd);
      if (dir == nullptr) {
        close(fd);
        return;
      }
      while (struct dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
          continue;
        ensureWritableDirectoriesForCleanup(path + "/" + entry->d_name);
      }
      closedir(dir);
    }

    using ReplaceTargetSnapshot = fs_compat::EntryIdentity;

    std::optional<ReplaceTargetSnapshot> queryReplaceTargetSnapshot(const char* targetPath) {
      return fs_compat::queryEntryIdentity(targetPath);
    }

    bool allocateReplacementBackup(
        GFile* target, const std::optional<ReplaceTargetSnapshot>& expected,
        std::optional<fs_compat::ReplacementBackup>& backup, std::string& message, GError** error
    ) {
      g_autofree char* targetPath = g_file_get_path(target);
      if (targetPath == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Replacement requires a local path.");
        return false;
      }
      return fs_compat::allocateReplacementBackup(targetPath, expected, backup, message, error);
    }

    constexpr std::size_t kMaxUndoTemplateBytes = 1024ULL * 1024ULL;

    struct TemplateFd {
      int fd = -1;
      ~TemplateFd() {
        if (fd >= 0)
          close(fd);
      }
    };

    bool readBoundedTemplateFile(
        const std::string& path, std::size_t maxBytes, std::vector<std::uint8_t>& out, bool& exceededLimit,
        TemplateFd& source, std::string& pinnedPath, GCancellable* cancellable, GError** error
    ) {
      out.clear();
      exceededLimit = false;
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return false;
      const auto ioError = [&](const char* action, int savedErrno) {
        if (!g_cancellable_set_error_if_cancelled(cancellable, error))
          g_set_error(error, G_IO_ERROR, g_io_error_from_errno(savedErrno), "%s: %s", action, g_strerror(savedErrno));
        return false;
      };

      // Pin and inspect the resolved inode without opening a FIFO or device for I/O.
      source.fd = open(path.c_str(), O_PATH | O_CLOEXEC);
      if (source.fd < 0)
        return ioError("Could not open template source", errno);
      struct stat state{};
      if (fstat(source.fd, &state) != 0)
        return ioError("Could not inspect template source", errno);
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return false;
      if (!S_ISREG(state.st_mode)) {
        g_set_error_literal(
            error, G_IO_ERROR, S_ISDIR(state.st_mode) ? G_IO_ERROR_IS_DIRECTORY : G_IO_ERROR_NOT_SUPPORTED,
            "Template source must be a regular file."
        );
        return false;
      }

      pinnedPath = "/proc/self/fd/" + std::to_string(source.fd);
      TemplateFd input{open(pinnedPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
      if (input.fd < 0)
        return ioError("Could not open template source for reading", errno);
      std::array<std::uint8_t, 16384> buffer{};
      while (true) {
        if (g_cancellable_set_error_if_cancelled(cancellable, error))
          return false;
        const ssize_t count = read(input.fd, buffer.data(), buffer.size());
        if (count < 0) {
          if (errno == EINTR)
            continue;
          return ioError("Could not read template source", errno);
        }
        if (count == 0)
          return g_close(std::exchange(input.fd, -1), error);
        if (out.size() + static_cast<std::size_t>(count) > maxBytes) {
          exceededLimit = true;
          out.clear();
          return true;
        }
        out.insert(out.end(), buffer.begin(), buffer.begin() + count);
      }
    }

  } // namespace

  void FileTransferPauseState::pause() {
    std::lock_guard lock(m_mutex);
    m_requested = true;
  }
  void FileTransferPauseState::resume() {
    std::lock_guard lock(m_mutex);
    m_requested = false;
    m_changed.notify_all();
  }
  void FileTransferPauseState::cancel() {
    std::lock_guard lock(m_mutex);
    m_cancelled = true;
    m_changed.notify_all();
  }
  bool FileTransferPauseState::requested() const {
    std::lock_guard lock(m_mutex);
    return m_requested;
  }
  std::chrono::steady_clock::duration FileTransferPauseState::pausedTime() const {
    std::lock_guard lock(m_mutex);
    return m_pausedTime;
  }
  bool FileTransferPauseState::checkpoint(GCancellable* cancellable, const std::function<void(bool)>& acknowledge) {
    std::unique_lock lock(m_mutex);
    if (m_requested && !m_cancelled && !g_cancellable_is_cancelled(cancellable)) {
      const auto started = std::chrono::steady_clock::now();
      lock.unlock();
      if (acknowledge)
        acknowledge(true);
      lock.lock();
      m_changed.wait(lock, [&] { return !m_requested || m_cancelled || g_cancellable_is_cancelled(cancellable); });
      m_pausedTime += std::chrono::steady_clock::now() - started;
      lock.unlock();
      if (acknowledge)
        acknowledge(false);
      lock.lock();
    }
    return !m_cancelled && !g_cancellable_is_cancelled(cancellable);
  }

  bool measureTree(GFile* file, GCancellable* cancellable, TransferMeasure& measure, GError** error) {
    GFileInfo* info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        cancellable, error
    );
    if (info == nullptr)
      return false;
    const auto type = g_file_info_get_file_type(info);
    if (type != G_FILE_TYPE_DIRECTORY) {
      measure.bytes += g_file_info_get_size(info);
      ++measure.items;
      g_object_unref(info);
      return true;
    }
    g_object_unref(info);
    GFileEnumerator* entries = g_file_enumerate_children(
        file, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
    );
    if (entries == nullptr)
      return false;
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
      GFile* next = g_file_get_child(file, g_file_info_get_name(child));
      const bool ok = measureTree(next, cancellable, measure, error);
      g_object_unref(next);
      g_object_unref(child);
      if (!ok) {
        g_object_unref(entries);
        return false;
      }
    }
    const bool ok = error == nullptr || *error == nullptr;
    g_object_unref(entries);
    return ok;
  }

  void publishProgress(TransferProgress& context, std::uint64_t currentBytes, bool force) {
    const auto now = std::chrono::steady_clock::now();
    const auto paused = context.pause ? context.pause->pausedTime() : std::chrono::steady_clock::duration{};
    const auto activeElapsed = now - context.lastReport - (paused - context.reportedPause);
    if (!force && activeElapsed < std::chrono::milliseconds(100))
      return;
    const std::uint64_t total = std::max(context.progressedBytes, context.baseBytes + currentBytes);
    const double seconds = std::chrono::duration<double>(activeElapsed).count();
    const std::uint64_t speed =
        seconds > 0.0 ? static_cast<std::uint64_t>(static_cast<double>(total - context.lastBytes) / seconds) : 0;
    context.lastReport = now;
    context.reportedPause = paused;
    context.lastBytes = total;
    if (context.callbacks.report)
      context.callbacks.report(total, context.totalBytes, context.completedItems, context.totalItems, speed);
    context.progressedBytes = total;
  }

  bool transferCheckpoint(TransferProgress* progress, std::uint64_t currentBytes) {
    if (!progress || !progress->pause)
      return true;
    if (progress->callbacks.boundary)
      progress->callbacks.boundary(progress->baseBytes + currentBytes);
    if (progress->pause->requested())
      publishProgress(*progress, currentBytes, true);
    return progress->pause->checkpoint(progress->cancellable, progress->callbacks.acknowledgePause);
  }

  bool removeTree(GFile* file, GCancellable* cancellable, GError** error, bool* mutated) {
    if (cancellable != nullptr && g_cancellable_set_error_if_cancelled(cancellable, error))
      return false;
    GFileInfo* info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
    );
    if (info == nullptr)
      return false;
    const auto type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if (type == G_FILE_TYPE_DIRECTORY) {
      GFileEnumerator* entries = g_file_enumerate_children(
          file, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
      );
      if (entries == nullptr)
        return false;
      while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
        const char* name = g_file_info_get_name(child);
        GFile* childFile = g_file_get_child(file, name);
        const bool ok = removeTree(childFile, cancellable, error, mutated);
        g_object_unref(childFile);
        g_object_unref(child);
        if (!ok) {
          g_object_unref(entries);
          return false;
        }
      }
      if (error != nullptr && *error != nullptr) {
        g_object_unref(entries);
        return false;
      }
      g_object_unref(entries);
    }
    if (fs_compat::removeEntryHookActive()) {
      if (g_autofree char* path = g_file_get_path(file); path != nullptr) {
        if (const int injectedErrno = fs_compat::invokeRemoveEntryHook(path); injectedErrno != 0) {
          g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(injectedErrno), g_strerror(injectedErrno));
          return false;
        }
      }
    }
    const bool removed = g_file_delete(file, cancellable, error);
    if (removed && mutated)
      *mutated = true;
    return removed;
  }

  bool entryExistsNoFollow(GFile* file) {
    if (file == nullptr)
      return false;
    GFileInfo* info =
        g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    if (info == nullptr)
      return false;
    g_object_unref(info);
    return true;
  }

  void cleanupStagedTree(GFile* file) {
    if (file == nullptr)
      return;
    if (g_autofree char* path = g_file_get_path(file); path != nullptr)
      ensureWritableDirectoriesForCleanup(path);
    GError* cleanupError = nullptr;
    removeTree(file, nullptr, &cleanupError);
    g_clear_error(&cleanupError);
  }

  fs_compat::EntryIdentity toCompatIdentity(const Revision& rev) {
    return {
        .device = rev.device,
        .inode = rev.inode,
        .mode = rev.mode,
        .links = rev.links,
    };
  }

  bool reject(GError** error, const char* message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
    return false;
  }

  void setErrorFromPublishOutcome(const fs_compat::PublishOutcome& outcome, GError** error) {
    if (error == nullptr)
      return;
    const int err = outcome.savedErrno ? outcome.savedErrno : EIO;
    const GIOErrorEnum code = err == EXDEV ? G_IO_ERROR_NOT_SUPPORTED : g_io_error_from_errno(err);
    const char* msg = outcome.message.empty() ? g_strerror(err) : outcome.message.c_str();
    g_set_error_literal(error, G_IO_ERROR, code, msg);
  }

  bool copyTree(
      GFile* source, GFile* destination, GCancellable* cancellable, GError** error, TransferProgress* progress,
      bool preserveDirectoryMetadata
  ) {
    if (!transferCheckpoint(progress) || (cancellable != nullptr && g_cancellable_is_cancelled(cancellable))) {
      g_cancellable_set_error_if_cancelled(cancellable, error);
      return false;
    }
    GFileInfo* info = g_file_query_info(
        source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
    );
    if (info == nullptr)
      return false;
    const auto type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if (type != G_FILE_TYPE_DIRECTORY) {
      if (progress != nullptr)
        progress->baseBytes = progress->progressedBytes;
      const bool copied = g_file_copy(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable,
          progress == nullptr ? nullptr : [](goffset current, goffset, gpointer data) {
            auto* context = static_cast<TransferProgress*>(data);
            const auto bytes = static_cast<std::uint64_t>(std::max<goffset>(0, current));
            if (transferCheckpoint(context, bytes)) publishProgress(*context, bytes);
          }, progress, error);
      return copied;
    }
    if (!g_file_make_directory(destination, cancellable, error))
      return false;
    GFileEnumerator* entries = g_file_enumerate_children(
        source, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        cancellable, error
    );
    if (entries == nullptr)
      return false;
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
      const char* name = g_file_info_get_name(child);
      GFile* from = g_file_get_child(source, name);
      GFile* to = g_file_get_child(destination, name);
      const bool ok = copyTree(from, to, cancellable, error, progress, preserveDirectoryMetadata);
      g_object_unref(from);
      g_object_unref(to);
      g_object_unref(child);
      if (!ok) {
        g_object_unref(entries);
        return false;
      }
    }
    if (error != nullptr && *error != nullptr) {
      g_object_unref(entries);
      return false;
    }
    g_object_unref(entries);
    if (preserveDirectoryMetadata) {
      // Directory size is not a writable attribute. Copy only the relevant
      // mode and timestamps after children have finished changing the directory.
      GFileInfo* metadata = g_file_query_info(
          source, "unix::mode,time::modified,time::modified-usec,time::access,time::access-usec",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
      );
      if (metadata == nullptr)
        return false;
      const bool copied = g_file_set_attributes_from_info(
          destination, metadata, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
      );
      g_object_unref(metadata);
      return copied;
    }
    return true;
  }

  FileIdentity parentIdentity(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    struct stat state{};
    if (parent.empty() || stat(parent.c_str(), &state) != 0 || !S_ISDIR(state.st_mode))
      return {};
    return {state.st_dev, state.st_ino};
  }

  std::optional<Revision> revision(const std::string& path) {
    struct stat state{};
    if (lstat(path.c_str(), &state) != 0)
      return std::nullopt;
    return Revision{
        state.st_dev,
        state.st_ino,
        state.st_nlink,
        state.st_mode,
        state.st_size,
        {state.st_mtim.tv_sec, state.st_mtim.tv_nsec},
        {state.st_ctim.tv_sec, state.st_ctim.tv_nsec}
    };
  }

  bool createItem(
      Kind kind, GFile* target, const std::string& link, const std::optional<std::vector<std::uint8_t>>& content,
      const std::string& pinnedTemplatePath, GCancellable* cancellable, GError** error, bool& created
  ) {
    if (kind == Kind::CreateSymlink)
      return created = g_file_make_symbolic_link(target, link.c_str(), cancellable, error);
    if (kind == Kind::CreateFolder)
      return created = g_file_make_directory(target, cancellable, error);
    if (kind == Kind::CreateFile && !content && !pinnedTemplatePath.empty()) {
      // Follow the descriptor link to the regular file validated by the reader.
      GFile* source = g_file_new_for_path(pinnedTemplatePath.c_str());
      const bool copied =
          g_file_copy(source, target, G_FILE_COPY_TARGET_DEFAULT_PERMS, cancellable, nullptr, nullptr, error);
      g_object_unref(source);
      if (copied)
        created = true;
      return copied;
    }
    if (g_autofree char* targetPath = g_file_get_path(target); targetPath != nullptr) {
      return fs_compat::writeBufferToNewFileExclusive(
          targetPath, content ? content->data() : nullptr, content ? content->size() : 0, cancellable, created, error
      );
    }
    GFileOutputStream* stream = g_file_create(target, G_FILE_CREATE_NONE, cancellable, error);
    if (!stream)
      return false;
    created = true;
    bool ok = true;
    if (content && !content->empty()) {
      gsize written = 0;
      ok = g_output_stream_write_all(
               G_OUTPUT_STREAM(stream), content->data(), content->size(), &written, cancellable, error
           )
          != FALSE;
    }
    const bool closed = g_output_stream_close(G_OUTPUT_STREAM(stream), cancellable, ok ? error : nullptr) != FALSE;
    g_object_unref(stream);
    return ok && closed;
  }

  PrivateStaging::PrivateStaging(const std::string& sibling)
      : directory((std::filesystem::path(sibling).parent_path() / ".nythera-stage-XXXXXX").string()) {
    if (!g_mkdtemp(directory.data()))
      directory.clear();
  }

  PrivateStaging::~PrivateStaging() {
    if (!directory.empty())
      rmdir(directory.c_str());
  }

  ConflictMetadata queryConflictMetadata(
      GFile* incoming, GFile* existing, GFileType incomingType, GFileType existingType, GCancellable* cancellable
  ) {
    return {
        .incoming = queryConflictItem(incoming, incomingType, cancellable),
        .existing = queryConflictItem(existing, existingType, cancellable),
        .sameItem = g_file_equal(incoming, existing) != FALSE,
    };
  }

  Result executeLocalOperation(
      Request& request, GCancellable* cancellable, FileTransferPauseState* pause, const Callbacks& callbacks
  ) {
    Result result;
    std::vector<TransferMeasure> measures;
    TransferProgress progress{.callbacks = callbacks.transfer, .pause = pause, .cancellable = cancellable};
    const auto remember =
        [&](const std::string& source, const std::string& destination, const std::optional<Revision>& sourceRevision,
            const std::optional<Revision>& resultRevision, FileIdentity sourceParent, FileIdentity destinationParent,
            std::string linkTarget = {}, std::optional<std::vector<std::uint8_t>> createdContent = std::nullopt) {
          if (callbacks.recordChange)
            callbacks.recordChange(
                source, destination, sourceRevision, resultRevision, sourceParent, destinationParent,
                std::move(linkTarget), std::move(createdContent)
            );
        };
    if (request.renameIdentity) {
      struct stat state{};
      const auto& path = request.sources.front();
      const auto destination = std::filesystem::path(request.destination);
      if (!filemanager::validFileName(destination.filename().string())
          || destination.parent_path() != std::filesystem::path(path).parent_path()
          || lstat(path.c_str(), &state) != 0
          || static_cast<std::uint64_t>(state.st_dev) != request.renameIdentity->first
          || state.st_ino != request.renameIdentity->second) {
        result.message = "The item changed or the new filename is invalid. Reopen Properties if the item was replaced.";
        return result;
      }
    }
    GError* error = nullptr;
    auto fail = [&](const char* fallback) {
      result.ok = false;
      result.message = error == nullptr ? fallback : error->message;
      result.cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_clear_error(&error);
    };
    if (request.kind == Kind::CreateSymlink || request.kind == Kind::CreateFolder || request.kind == Kind::CreateFile) {
      const bool symlink = request.kind == Kind::CreateSymlink;
      auto destination = std::filesystem::path(request.destination);
      const auto link = symlink ? request.sources.front() : std::string{};
      const auto templateSource =
          request.kind == Kind::CreateFile && !request.sources.empty() ? request.sources.front() : std::string{};
      TemplateFd pinnedTemplate;
      std::string pinnedTemplatePath;
      std::optional<std::vector<std::uint8_t>> templateSnapshot;
      bool templateTooLargeForHistory = false;
      bool templateReadFailed = false;
      if (!templateSource.empty()) {
        std::vector<std::uint8_t> bytes;
        if (!readBoundedTemplateFile(
                templateSource, kMaxUndoTemplateBytes, bytes, templateTooLargeForHistory, pinnedTemplate,
                pinnedTemplatePath, cancellable, &error
            )) {
          templateReadFailed = true;
        } else if (!templateTooLargeForHistory) {
          templateSnapshot = std::move(bytes);
        }
      }
      const auto parent = parentIdentity(request.destination);
      const auto source = request.keepBoth && std::filesystem::path(link).is_absolute() ? revision(link) : std::nullopt;
      if (templateReadFailed) {
        // Preserve GError set by readBoundedTemplateFile.
      } else if (
          symlink
          && (parent == FileIdentity{}
              || !filemanager::validFileName(destination.filename().string())
              || link.empty()
              || link.find('\0') != std::string::npos)
      ) {
        reject(&error, "Enter a valid name and target path.");
      } else if (request.keepBoth && (!source || (!S_ISREG(source->mode) && !S_ISDIR(source->mode)))) {
        reject(&error, "The selected file or folder is no longer available.");
      } else {
        const auto filename = destination.filename().string();
        const auto extension = source && S_ISREG(source->mode) ? filename.find('.', 1) : std::string::npos;
        const auto stem = filename.substr(0, extension);
        const auto suffix = extension == std::string::npos ? std::string{} : filename.substr(extension);
        const auto directory = destination.parent_path();
        const auto caps = fs_compat::probeCapabilities(directory);
        const bool directExclusiveFolder =
            request.kind == Kind::CreateFolder && caps.renameNoReplace == fs_compat::CapabilitySupport::Unsupported;
        std::optional<PrivateStaging> staging;
        std::string staged;
        std::optional<Revision> stagedState;
        bool useDirectCreate = directExclusiveFolder;
        if (!useDirectCreate) {
          staging.emplace(request.destination);
          staged = staging->directory + "/item";
          useDirectCreate = staging->directory.empty();
        }
        if (!useDirectCreate) {
          GFile* target = g_file_new_for_path(staged.c_str());
          bool created = false;
          if (createItem(
                  request.kind, target, link, templateSnapshot, pinnedTemplatePath, cancellable, &error, created
              ))
            stagedState = revision(staged);
          g_object_unref(target);
        }
        for (unsigned number = 1; (stagedState || useDirectCreate) && number < 10000; ++number) {
          destination = directory / numberedName(stem, suffix, number, false);
          if (g_cancellable_set_error_if_cancelled(cancellable, &error))
            break;
          if (request.keepBoth) {
            const auto current = revision(link);
            if (!current
                || current->device != source->device
                || current->inode != source->inode
                || (current->mode & S_IFMT) != (source->mode & S_IFMT)) {
              reject(&error, "The selected item was replaced before the link could be created.");
              break;
            }
          }
          if (stagedState && !useDirectCreate) {
            const auto dstStr = destination.string();
            const auto outcome = request.kind == Kind::CreateFolder
                ? fs_compat::moveOrPublishDirectoryOptionA(
                      staged.c_str(), dstStr.c_str(), true, toCompatIdentity(*stagedState)
                  )
                : fs_compat::moveOrPublishNonDirectory(
                      staged.c_str(), dstStr.c_str(), true, toCompatIdentity(*stagedState), symlink
                  );
            result.ok = outcome.ok;
            result.mutated |= outcome.destinationCommitted;
            if (!outcome.ok) {
              if (outcome.splitState) {
                result.message = outcome.message;
                result.customDescription = outcome.message;
                setErrorFromPublishOutcome(outcome, &error);
                break;
              }
              setErrorFromPublishOutcome(outcome, &error);
            }
            useDirectCreate = !result.ok && outcome.unsupported;
            if (useDirectCreate)
              g_clear_error(&error);
          }
          if (useDirectCreate) {
            // Preserve creation on providers without native no-replace rename.
            if (request.kind == Kind::CreateFolder) {
              int mkdirErrno = 0;
              const auto dstStr = destination.string();
              result.ok = fs_compat::createDirectoryExclusive(dstStr.c_str(), 0777, mkdirErrno, &error);
              result.mutated |= result.ok;
            } else {
              GFile* target = g_file_new_for_path(destination.c_str());
              result.ok = createItem(
                  request.kind, target, link, templateSnapshot, pinnedTemplatePath, cancellable, &error, result.mutated
              );
              g_object_unref(target);
            }
          }
          if (result.ok || !request.keepBoth || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            break;
          g_clear_error(&error);
        }
        if (staging && !staging->directory.empty()) {
          if (request.kind == Kind::CreateFolder)
            rmdir(staged.c_str());
          else
            unlink(staged.c_str());
        }
        if (result.ok) {
          result.mutated = true;
          request.destination = destination.string();
          const auto created = revision(request.destination);
          if (created && stagedState && !useDirectCreate) {
            stagedState->changed = created->changed;
            if (created == stagedState && !templateTooLargeForHistory) {
              remember({}, request.destination, {}, created, {}, parent, link, std::move(templateSnapshot));
              if (symlink)
                result.completedEntries.push_back({request.destination, created->device, created->inode});
            }
          } else if (created && useDirectCreate && !templateTooLargeForHistory) {
            remember({}, request.destination, {}, created, {}, parent, link, std::move(templateSnapshot));
            if (symlink && S_ISLNK(created->mode))
              result.completedEntries.push_back({request.destination, created->device, created->inode});
          } else if (symlink && created && S_ISLNK(created->mode)) {
            std::error_code linkError;
            if (std::filesystem::read_symlink(destination, linkError).native() == link
                && !linkError
                && revision(request.destination) == created)
              result.completedEntries.push_back({request.destination, created->device, created->inode});
          }
        }
      }
      if (!result.ok)
        fail(symlink ? "Could not create symlink or find an available name." : "Could not create item.");
    } else {
      auto count = request.sources.size();
      std::vector<std::size_t> reportRoots(count);
      for (std::size_t i = 0; i < count; ++i)
        reportRoots[i] = i;
      std::vector<std::string> reportPaths(count);
      std::vector<bool> reportPathSet(count, false);
      std::vector<bool> reportTransferred(count, false);
      std::vector<bool> reportFailed(count, false);
      std::vector<std::optional<CompletedEntry>> reportEntries(count);
      std::vector<std::string> destinationParents(count, request.destination);
      std::vector<std::string> mergedMoveDirectories;
      std::vector<std::size_t> mergedMoveRoots;
      std::optional<ConflictAction> applyConflictToAll;
      std::optional<GFileType> applyConflictItemType;
      std::optional<GFileType> applyConflictDestinationType;
      if (isTransfer(request.kind)) {
        measures.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
          if (!transferCheckpoint(&progress)) {
            g_cancellable_set_error_if_cancelled(cancellable, &error);
            fail("Cancelled");
            break;
          }
          GFile* source = g_file_new_for_path(request.sources[i].c_str());
          const bool measured = measureTree(source, cancellable, measures[i], &error);
          g_object_unref(source);
          if (!measured) {
            fail("Could not calculate transfer size");
            break;
          }
          result.totalMeasure.bytes += measures[i].bytes;
          result.totalMeasure.items += measures[i].items;
        }
        if (result.message.empty() && callbacks.transfer.report)
          callbacks.transfer.report(0, result.totalMeasure.bytes, 0, result.totalMeasure.items, 0);
      }
      std::uint64_t completedBytes = 0;
      std::uint64_t completedItems = 0;
      progress.totalBytes = result.totalMeasure.bytes;
      progress.totalItems = result.totalMeasure.items;
      progress.lastReport = std::chrono::steady_clock::now();
      if (progress.pause)
        progress.reportedPause = progress.pause->pausedTime();
      for (std::size_t i = 0; i < count && result.message.empty(); ++i) {
        transferCheckpoint(&progress);
        if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
          if (isTransfer(request.kind) && i < reportRoots.size())
            reportFailed[reportRoots[i]] = true;
          fail("Cancelled");
          break;
        }
        const std::size_t reportRoot = reportRoots[i];
        const auto& sourcePath = request.sources[i];
        const auto sourceBefore =
            isTransfer(request.kind) || request.kind == Kind::Rename ? revision(sourcePath) : std::nullopt;
        const auto sourceParent = sourceBefore ? parentIdentity(sourcePath) : FileIdentity{};
        GFile* source = g_file_new_for_path(request.sources[i].c_str());
        GFile* target = nullptr;
        if (isTransfer(request.kind)) {
          GFile* parent = g_file_new_for_path(destinationParents[i].c_str());
          target = g_file_get_child(parent, std::filesystem::path(request.sources[i]).filename().string().c_str());
          g_object_unref(parent);
        } else if (request.kind == Kind::Rename)
          target = g_file_new_for_path(request.destination.c_str());
        if (isTransfer(request.kind) && target != nullptr) {
          const bool sameItem = g_file_equal(source, target);
          if (sameItem && request.kind == Kind::Move)
            result.message = "Cannot move an item onto itself";
          if (request.kind == Kind::Duplicate && !sameItem)
            result.message = "Duplicate requires a source in the displayed folder";
          GFileInfo* sourceInfo = g_file_query_info(
              source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error
          );
          if (sourceInfo == nullptr) {
            fail("Could not inspect source");
          } else {
            const bool directory = g_file_info_get_file_type(sourceInfo) == G_FILE_TYPE_DIRECTORY;
            g_object_unref(sourceInfo);
            char* targetPath = g_file_get_path(target);
            if (directory && !sameItem && targetPath != nullptr && isSameOrDescendant(request.sources[i], targetPath))
              result.message = "Cannot copy a directory into itself or one of its descendants";
            g_free(targetPath);
          }
        }
        if (!result.message.empty()) {
          result.ok = false;
          if (isTransfer(request.kind))
            reportFailed[reportRoot] = true;
          if (target != nullptr)
            g_object_unref(target);
          g_object_unref(source);
          break;
        }
        if (request.renameIdentity && target) {
          GFileInfo* existing = g_file_query_info(
              target, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
          );
          if (existing) {
            g_object_unref(existing);
            result.message = "An item with that name already exists.";
            g_object_unref(target);
            g_object_unref(source);
            break;
          }
        }
        bool replaceExisting = false;
        bool skipped = false;
        bool merge = false;
        std::optional<ReplaceTargetSnapshot> existingTargetBefore;
        if (request.kind != Kind::Duplicate
            && (isTransfer(request.kind) || request.kind == Kind::Rename)
            && target != nullptr
            && g_file_query_exists(target, cancellable)) {
          g_autofree char* initialTargetPath = g_file_get_path(target);
          existingTargetBefore = queryReplaceTargetSnapshot(initialTargetPath);
          const bool sameItem = request.kind != Kind::Rename && g_file_equal(source, target);
          GFileInfo* incomingInfo = g_file_query_info(
              source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
          );
          const auto incomingType =
              incomingInfo == nullptr ? G_FILE_TYPE_UNKNOWN : g_file_info_get_file_type(incomingInfo);
          if (incomingInfo != nullptr)
            g_object_unref(incomingInfo);
          const auto existingType = g_file_query_file_type(target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
          bool compatibleBatchConflict = true;
          if (applyConflictToAll) {
            GFileInfo* existingInfo = g_file_query_info(
                target, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
            );
            compatibleBatchConflict = !sameItem
                && incomingType != G_FILE_TYPE_UNKNOWN
                && existingInfo != nullptr
                && incomingType == g_file_info_get_file_type(existingInfo)
                && applyConflictItemType == incomingType
                && applyConflictDestinationType == existingType;
            if (existingInfo != nullptr)
              g_object_unref(existingInfo);
          }
          ConflictDecision decision{.action = ConflictAction::Cancel};
          if (applyConflictToAll && compatibleBatchConflict)
            decision.action = *applyConflictToAll;
          else if (callbacks.resolveConflict && !g_cancellable_is_cancelled(cancellable)) {
            char* destinationPath = g_file_get_path(target);
            const auto metadata = queryConflictMetadata(source, target, incomingType, existingType, cancellable);
            decision = callbacks.resolveConflict(
                request.sources[i], destinationPath == nullptr ? request.destination : destinationPath,
                i + 1 < count || incomingType == G_FILE_TYPE_DIRECTORY, metadata, cancellable
            );
            g_free(destinationPath);
          }
          if (decision.applyToAll) {
            applyConflictToAll = decision.action;
            applyConflictItemType = incomingType;
            applyConflictDestinationType = existingType;
          }
          if (decision.action == ConflictAction::Skip)
            skipped = true;
          else if (
              decision.action == ConflictAction::Merge
              && !sameItem
              && incomingType == G_FILE_TYPE_DIRECTORY
              && existingType == G_FILE_TYPE_DIRECTORY
              && isTransfer(request.kind)
          )
            merge = true;
          else if (
              decision.action == ConflictAction::Replace
              && !sameItem
              && incomingType == existingType
              && incomingType != G_FILE_TYPE_DIRECTORY
              && incomingType != G_FILE_TYPE_UNKNOWN
          )
            replaceExisting = true;
          else if (decision.action == ConflictAction::Rename) {
            GFile* parent = g_file_get_parent(target);
            const auto original = std::filesystem::path(request.sources[i]).filename();
            const auto stem = incomingType == G_FILE_TYPE_DIRECTORY ? original.string() : original.stem().string();
            const auto extension =
                incomingType == G_FILE_TYPE_DIRECTORY ? std::string{} : original.extension().string();
            std::string candidate;
            if (!decision.alternativeName.empty()) {
              const auto& name = decision.alternativeName;
              if (!filemanager::validFileName(name)) {
                result.message = "The alternative name must be a single valid filename";
              } else {
                candidate = name;
                GFile* next = g_file_get_child(parent, candidate.c_str());
                if (g_file_query_exists(next, cancellable)) {
                  result.message = "The alternative name already exists in the destination";
                  g_object_unref(next);
                } else {
                  g_object_unref(target);
                  target = next;
                }
              }
            } else {
              for (unsigned suffix = 1; suffix < 10000; ++suffix) {
                candidate = numberedName(stem, extension, suffix, true);
                GFile* next = g_file_get_child(parent, candidate.c_str());
                const bool exists = g_file_query_exists(next, cancellable);
                if (!exists) {
                  g_object_unref(target);
                  target = next;
                  break;
                }
                g_object_unref(next);
              }
              if (target == nullptr || g_file_query_exists(target, cancellable))
                result.message = "Could not find a free name for the incoming item";
            }
            g_object_unref(parent);
          } else {
            result.message = "Operation cancelled";
            result.cancelled = true;
          }
        }
        if (!result.message.empty()) {
          result.ok = false;
          if (isTransfer(request.kind))
            reportFailed[reportRoot] = true;
          if (target != nullptr)
            g_object_unref(target);
          g_object_unref(source);
          break;
        }
        if (isTransfer(request.kind) && target != nullptr && !reportPathSet[reportRoot]) {
          char* path = g_file_get_path(target);
          if (path != nullptr) {
            reportPaths[reportRoot] = path;
            reportPathSet[reportRoot] = true;
          }
          g_free(path);
        }
        if (merge) {
          // Expand only a pair explicitly approved for merging. Every child still
          // passes through the normal conflict and staged-finalisation path.
          GFileEnumerator* entries = g_file_enumerate_children(
              source, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error
          );
          if (entries == nullptr)
            fail("Could not enumerate directory for merging");
          else {
            char* destinationPath = g_file_get_path(target);
            while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, &error)) {
              GFile* childFile = g_file_get_child(source, g_file_info_get_name(child));
              char* childPath = g_file_get_path(childFile);
              TransferMeasure measure;
              const bool measured = childPath != nullptr && measureTree(childFile, cancellable, measure, &error);
              if (measured) {
                request.sources.emplace_back(childPath);
                destinationParents.emplace_back(destinationPath);
                measures.push_back(measure);
                reportRoots.push_back(reportRoot);
              }
              g_free(childPath);
              g_object_unref(childFile);
              g_object_unref(child);
              if (!measured)
                break;
            }
            g_free(destinationPath);
            g_object_unref(entries);
            if (error != nullptr)
              fail("Could not enumerate directory for merging");
            else if (request.kind == Kind::Move) {
              mergedMoveDirectories.push_back(request.sources[i]);
              mergedMoveRoots.push_back(reportRoot);
            }
            count = request.sources.size();
          }
          if (!result.message.empty())
            reportFailed[reportRoot] = true;
          g_object_unref(target);
          g_object_unref(source);
          continue;
        }
        if (skipped) {
          result.ok = true;
          if (isTransfer(request.kind)) {
            result.totalMeasure.bytes -= measures[i].bytes;
            result.totalMeasure.items -= measures[i].items;
            progress.totalBytes = result.totalMeasure.bytes;
            progress.totalItems = result.totalMeasure.items;
            progress.baseBytes = completedBytes;
            progress.progressedBytes = completedBytes;
            progress.completedItems = completedItems;
            publishProgress(progress, 0, true);
          }
          if (target != nullptr)
            g_object_unref(target);
          g_object_unref(source);
          continue;
        }
        if (request.kind == Kind::Delete)
          result.ok = removeTree(source, cancellable, &error, &result.mutated);
        else if (request.kind == Kind::Rename) {
          g_autofree char* targetPath = g_file_get_path(target);
          const auto targetParent = targetPath ? parentIdentity(targetPath) : FileIdentity{};
          std::optional<fs_compat::ReplacementBackup> backup;
          bool preserveBackup = false;
          result.ok = true;
          if (request.renameIdentity) {
            struct stat state{};
            if (lstat(request.sources.front().c_str(), &state) != 0
                || static_cast<std::uint64_t>(state.st_dev) != request.renameIdentity->first
                || state.st_ino != request.renameIdentity->second) {
              result.ok = false;
              g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "The source item was replaced before rename.");
            }
          }
          if (result.ok && replaceExisting) {
            result.ok = allocateReplacementBackup(target, existingTargetBefore, backup, result.message, &error);
            result.mutated |= result.ok || !result.message.empty();
          }
          if (result.ok) {
            const bool isSymlink = sourceBefore && S_ISLNK(sourceBefore->mode);
            const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
                sourcePath.c_str(), targetPath == nullptr ? "" : targetPath, false,
                sourceBefore ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*sourceBefore)) : std::nullopt,
                isSymlink
            );
            result.ok = outcome.ok;
            result.mutated |= outcome.destinationCommitted;
            if (!outcome.ok) {
              if (outcome.splitState) {
                preserveBackup = backup.has_value();
                result.message = outcome.message
                    + (backup ? "; previous destination backup remains at " + backup->backupPath.string()
                              : std::string{});
                result.customDescription = result.message;
              }
              setErrorFromPublishOutcome(outcome, &error);
            }
          }
          if (result.ok && !replaceExisting && sourceBefore && targetPath) {
            const auto moved = revision(targetPath);
            if (moved) {
              auto expected = *sourceBefore;
              expected.changed = moved->changed;
              if (moved == expected)
                remember(sourcePath, targetPath, sourceBefore, moved, sourceParent, targetParent);
            }
          }
          if (!result.ok && backup && !preserveBackup) {
            fs_compat::restoreOrReportBackup(*backup, "Rename failed", error, result.message);
          } else if (result.ok && backup) {
            result.ok = fs_compat::removeOrReportBackup(
                *backup, "Renamed item, but could not remove the replaced destination backup: ", result.message
            );
          }
        } else {
          g_autofree char* plannedPath = g_file_get_path(target);
          const auto destinationParent = plannedPath ? parentIdentity(plannedPath) : FileIdentity{};
          const bool sourceIsDir = sourceBefore && S_ISDIR(sourceBefore->mode);
          const bool sourceIsSymlink = sourceBefore && S_ISLNK(sourceBefore->mode);
          bool needStagedTransfer = true;
          if (request.kind == Kind::Move) {
            progress.baseBytes = completedBytes;
            progress.progressedBytes = completedBytes;
            progress.completedItems = completedItems;
            if (!transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error)) {
              g_cancellable_set_error_if_cancelled(cancellable, &error);
              if (error == nullptr)
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
              result.ok = false;
              needStagedTransfer = false;
            } else if (!replaceExisting) {
              const bool crossDevice =
                  sourceBefore && destinationParent.first != 0 && sourceBefore->device != destinationParent.first;
              if (crossDevice) {
                needStagedTransfer = true;
              } else {
                const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
                    sourcePath.c_str(), plannedPath == nullptr ? "" : plannedPath, false,
                    sourceBefore ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*sourceBefore))
                                 : std::nullopt,
                    sourceIsSymlink
                );
                result.ok = outcome.ok;
                result.mutated |= outcome.destinationCommitted;
                if (outcome.ok) {
                  needStagedTransfer = false;
                  if (i == reportRoot && sourceBefore && plannedPath != nullptr) {
                    const auto& committedPath = reportPaths[reportRoot];
                    const auto current = revision(committedPath);
                    if (current) {
                      auto expected = *sourceBefore;
                      expected.changed = current->changed;
                      if (current == expected)
                        remember(sourcePath, committedPath, sourceBefore, current, sourceParent, destinationParent);
                    }
                  }
                  completedBytes += measures[i].bytes;
                  completedItems += measures[i].items;
                  result.completedMeasure.bytes = completedBytes;
                  result.completedMeasure.items = completedItems;
                  progress.baseBytes = completedBytes;
                  progress.progressedBytes = completedBytes;
                  progress.completedItems = completedItems;
                  publishProgress(progress, 0, true);
                } else if (!outcome.splitState && (outcome.savedErrno == EXDEV || outcome.unsupported)) {
                  needStagedTransfer = true;
                } else {
                  needStagedTransfer = false;
                  if (outcome.splitState) {
                    result.message = outcome.message;
                    result.customDescription = outcome.message;
                  }
                  setErrorFromPublishOutcome(outcome, &error);
                }
              }
            } else if (
                sourceBefore && destinationParent.first != 0 && sourceBefore->device == destinationParent.first
            ) {
              std::optional<fs_compat::ReplacementBackup> backup;
              result.ok = allocateReplacementBackup(target, existingTargetBefore, backup, result.message, &error);
              result.mutated |= result.ok || !result.message.empty();
              if (!result.ok) {
                needStagedTransfer = false;
              } else {
                const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
                    sourcePath.c_str(), plannedPath == nullptr ? "" : plannedPath, false,
                    sourceBefore ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*sourceBefore))
                                 : std::nullopt,
                    sourceIsSymlink
                );
                result.ok = outcome.ok;
                result.mutated |= outcome.destinationCommitted;
                if (outcome.ok) {
                  needStagedTransfer = false;
                  result.ok = fs_compat::removeOrReportBackup(
                      *backup,
                      "Transferred item, but could not remove the replaced destination backup: ", result.message
                  );
                  if (result.ok) {
                    completedBytes += measures[i].bytes;
                    completedItems += measures[i].items;
                    result.completedMeasure.bytes = completedBytes;
                    result.completedMeasure.items = completedItems;
                    progress.baseBytes = completedBytes;
                    progress.progressedBytes = completedBytes;
                    progress.completedItems = completedItems;
                    publishProgress(progress, 0, true);
                  }
                } else if (!outcome.splitState && (outcome.savedErrno == EXDEV || outcome.unsupported)) {
                  const auto restoreOutcome = fs_compat::moveOrPublishEntryNoReplace(
                      backup->backupPath.c_str(), backup->targetPath.c_str(), true, backup->expectedTarget, true
                  );
                  if (restoreOutcome.ok) {
                    if (backup->usedPrivateContainer && !backup->containerPath.empty())
                      rmdir(backup->containerPath.c_str());
                    g_clear_error(&error);
                    needStagedTransfer = true;
                  } else {
                    setErrorFromPublishOutcome(outcome, &error);
                    needStagedTransfer = false;
                    result.ok = false;
                    result.message = std::string(error == nullptr ? "Operation failed" : error->message)
                        + "; previous destination remains at "
                        + backup->backupPath.string();
                  }
                } else {
                  needStagedTransfer = false;
                  setErrorFromPublishOutcome(outcome, &error);
                  if (outcome.splitState) {
                    result.message =
                        outcome.message + "; previous destination backup remains at " + backup->backupPath.string();
                    result.customDescription = result.message;
                  } else {
                    fs_compat::restoreOrReportBackup(*backup, "Operation failed", error, result.message);
                  }
                }
              }
            }
          }
          if (needStagedTransfer) {
            // Preflight destination capabilities before transferring potentially large data.
            const auto targetDir = plannedPath != nullptr ? std::filesystem::path(plannedPath).parent_path()
                                                          : std::filesystem::path(request.destination);
            if (!fs_compat::preflightTransferDestination(
                    targetDir, sourceIsDir, sourceIsSymlink, replaceExisting, &error
                )) {
              result.ok = false;
            } else {
              // Stage beside the destination so finalisation is a single no-overwrite publish.
              char* uuid = g_uuid_string_random();
              char* leaf = g_strdup_printf(".nythera-partial-%s-%zu", uuid, i);
              g_free(uuid);
              GFile* targetParent = g_file_get_parent(target);
              GFile* staged = g_file_get_child(targetParent, leaf);
              g_object_unref(targetParent);
              g_free(leaf);
              const bool stagedWasAbsent = !entryExistsNoFollow(staged);
              progress.baseBytes = completedBytes;
              progress.progressedBytes = completedBytes;
              progress.completedItems = completedItems;
              if (stagedWasAbsent)
                result.ok = copyTree(source, staged, cancellable, &error, &progress, request.kind == Kind::Move);
              else {
                g_set_error_literal(
                    &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary destination"
                );
                result.ok = false;
              }
              if (result.ok
                  && (!transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error))) {
                g_cancellable_set_error_if_cancelled(cancellable, &error);
                if (error == nullptr)
                  g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
                result.ok = false;
              }
              g_autofree char* stagePath = g_file_get_path(staged);
              const auto stagedRevision = result.ok && stagePath ? revision(stagePath) : std::nullopt;
              std::optional<fs_compat::ReplacementBackup> backup;
              if (result.ok && replaceExisting) {
                result.ok = allocateReplacementBackup(target, existingTargetBefore, backup, result.message, &error);
                result.mutated |= result.ok || !result.message.empty();
              }
              bool publishSplitState = false;
              std::string publishSplitMessage;
              if (result.ok && request.kind == Kind::Duplicate) {
                const auto name = std::filesystem::path(sourcePath).filename();
                const bool directory = sourceBefore && S_ISDIR(sourceBefore->mode);
                const auto stem = directory ? name.string() : name.stem().string();
                const auto extension = directory ? std::string{} : name.extension().string();
                const auto parent = std::filesystem::path(plannedPath).parent_path();
                result.ok = false;
                for (unsigned suffix = 1; suffix < 10000; ++suffix) {
                  const auto candidate = (parent / numberedName(stem, extension, suffix, true)).string();
                  if (!transferCheckpoint(&progress)) {
                    g_cancellable_set_error_if_cancelled(cancellable, &error);
                    if (error == nullptr)
                      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
                    break;
                  }
                  const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
                      stagePath == nullptr ? "" : stagePath, candidate.c_str(), true,
                      stagedRevision ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*stagedRevision))
                                     : std::nullopt,
                      sourceIsSymlink
                  );
                  if (outcome.ok) {
                    reportPaths[reportRoot] = candidate;
                    result.ok = true;
                    break;
                  }
                  if (outcome.splitState) {
                    reportPaths[reportRoot] = candidate;
                    publishSplitState = true;
                    publishSplitMessage = outcome.message;
                    setErrorFromPublishOutcome(outcome, &error);
                    break;
                  }
                  if (outcome.savedErrno != EEXIST) {
                    setErrorFromPublishOutcome(outcome, &error);
                    break;
                  }
                }
                if (!result.ok && !error)
                  reject(&error, "Could not find a free name for the duplicate.");
              } else if (result.ok) {
                const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
                    stagePath == nullptr ? "" : stagePath, plannedPath == nullptr ? "" : plannedPath, true,
                    stagedRevision ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*stagedRevision))
                                   : std::nullopt,
                    sourceIsSymlink
                );
                result.ok = outcome.ok;
                if (!outcome.ok) {
                  publishSplitState = outcome.splitState;
                  publishSplitMessage = outcome.message;
                  setErrorFromPublishOutcome(outcome, &error);
                }
              }
              const bool destinationCommitted = result.ok || publishSplitState;
              result.mutated |= destinationCommitted;
              if (publishSplitState && stagedWasAbsent && entryExistsNoFollow(staged)) {
                cleanupStagedTree(staged);
                if (!entryExistsNoFollow(staged)) {
                  // If cleanupStagedTree removed the staged link after linkat succeeded,
                  // the destination is cleanly committed without leftover staging artifacts.
                  result.ok = true;
                  publishSplitState = false;
                  g_clear_error(&error);
                } else {
                  result.message = publishSplitMessage;
                  result.customDescription = publishSplitMessage;
                }
              }
              const bool stableSource = sourceBefore && revision(sourcePath) == sourceBefore;
              bool preserveBackup = publishSplitState && backup.has_value();
              if (result.ok && destinationCommitted && request.kind == Kind::Move) {
                // Check cancellation and remove source before unlinking backup so both
                // copies and any backup stay intact and reportable on partial failure.
                const bool cancelledBeforeCleanup =
                    !transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error);
                if (cancelledBeforeCleanup) {
                  result.ok = false;
                  result.cancelled = true;
                } else {
                  result.ok = removeTree(source, cancellable, &error, &result.mutated);
                  result.cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
                }
                if (!result.ok) {
                  preserveBackup = backup.has_value();
                  const bool sourceStillExists =
                      !sourcePath.empty() ? revision(sourcePath).has_value() : entryExistsNoFollow(source);
                  result.message = std::string("Moved copy remains at ")
                      + (plannedPath == nullptr ? "the destination" : plannedPath)
                      + (sourceStillExists ? "; source still exists at " + sourcePath
                                           : "; source was already removed from " + sourcePath)
                      + (result.cancelled
                             ? " because the move was cancelled before source cleanup finished"
                             : ": " + std::string(error == nullptr ? "could not remove source" : error->message))
                      + (!backup ? std::string{}
                                 : "; previous destination backup remains at " + backup->backupPath.string());
                  result.customDescription = result.message;
                }
              }
              if (!result.ok && backup && !preserveBackup) {
                fs_compat::restoreOrReportBackup(*backup, "Operation failed", error, result.message);
              } else if (result.ok && backup) {
                result.ok = fs_compat::removeOrReportBackup(
                    *backup, "Transferred item, but could not remove the replaced destination backup: ", result.message
                );
              }
              if (!destinationCommitted && stagedWasAbsent && entryExistsNoFollow(staged)) {
                cleanupStagedTree(staged);
              }
              if (result.ok) {
                if (!replaceExisting && isCopy(request.kind) && i == reportRoot && stagedRevision) {
                  const auto& committedPath = reportPaths[reportRoot];
                  const auto current = revision(committedPath);
                  auto expected = *stagedRevision;
                  if (current) {
                    expected.changed = current->changed;
                    if (current == expected) {
                      if (stableSource)
                        remember(sourcePath, committedPath, sourceBefore, current, sourceParent, destinationParent);
                      if (request.kind == Kind::Duplicate)
                        reportEntries[reportRoot] = CompletedEntry{committedPath, current->device, current->inode};
                    }
                  }
                }
                completedBytes += measures[i].bytes;
                completedItems += measures[i].items;
                result.completedMeasure.bytes = completedBytes;
                result.completedMeasure.items = completedItems;
                progress.baseBytes = completedBytes;
                progress.progressedBytes = completedBytes;
                progress.completedItems = completedItems;
                publishProgress(progress, 0, true);
              }
              g_object_unref(staged);
            }
          }
        }
        if (request.kind == Kind::Delete && result.ok && callbacks.transfer.report) {
          const auto done = static_cast<std::uint64_t>(i + 1);
          callbacks.transfer.report(0, 0, done, static_cast<std::uint64_t>(count), 0);
        }
        if (!result.ok) {
          if (result.message.empty())
            fail("Operation failed");
          else {
            result.cancelled |= error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            g_clear_error(&error);
          }
        }
        if (isTransfer(request.kind)) {
          if (result.ok) {
            reportTransferred[reportRoot] = true;
            if (request.kind != Kind::Duplicate && !reportEntries[reportRoot] && reportPathSet[reportRoot]) {
              struct stat state{};
              if (lstat(reportPaths[reportRoot].c_str(), &state) == 0)
                reportEntries[reportRoot] = CompletedEntry{
                    reportPaths[reportRoot], static_cast<std::uint64_t>(state.st_dev),
                    static_cast<std::uint64_t>(state.st_ino)
                };
            }
          } else
            reportFailed[reportRoot] = true;
        }
        if (target != nullptr)
          g_object_unref(target);
        g_object_unref(source);
      }
      if (result.message.empty() && request.kind == Kind::Move) {
        // Remove only empty merged source directories. Skipped children remain
        // at the source, and cancellation/failure never triggers recursive cleanup.
        for (std::size_t index = mergedMoveDirectories.size(); index > 0; --index) {
          const auto root = mergedMoveRoots[index - 1];
          GFile* directory = g_file_new_for_path(mergedMoveDirectories[index - 1].c_str());
          GError* cleanupError = nullptr;
          bool removed = false;
          if (const int injectedErrno = fs_compat::invokeRemoveEntryHook(mergedMoveDirectories[index - 1].c_str());
              injectedErrno != 0) {
            g_set_error_literal(
                &cleanupError, G_IO_ERROR, g_io_error_from_errno(injectedErrno), g_strerror(injectedErrno)
            );
          } else {
            removed = g_file_delete(directory, cancellable, &cleanupError);
          }
          result.mutated |= removed;
          if (!removed && !g_error_matches(cleanupError, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) {
            result.ok = false;
            reportFailed[root] = true;
            const std::string& mergedDestination =
                root < reportPaths.size() && !reportPaths[root].empty() ? reportPaths[root] : request.destination;
            const std::string& remainingSource = mergedMoveDirectories[index - 1];
            result.cancelled |=
                cleanupError != nullptr && g_error_matches(cleanupError, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            result.message = "Merged items remain at "
                + mergedDestination
                + "; source directory still exists at "
                + remainingSource
                + ": "
                + (cleanupError == nullptr ? "could not remove source directory" : cleanupError->message);
            result.customDescription = result.message;
          }
          g_clear_error(&cleanupError);
          g_object_unref(directory);
          if (!result.message.empty())
            break;
        }
      } else if (request.kind == Kind::Move && !mergedMoveRoots.empty()) {
        // A later top-level error/cancellation prevents merged source-directory
        // cleanup, so those merge roots are not fully completed move entries.
        for (const auto root : mergedMoveRoots)
          reportFailed[root] = true;
      }
      if (isTransfer(request.kind)) {
        for (std::size_t root = 0; root < reportPaths.size(); ++root) {
          if (!reportTransferred[root] || reportFailed[root])
            continue;
          if (request.kind == Kind::Move && root < request.sources.size())
            result.movedSources.push_back(request.sources[root]);
          if (!reportEntries[root])
            continue;
          struct stat state{};
          const auto& entry = *reportEntries[root];
          if (lstat(entry.path.c_str(), &state) == 0
              && static_cast<std::uint64_t>(state.st_dev) == entry.device
              && static_cast<std::uint64_t>(state.st_ino) == entry.inode)
            result.completedEntries.push_back(entry);
        }
      }
      if (result.message.empty())
        result.ok = true;
    }
    return result;
  }
} // namespace filemanager::file_operation_engine
