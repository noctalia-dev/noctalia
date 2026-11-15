#include "filemanager/file_operation_manager.h"

#include <filesystem>
#include <algorithm>
#include <array>
#include <chrono>
#include <ranges>
#include <utility>

namespace {
  struct Result { bool ok = false; bool cancelled = false; std::string message; };

  struct TransferMeasure { std::uint64_t bytes = 0; std::uint64_t items = 0; };

  bool measureTree(GFile* file, GCancellable* cancellable, TransferMeasure& measure, GError** error) {
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (info == nullptr) return false;
    const auto type = g_file_info_get_file_type(info);
    if (type != G_FILE_TYPE_DIRECTORY) {
      measure.bytes += g_file_info_get_size(info);
      ++measure.items;
      g_object_unref(info);
      return true;
    }
    g_object_unref(info);
    GFileEnumerator* entries = g_file_enumerate_children(file,
        G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (entries == nullptr) return false;
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
      GFile* next = g_file_get_child(file, g_file_info_get_name(child));
      const bool ok = measureTree(next, cancellable, measure, error);
      g_object_unref(next);
      g_object_unref(child);
      if (!ok) { g_object_unref(entries); return false; }
    }
    const bool ok = error == nullptr || *error == nullptr;
    g_object_unref(entries);
    return ok;
  }

  struct TransferProgress {
    FileOperationManager* owner;
    std::uint64_t id;
    std::uint64_t baseBytes;
    std::uint64_t progressedBytes = 0;
    std::uint64_t totalBytes;
    std::uint64_t completedItems;
    std::uint64_t totalItems;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastReport = started;
    std::uint64_t lastBytes = 0;
  };

  void publishProgress(TransferProgress& context, std::uint64_t currentBytes, bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - context.lastReport < std::chrono::milliseconds(100)) return;
    const std::uint64_t total = std::max(context.progressedBytes, context.baseBytes + currentBytes);
    const double seconds = std::chrono::duration<double>(now - context.lastReport).count();
    const std::uint64_t speed = seconds > 0.0 ? static_cast<std::uint64_t>(
        static_cast<double>(total - context.lastBytes) / seconds) : 0;
    context.lastReport = now;
    context.lastBytes = total;
    auto* owner = context.owner;
    g_idle_add([](gpointer data) -> gboolean {
      auto* report = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(data);
      const auto& values = report->second;
      report->first->updateProgress(values[0], values[1], values[2], values[3], values[4], values[5]);
      delete report;
      return G_SOURCE_REMOVE;
    }, new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{owner,
        {context.id, total, context.totalBytes, context.completedItems, context.totalItems, speed}});
    context.progressedBytes = total;
  }

  std::string basename(const std::string& path) { return std::filesystem::path(path).filename().string(); }

  const char* operationName(FileOperationManager::Kind kind) {
    switch (kind) {
    case FileOperationManager::Kind::Copy: return "Copy";
    case FileOperationManager::Kind::Move: return "Move";
    case FileOperationManager::Kind::Rename: return "Rename";
    case FileOperationManager::Kind::CreateFolder:
    case FileOperationManager::Kind::CreateFile: return "Create";
    case FileOperationManager::Kind::Trash: return "Trash";
    case FileOperationManager::Kind::Delete: return "Permanent delete";
    }
    return "File operation";
  }

  std::string quotedBasename(const std::string& path) {
    const std::string name = basename(path);
    return name.empty() ? "item" : "\"" + name + "\"";
  }

  std::string displayDestination(const std::string& path) {
    std::filesystem::path destination(path);
    if (const char* home = g_get_home_dir(); home != nullptr) {
      const std::filesystem::path homePath(home);
      std::error_code ec;
      const auto relative = std::filesystem::relative(destination, homePath, ec);
      if (!ec && !relative.empty() && *relative.begin() != "..") destination = relative;
      else if (!ec && relative == ".") return "Home";
    }
    std::string label = destination.string();
    constexpr std::size_t kMaxDestinationLabel = 72;
    if (label.size() > kMaxDestinationLabel)
      label = "…" + label.substr(label.size() - (kMaxDestinationLabel - 1));
    return label;
  }

  std::string completedDescription(const FileOperationManager::Operation& operation) {
    const auto kind = operation.kind;
    const auto count = kind == FileOperationManager::Kind::Copy || kind == FileOperationManager::Kind::Move
        ? (operation.totalFiles > 0 ? operation.totalFiles : operation.requestedItemCount)
        : operation.requestedItemCount;
    const char* unit = kind == FileOperationManager::Kind::Copy || kind == FileOperationManager::Kind::Move
        ? (operation.totalFiles > 0 ? "file" : "item") : "item";
    const std::string quantity = std::to_string(count) + " " + unit + (count == 1 ? "" : "s");
    switch (kind) {
    case FileOperationManager::Kind::Copy:
      return quantity + " copied to " + displayDestination(operation.destination);
    case FileOperationManager::Kind::Move:
      return quantity + " moved to " + displayDestination(operation.destination);
    case FileOperationManager::Kind::Rename:
      return "Renamed " + quotedBasename(operation.source) + " to " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::CreateFolder:
      return "Created folder " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::CreateFile:
      return "Created file " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::Trash:
      return count == 1 ? "Moved " + quotedBasename(operation.source) + " to Trash"
                        : "Moved " + quantity + " to Trash";
    case FileOperationManager::Kind::Delete:
      return count == 1 ? "Permanently deleted " + quotedBasename(operation.source)
                        : "Permanently deleted " + quantity;
    }
    return "File operation completed";
  }

  std::string failedDescription(const FileOperationManager::Operation& operation, std::string detail) {
    if (detail.find("Cannot copy a directory into itself") != std::string::npos)
      return "Cannot copy " + quotedBasename(operation.source) + " into itself";
    const auto replacePath = [&detail](const std::string& path) {
      if (path.empty() || path.size() < 2) return;
      std::size_t offset = 0;
      const std::string replacement = quotedBasename(path);
      while ((offset = detail.find(path, offset)) != std::string::npos) {
        detail.replace(offset, path.size(), replacement);
        offset += replacement.size();
      }
    };
    replacePath(operation.source);
    replacePath(operation.destination);
    constexpr std::size_t kMaxErrorDescription = 240;
    if (detail.size() > kMaxErrorDescription) detail = detail.substr(0, kMaxErrorDescription - 1) + "…";
    return detail;
  }

  bool isSameOrDescendant(const std::filesystem::path& parent, const std::filesystem::path& child) {
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(parent, ec);
    if (ec) return true;
    auto c = std::filesystem::weakly_canonical(child, ec);
    if (ec) return true;
    auto pi = p.begin();
    auto ci = c.begin();
    for (; pi != p.end(); ++pi, ++ci)
      if (ci == c.end() || *pi != *ci) return false;
    return true;
  }

  bool removeTree(GFile* file, GCancellable* cancellable, GError** error) {
    if (cancellable != nullptr && g_cancellable_set_error_if_cancelled(cancellable, error)) return false;
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (info == nullptr) return false;
    const auto type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if (type == G_FILE_TYPE_DIRECTORY) {
      GFileEnumerator* entries = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (entries == nullptr) return false;
      while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
        const char* name = g_file_info_get_name(child);
        GFile* childFile = g_file_get_child(file, name);
        const bool ok = removeTree(childFile, cancellable, error);
        g_object_unref(childFile);
        g_object_unref(child);
        if (!ok) { g_object_unref(entries); return false; }
      }
      if (error != nullptr && *error != nullptr) { g_object_unref(entries); return false; }
      g_object_unref(entries);
    }
    return g_file_delete(file, cancellable, error);
  }

  bool copyTree(GFile* source, GFile* destination, GCancellable* cancellable, GError** error, TransferProgress* progress) {
    if (cancellable != nullptr && g_cancellable_set_error_if_cancelled(cancellable, error)) return false;
    GFileInfo* info = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (info == nullptr) return false;
    const auto type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if (type != G_FILE_TYPE_DIRECTORY) {
      if (progress != nullptr) progress->baseBytes = progress->progressedBytes;
      const bool copied = g_file_copy(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable,
          progress == nullptr ? nullptr : [](goffset current, goffset, gpointer data) {
            auto* context = static_cast<TransferProgress*>(data);
            publishProgress(*context, static_cast<std::uint64_t>(std::max<goffset>(0, current)));
          }, progress, error);
      return copied;
    }
    if (!g_file_make_directory(destination, cancellable, error)) return false;
    GFileEnumerator* entries = g_file_enumerate_children(source,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (entries == nullptr) return false;
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
      const char* name = g_file_info_get_name(child);
      GFile* from = g_file_get_child(source, name);
      GFile* to = g_file_get_child(destination, name);
      const bool ok = copyTree(from, to, cancellable, error, progress);
      g_object_unref(from); g_object_unref(to); g_object_unref(child);
      if (!ok) { g_object_unref(entries); return false; }
    }
    if (error != nullptr && *error != nullptr) { g_object_unref(entries); return false; }
    g_object_unref(entries);
    return true;
  }
}

struct FileOperationManager::Task {
  FileOperationManager* owner;
  std::uint64_t id;
  Kind kind;
  std::vector<std::string> sources;
  std::string destination;
  std::vector<TransferMeasure> measures;
  TransferMeasure totalMeasure;
  TransferMeasure completedMeasure;
};

FileOperationManager::FileOperationManager(Changed changed) : m_changed(std::move(changed)) {
  GError* error = nullptr;
  m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  g_clear_error(&error);
  if (m_bus != nullptr)
    m_serviceWatch = g_bus_watch_name_on_connection(m_bus, "org.kde.JobViewServer", G_BUS_NAME_WATCHER_FLAGS_NONE,
        serviceAppeared, serviceVanished, this, nullptr);
}
FileOperationManager::~FileOperationManager() {
  for (const auto& [id, timer] : m_jobRevealTimers) { (void)id; g_source_remove(timer); }
  if (m_serviceWatch != 0) g_bus_unwatch_name(m_serviceWatch);
  for (const auto& [id, subscription] : m_signalSubscriptions) { (void)id; g_dbus_connection_signal_unsubscribe(m_bus, subscription); }
  if (m_bus != nullptr) g_object_unref(m_bus);
  for (auto& [id, cancellable] : m_cancellables) { (void)id; g_cancellable_cancel(cancellable); g_object_unref(cancellable); }
}
bool FileOperationManager::active() const noexcept {
  return std::ranges::any_of(m_operations, [](const Operation& op) { return !op.finished; });
}
bool FileOperationManager::usesMount(const std::filesystem::path& mountPath) const {
  if (mountPath.empty()) return active();
  // Test path components, never string prefixes ("/media/a" is not "/media/ab").
  // Keep both lexical and canonical paths so symlink aliases and paths removed
  // during a move remain protected. Ancestors also cover recursive operations
  // whose directory trees contain another mount.
  auto contains = [](const auto& parent, const auto& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for (; p != parent.end(); ++p, ++c)
      if (c == child.end() || *p != *c) return false;
    return true;
  };
  std::error_code error;
  const auto lexical = std::filesystem::absolute(mountPath, error).lexically_normal();
  if (error) return active();
  const auto canonical = std::filesystem::weakly_canonical(lexical, error);
  for (const auto& [id, paths] : m_activePaths) {
    (void)id;
    for (const auto& path : paths) {
      if (path.empty() || contains(lexical, path) || contains(path, lexical)) return true;
      if (!error && (contains(canonical, path) || contains(path, canonical))) return true;
    }
  }
  return false;
}
bool FileOperationManager::hasJobService() const noexcept {
  if (!m_serviceAvailable) return false;
  return std::ranges::all_of(m_operations, [this](const Operation& operation) {
    const bool progressOperation = operation.kind == Kind::Copy || operation.kind == Kind::Move
        || operation.kind == Kind::Trash || operation.kind == Kind::Delete;
    return operation.finished || !progressOperation || !operation.progressRevealed || m_jobPaths.contains(operation.id);
  });
}
FileOperationManager::Operation* FileOperationManager::find(std::uint64_t id) {
  auto it = std::ranges::find(m_operations, id, &Operation::id);
  return it == m_operations.end() ? nullptr : &*it;
}
void FileOperationManager::update(std::uint64_t id, double fraction, std::string status) {
  if (auto* op = find(id)) { op->fraction = fraction; op->status = std::move(status); publishJob(*op); if (m_changed) m_changed(*op); }
}
void FileOperationManager::updateProgress(std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes,
    std::uint64_t files, std::uint64_t totalFiles, std::uint64_t speed) {
  if (auto* op = find(id)) {
    op->processedBytes = bytes;
    op->totalBytes = totalBytes;
    op->processedFiles = files;
    op->totalFiles = totalFiles;
    op->speedBytesPerSecond = speed;
    if (totalBytes > 0) op->fraction = std::clamp(static_cast<double>(bytes) / static_cast<double>(totalBytes), 0.0, 1.0);
    else if (totalFiles > 0) op->fraction = std::clamp(static_cast<double>(files) / static_cast<double>(totalFiles), 0.0, 1.0);
    if (totalBytes > 0) {
      char* processed = g_format_size(bytes);
      char* total = g_format_size(totalBytes);
      char* speedText = g_format_size(speed);
      op->status = std::string(processed) + " / " + total + " · " + std::to_string(files) + " / "
          + std::to_string(totalFiles) + " files" + (speed > 0 ? std::string(" · ") + speedText + "/s" : std::string{});
      g_free(processed); g_free(total); g_free(speedText);
    } else op->status = std::to_string(files) + " / " + std::to_string(totalFiles) + " items";
    publishJob(*op);
    if (m_changed) m_changed(*op);
  }
}
void FileOperationManager::cancel(std::uint64_t id) {
  auto it = std::ranges::find(m_cancellables, id, &std::pair<std::uint64_t, GCancellable*>::first);
  if (it != m_cancellables.end()) g_cancellable_cancel(it->second);
}
std::uint64_t FileOperationManager::copy(std::vector<std::string> sources, std::string destination) {
  return start(Kind::Copy, std::move(sources), std::move(destination));
}
std::uint64_t FileOperationManager::move(std::vector<std::string> sources, std::string destination) {
  return start(Kind::Move, std::move(sources), std::move(destination));
}
std::uint64_t FileOperationManager::rename(std::string source, std::string destination) {
  return start(Kind::Rename, {std::move(source)}, std::move(destination));
}
std::uint64_t FileOperationManager::createFolder(std::string destination) { return start(Kind::CreateFolder, {}, std::move(destination)); }
std::uint64_t FileOperationManager::createFile(std::string destination) { return start(Kind::CreateFile, {}, std::move(destination)); }
std::uint64_t FileOperationManager::trash(std::vector<std::string> sources) { return start(Kind::Trash, std::move(sources), {}); }
std::uint64_t FileOperationManager::removePermanently(std::vector<std::string> sources) { return start(Kind::Delete, std::move(sources), {}); }

std::uint64_t FileOperationManager::start(Kind kind, std::vector<std::string> sources, std::string destination) {
  const auto id = m_nextId++;
  Operation operation{.id=id, .kind=kind, .source=sources.empty() ? std::string{} : sources.front(), .destination=destination,
      .status="Queued", .requestedItemCount=std::max<std::size_t>(1, sources.size())};
  m_operations.push_back(operation);
  auto& activePaths = m_activePaths[id];
  auto trackPath = [&](const std::string& rawPath) {
    if (rawPath.empty()) return;
    std::error_code error;
    const auto lexical = std::filesystem::absolute(rawPath, error).lexically_normal();
    if (error) { activePaths.emplace_back(); return; }
    activePaths.push_back(lexical);
    const auto canonical = std::filesystem::weakly_canonical(lexical, error);
    if (!error && canonical != lexical) activePaths.push_back(canonical);
  };
  for (const auto& source : sources) trackPath(source);
  trackPath(destination);
  const bool reportsProgress = kind == Kind::Copy || kind == Kind::Move || kind == Kind::Trash || kind == Kind::Delete;
  if (reportsProgress) {
    const auto timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 450, [](gpointer data) -> gboolean {
      auto* request = static_cast<std::pair<FileOperationManager*, std::uint64_t>*>(data);
      auto* self = request->first;
      const auto operationId = request->second;
      self->m_jobRevealTimers.erase(operationId);
      if (auto* activeOperation = self->find(operationId); activeOperation != nullptr && !activeOperation->finished) {
        activeOperation->progressRevealed = true;
        self->registerJob(*activeOperation);
        self->publishJob(*activeOperation);
        if (self->m_changed) self->m_changed(*activeOperation);
      }
      delete request;
      return G_SOURCE_REMOVE;
    }, new std::pair<FileOperationManager*, std::uint64_t>{this, id}, nullptr);
    m_jobRevealTimers.emplace(id, timer);
  }
  if (m_changed) m_changed(m_operations.back());
  auto* taskData = new Task{this, id, kind, std::move(sources), std::move(destination)};
  GCancellable* cancellable = g_cancellable_new();
  m_cancellables.emplace_back(id, G_CANCELLABLE(g_object_ref(cancellable)));
  GTask* task = g_task_new(nullptr, cancellable, completed, this);
  // Completion releases mount protection. Cancellation must not dispatch it
  // early while the worker is still unwinding or cleaning staged copies.
  g_task_set_return_on_cancel(task, false);
  g_task_set_task_data(task, taskData, [](gpointer data) { delete static_cast<Task*>(data); });
  g_task_run_in_thread(task, worker);
  g_object_unref(task); g_object_unref(cancellable);
  return id;
}

void FileOperationManager::serviceAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  self->m_serviceAvailable = true;
  for (const auto& operation : self->m_operations)
    if (!operation.finished && (operation.kind == Kind::Copy || operation.kind == Kind::Move || operation.kind == Kind::Trash || operation.kind == Kind::Delete)
        && !self->m_jobRevealTimers.contains(operation.id)) { self->registerJob(operation); self->publishJob(operation); if (self->m_changed) self->m_changed(operation); }
}
void FileOperationManager::serviceVanished(GDBusConnection*, const gchar*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  self->m_serviceAvailable = false;
  for (const auto& [id, subscription] : self->m_signalSubscriptions) {
    (void)id;
    g_dbus_connection_signal_unsubscribe(self->m_bus, subscription);
  }
  self->m_signalSubscriptions.clear();
  self->m_jobPaths.clear();
  for (const auto& operation : self->m_operations)
    if (!operation.finished && self->m_changed) self->m_changed(operation);
}
void FileOperationManager::jobSignal(GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar*, GVariant*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  for (const auto& [id, jobPath] : self->m_jobPaths)
    if (jobPath == path) { self->cancel(id); return; }
}
void FileOperationManager::registerJob(const Operation& operation) {
  if (m_bus == nullptr || !m_serviceAvailable || m_jobPaths.contains(operation.id)) return;
  GVariantBuilder hints;
  g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints, "{sv}", "title", g_variant_new_string("File operation"));
  GError* error = nullptr;
  GVariant* response = g_dbus_connection_call_sync(m_bus, "org.kde.JobViewServer", "/JobViewServer",
      "org.kde.JobViewServerV2", "requestView",
      g_variant_new("(si@a{sv})", "org.nythera.FileManager", 1, g_variant_builder_end(&hints)),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error);
  if (response == nullptr) { g_clear_error(&error); return; }
  const gchar* objectPath = nullptr;
  g_variant_get(response, "(&o)", &objectPath);
  const std::string ownedPath(objectPath);
  g_variant_unref(response);
  m_jobPaths.emplace(operation.id, ownedPath);
  const guint sub = g_dbus_connection_signal_subscribe(m_bus, "org.kde.JobViewServer", "org.kde.JobViewV3",
      "cancelRequested", ownedPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, jobSignal, this, nullptr);
  m_signalSubscriptions.emplace(operation.id, sub);
}
void FileOperationManager::publishJob(const Operation& operation) {
  const auto path = m_jobPaths.find(operation.id);
  if (m_bus == nullptr || !m_serviceAvailable || path == m_jobPaths.end()) return;
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  const char* title = operation.finished ? operation.resultTitle.c_str()
      : operation.kind == Kind::Copy ? "Copying files" : operation.kind == Kind::Move ? "Moving files"
      : operation.kind == Kind::Rename ? "Renaming item" : operation.kind == Kind::Trash ? "Moving to Trash"
      : operation.kind == Kind::Delete ? "Deleting items" : "Creating item";
  g_variant_builder_add(&properties, "{sv}", "title", g_variant_new_string(title));
  g_variant_builder_add(&properties, "{sv}", "infoMessage", g_variant_new_string(operation.finished
      ? operation.resultDescription.c_str() : operation.status.c_str()));
  g_variant_builder_add(&properties, "{sv}", "percent", g_variant_new_uint32(static_cast<guint32>(operation.fraction * 100.0)));
  if (operation.totalBytes > 0) {
    g_variant_builder_add(&properties, "{sv}", "processedBytes", g_variant_new_uint64(operation.processedBytes));
    g_variant_builder_add(&properties, "{sv}", "totalBytes", g_variant_new_uint64(operation.totalBytes));
    g_variant_builder_add(&properties, "{sv}", "speed", g_variant_new_uint64(operation.speedBytesPerSecond));
  }
  if (operation.totalFiles > 0) {
    g_variant_builder_add(&properties, "{sv}", "processedFiles", g_variant_new_uint64(operation.processedFiles));
    g_variant_builder_add(&properties, "{sv}", "totalFiles", g_variant_new_uint64(operation.totalFiles));
    g_variant_builder_add(&properties, "{sv}", "processedItems", g_variant_new_uint64(operation.processedFiles));
    g_variant_builder_add(&properties, "{sv}", "totalItems", g_variant_new_uint64(operation.totalFiles));
  }
  if (!operation.destination.empty()) {
    char* uri = g_filename_to_uri(operation.destination.c_str(), nullptr, nullptr);
    if (uri != nullptr) { g_variant_builder_add(&properties, "{sv}", "destUrl", g_variant_new_string(uri)); g_free(uri); }
  }
  g_dbus_connection_call(m_bus, "org.kde.JobViewServer", path->second.c_str(),
      "org.kde.JobViewV3", "update", g_variant_new("(@a{sv})", g_variant_builder_end(&properties)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer) {
        GError* callError = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &callError);
        if (reply != nullptr) g_variant_unref(reply);
        g_clear_error(&callError);
      }, nullptr);
}
void FileOperationManager::finishJob(const Operation& operation) {
  if (const auto timer = m_jobRevealTimers.find(operation.id); timer != m_jobRevealTimers.end()) {
    g_source_remove(timer->second);
    m_jobRevealTimers.erase(timer);
  }
  const auto path = m_jobPaths.find(operation.id);
  if (m_bus != nullptr && m_serviceAvailable && path != m_jobPaths.end()) {
    const guint subscription = m_signalSubscriptions[operation.id];
    if (subscription != 0) g_dbus_connection_signal_unsubscribe(m_bus, subscription);
    m_signalSubscriptions.erase(operation.id);
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(m_bus, "org.kde.JobViewServer", path->second.c_str(),
        "org.kde.JobViewV3", "terminate",
        g_variant_new("(us@a{sv})", operation.succeeded ? 0U : operation.cancelled ? 1U : 2U,
            operation.status.c_str(), g_variant_builder_end(&hints)),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error);
    if (reply != nullptr) g_variant_unref(reply);
    g_clear_error(&error);
    m_jobPaths.erase(path);
  }
}

void FileOperationManager::worker(GTask* task, gpointer, gpointer taskData, GCancellable* cancellable) {
  auto* data = static_cast<Task*>(taskData);
  auto* result = new Result{};
  GError* error = nullptr;
  auto fail = [&](const char* fallback) {
    result->ok = false;
    result->message = error == nullptr ? fallback : error->message;
    result->cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
  };
  if (data->kind == Kind::CreateFolder || data->kind == Kind::CreateFile) {
    GFile* target = g_file_new_for_path(data->destination.c_str());
    if (data->kind == Kind::CreateFolder) result->ok = g_file_make_directory(target, cancellable, &error);
    else {
      GFileOutputStream* stream = g_file_create(target, G_FILE_CREATE_NONE, cancellable, &error);
      if (stream != nullptr) { result->ok = g_output_stream_close(G_OUTPUT_STREAM(stream), cancellable, &error); g_object_unref(stream); }
    }
    g_object_unref(target);
    if (!result->ok) fail("Could not create item");
  } else {
    auto count = data->sources.size();
    std::vector<std::string> destinationParents(count, data->destination);
    std::vector<std::string> mergedMoveDirectories;
    std::optional<ConflictAction> applyConflictToAll;
    std::optional<GFileType> applyConflictItemType;
    std::optional<GFileType> applyConflictDestinationType;
    if (data->kind == Kind::Copy || data->kind == Kind::Move) {
      data->measures.resize(count);
      for (std::size_t i = 0; i < count; ++i) {
        GFile* source = g_file_new_for_path(data->sources[i].c_str());
        const bool measured = measureTree(source, cancellable, data->measures[i], &error);
        g_object_unref(source);
        if (!measured) { fail("Could not calculate transfer size"); break; }
        data->totalMeasure.bytes += data->measures[i].bytes;
        data->totalMeasure.items += data->measures[i].items;
      }
      if (result->message.empty()) {
        auto* report = new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{data->owner,
            {data->id, 0, data->totalMeasure.bytes, 0, data->totalMeasure.items, 0}};
        g_idle_add([](gpointer value) -> gboolean {
          auto* event = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(value);
          auto& v = event->second;
          event->first->updateProgress(v[0], v[1], v[2], v[3], v[4], v[5]);
          delete event; return G_SOURCE_REMOVE;
        }, report);
      }
    }
    std::uint64_t completedBytes = 0;
    std::uint64_t completedItems = 0;
    TransferProgress progress{.owner=data->owner, .id=data->id, .baseBytes=0, .progressedBytes=0,
        .totalBytes=data->totalMeasure.bytes, .completedItems=0, .totalItems=data->totalMeasure.items};
    for (std::size_t i = 0; i < count && result->message.empty(); ++i) {
      if (g_cancellable_set_error_if_cancelled(cancellable, &error)) { fail("Cancelled"); break; }
      GFile* source = g_file_new_for_path(data->sources[i].c_str());
      GFile* target = nullptr;
      if (data->kind == Kind::Copy || data->kind == Kind::Move)
      {
        GFile* parent = g_file_new_for_path(destinationParents[i].c_str());
        target = g_file_get_child(parent, basename(data->sources[i]).c_str());
        g_object_unref(parent);
      }
      else if (data->kind == Kind::Rename) target = g_file_new_for_path(data->destination.c_str());
      if ((data->kind == Kind::Copy || data->kind == Kind::Move) && target != nullptr) {
        const bool sameItem = g_file_equal(source, target);
        if (sameItem && data->kind == Kind::Move) result->message = "Cannot move an item onto itself";
        GFileInfo* sourceInfo = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
        if (sourceInfo == nullptr) { fail("Could not inspect source"); }
        else {
          const bool directory = g_file_info_get_file_type(sourceInfo) == G_FILE_TYPE_DIRECTORY;
          g_object_unref(sourceInfo);
          char* targetPath = g_file_get_path(target);
          if (directory && !sameItem && targetPath != nullptr && isSameOrDescendant(data->sources[i], targetPath))
            result->message = "Cannot copy a directory into itself or one of its descendants";
          g_free(targetPath);
        }
      }
      if (!result->message.empty()) {
        result->ok = false;
        if (target != nullptr) g_object_unref(target);
        g_object_unref(source);
        break;
      }
      bool replaceExisting = false;
      bool skipped = false;
      bool merge = false;
      if ((data->kind == Kind::Copy || data->kind == Kind::Move || data->kind == Kind::Rename)
          && target != nullptr && g_file_query_exists(target, cancellable)) {
        const bool sameItem = data->kind != Kind::Rename && g_file_equal(source, target);
        GFileInfo* incomingInfo = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr);
        const auto incomingType = incomingInfo == nullptr ? G_FILE_TYPE_UNKNOWN : g_file_info_get_file_type(incomingInfo);
        if (incomingInfo != nullptr) g_object_unref(incomingInfo);
        const auto existingType = g_file_query_file_type(target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
        bool compatibleBatchConflict = true;
        if (applyConflictToAll) {
          GFileInfo* existingInfo = g_file_query_info(target, G_FILE_ATTRIBUTE_STANDARD_TYPE,
              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr);
          compatibleBatchConflict = !sameItem && incomingType != G_FILE_TYPE_UNKNOWN && existingInfo != nullptr
              && incomingType == g_file_info_get_file_type(existingInfo) && applyConflictItemType == incomingType && applyConflictDestinationType == existingType;
          if (existingInfo != nullptr) g_object_unref(existingInfo);
        }
        ConflictDecision decision{.action=ConflictAction::Cancel};
        if (applyConflictToAll && compatibleBatchConflict) decision.action = *applyConflictToAll;
        else if (data->owner->m_conflictResolver) {
          char* destinationPath = g_file_get_path(target);
          decision = data->owner->m_conflictResolver(data->sources[i],
              destinationPath == nullptr ? data->destination : destinationPath, i + 1 < count || incomingType == G_FILE_TYPE_DIRECTORY, cancellable);
          g_free(destinationPath);
        }
        if (decision.applyToAll) {
          applyConflictToAll = decision.action;
          applyConflictItemType = incomingType;
          applyConflictDestinationType = existingType;
        }
        if (decision.action == ConflictAction::Skip) skipped = true;
        else if (decision.action == ConflictAction::Merge && !sameItem
            && incomingType == G_FILE_TYPE_DIRECTORY && existingType == G_FILE_TYPE_DIRECTORY
            && (data->kind == Kind::Copy || data->kind == Kind::Move)) merge = true;
        else if (decision.action == ConflictAction::Replace && !sameItem
            && incomingType == existingType && incomingType != G_FILE_TYPE_DIRECTORY
            && incomingType != G_FILE_TYPE_UNKNOWN) replaceExisting = true;
        else if (decision.action == ConflictAction::Rename) {
          GFile* parent = g_file_get_parent(target);
          const auto original = std::filesystem::path(data->sources[i]).filename();
          const auto stem = incomingType == G_FILE_TYPE_DIRECTORY ? original.string() : original.stem().string();
          const auto extension = incomingType == G_FILE_TYPE_DIRECTORY ? std::string{} : original.extension().string();
          std::string candidate;
          if (!decision.alternativeName.empty()) {
            const auto& name = decision.alternativeName;
            if (name == "." || name == ".." || name.find('/') != std::string::npos) {
              result->message = "The alternative name must be a single valid filename";
            } else {
              candidate = name;
              GFile* next = g_file_get_child(parent, candidate.c_str());
              if (g_file_query_exists(next, cancellable)) {
                result->message = "The alternative name already exists in the destination";
                g_object_unref(next);
              } else {
                g_object_unref(target);
                target = next;
              }
            }
          } else {
            for (unsigned suffix = 1; suffix < 10000; ++suffix) {
              candidate = stem + " (copy" + (suffix == 1 ? std::string{} : " " + std::to_string(suffix)) + ")" + extension;
              GFile* next = g_file_get_child(parent, candidate.c_str());
              const bool exists = g_file_query_exists(next, cancellable);
              if (!exists) { g_object_unref(target); target = next; break; }
              g_object_unref(next);
            }
            if (target == nullptr || g_file_query_exists(target, cancellable))
              result->message = "Could not find a free name for the incoming item";
          }
          g_object_unref(parent);
        } else {
          result->message = "Operation cancelled";
          result->cancelled = true;
        }
      }
      if (!result->message.empty()) {
        result->ok = false;
        if (target != nullptr) g_object_unref(target);
        g_object_unref(source);
        break;
      }
      if (merge) {
        // Expand only a pair explicitly approved for merging. Every child still
        // passes through the normal conflict and staged-finalisation path.
        GFileEnumerator* entries = g_file_enumerate_children(source, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
        if (entries == nullptr) fail("Could not enumerate directory for merging");
        else {
          char* destinationPath = g_file_get_path(target);
          while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, &error)) {
            GFile* childFile = g_file_get_child(source, g_file_info_get_name(child));
            char* childPath = g_file_get_path(childFile);
            TransferMeasure measure;
            const bool measured = childPath != nullptr && measureTree(childFile, cancellable, measure, &error);
            if (measured) {
              data->sources.emplace_back(childPath);
              destinationParents.emplace_back(destinationPath);
              data->measures.push_back(measure);
            }
            g_free(childPath);
            g_object_unref(childFile);
            g_object_unref(child);
            if (!measured) break;
          }
          g_free(destinationPath);
          g_object_unref(entries);
          if (error != nullptr) fail("Could not enumerate directory for merging");
          else if (data->kind == Kind::Move) mergedMoveDirectories.push_back(data->sources[i]);
          count = data->sources.size();
        }
        g_object_unref(target);
        g_object_unref(source);
        continue;
      }
      if (skipped) {
        result->ok = true;
        if (data->kind == Kind::Copy || data->kind == Kind::Move) {
          data->totalMeasure.bytes -= data->measures[i].bytes;
          data->totalMeasure.items -= data->measures[i].items;
          progress.totalBytes = data->totalMeasure.bytes;
          progress.totalItems = data->totalMeasure.items;
          progress.baseBytes = completedBytes;
          progress.progressedBytes = completedBytes;
          progress.completedItems = completedItems;
          publishProgress(progress, 0, true);
        }
        if (target != nullptr) g_object_unref(target);
        g_object_unref(source);
        continue;
      }
      if (data->kind == Kind::Trash) result->ok = g_file_trash(source, cancellable, &error);
      else if (data->kind == Kind::Delete) result->ok = removeTree(source, cancellable, &error);
      else if (data->kind == Kind::Rename) {
        GFile* backup = nullptr;
        if (replaceExisting) {
          char* backupName = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
          GFile* parent = g_file_get_parent(target);
          backup = g_file_get_child(parent, backupName);
          g_object_unref(parent);
          g_free(backupName);
          result->ok = g_file_move(target, backup, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        } else result->ok = true;
        if (result->ok) result->ok = g_file_move(source, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        if (!result->ok && backup != nullptr) {
          GError* restoreError = nullptr;
          g_file_move(backup, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &restoreError);
          g_clear_error(&restoreError);
        } else if (result->ok && backup != nullptr) {
          GError* cleanupError = nullptr;
          removeTree(backup, nullptr, &cleanupError);
          g_clear_error(&cleanupError);
        }
        if (backup != nullptr) g_object_unref(backup);
      }
      else {
        // Stage beside the destination so finalisation is a single no-overwrite rename.
        char* uuid = g_uuid_string_random();
        char* leaf = g_strdup_printf(".nythera-partial-%s-%zu", uuid, i);
        g_free(uuid);
        GFile* targetParent = g_file_get_parent(target);
        GFile* staged = g_file_get_child(targetParent, leaf);
        g_object_unref(targetParent);
        g_free(leaf);
        const bool stagedWasAbsent = !g_file_query_exists(staged, cancellable);
        progress.baseBytes = completedBytes;
        progress.progressedBytes = completedBytes;
        progress.completedItems = completedItems;
        if (stagedWasAbsent) result->ok = copyTree(source, staged, cancellable, &error, &progress);
        else {
          g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary destination");
          result->ok = false;
        }
        GFile* backup = nullptr;
        if (result->ok && replaceExisting) {
          char* backupName = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
          GFile* parent = g_file_get_parent(target);
          backup = g_file_get_child(parent, backupName);
          g_object_unref(parent);
          g_free(backupName);
          result->ok = g_file_move(target, backup, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        }
        if (result->ok) result->ok = g_file_move(staged, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        if (!result->ok && backup != nullptr) {
          GError* restoreError = nullptr;
          g_file_move(backup, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &restoreError);
          g_clear_error(&restoreError);
        } else if (result->ok && backup != nullptr) {
          GError* cleanupError = nullptr;
          removeTree(backup, nullptr, &cleanupError);
          g_clear_error(&cleanupError);
        }
        if (backup != nullptr) g_object_unref(backup);
        if (!result->ok && stagedWasAbsent && g_file_query_exists(staged, nullptr)) {
          GError* cleanupError = nullptr; removeTree(staged, nullptr, &cleanupError); g_clear_error(&cleanupError);
        }
        if (result->ok && data->kind == Kind::Move) {
          if (g_cancellable_set_error_if_cancelled(cancellable, &error)) result->ok = false;
          else result->ok = removeTree(source, nullptr, &error);
        }
        if (result->ok) {
          completedBytes += data->measures[i].bytes;
          completedItems += data->measures[i].items;
          data->completedMeasure.bytes = completedBytes;
          data->completedMeasure.items = completedItems;
          progress.baseBytes = completedBytes;
          progress.progressedBytes = completedBytes;
          progress.completedItems = completedItems;
          publishProgress(progress, 0, true);
        }
        g_object_unref(staged);
      }
      if ((data->kind == Kind::Trash || data->kind == Kind::Delete) && result->ok) {
        const auto done = static_cast<std::uint64_t>(i + 1);
        auto* event = new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{data->owner,
            {data->id, 0, 0, done, static_cast<std::uint64_t>(count), 0}};
        g_idle_add([](gpointer value) -> gboolean {
          auto* report = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(value);
          auto& v = report->second;
          report->first->updateProgress(v[0], v[1], v[2], v[3], v[4], v[5]);
          delete report; return G_SOURCE_REMOVE;
        }, event);
      }
      if (!result->ok) fail("Operation failed");
      if (target != nullptr) g_object_unref(target);
      g_object_unref(source);
    }
    if (result->message.empty() && data->kind == Kind::Move) {
      // Remove only empty merged source directories. Skipped children remain
      // at the source, and cancellation/failure never triggers recursive cleanup.
      for (auto it = mergedMoveDirectories.rbegin(); it != mergedMoveDirectories.rend(); ++it) {
        GFile* directory = g_file_new_for_path(it->c_str());
        GError* cleanupError = nullptr;
        if (!g_file_delete(directory, cancellable, &cleanupError)
            && !g_error_matches(cleanupError, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) {
          error = cleanupError;
          cleanupError = nullptr;
          fail("Could not remove merged source directory");
        }
        g_clear_error(&cleanupError);
        g_object_unref(directory);
        if (!result->message.empty()) break;
      }
    }
    if (result->message.empty()) result->ok = true;
  }
  g_task_return_pointer(task, result, [](gpointer ptr) { delete static_cast<Result*>(ptr); });
}

void FileOperationManager::completed(GObject*, GAsyncResult* asyncResult, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  GError* error = nullptr;
  std::unique_ptr<Result> result(static_cast<Result*>(g_task_propagate_pointer(G_TASK(asyncResult), &error)));
  auto* data = static_cast<Task*>(g_task_get_task_data(G_TASK(asyncResult)));
  if (data != nullptr) {
    auto* operation = self->find(data->id);
    if (operation != nullptr) {
      operation->finished = true;
      self->m_activePaths.erase(data->id);
      operation->succeeded = result && result->ok;
      operation->cancelled = result ? result->cancelled : error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      if (data->kind == Kind::Copy || data->kind == Kind::Move) {
        operation->processedBytes = data->completedMeasure.bytes;
        operation->totalBytes = data->totalMeasure.bytes;
        operation->processedFiles = data->completedMeasure.items;
        operation->totalFiles = data->totalMeasure.items;
        if (operation->totalBytes > 0)
          operation->fraction = std::clamp(static_cast<double>(operation->processedBytes) / static_cast<double>(operation->totalBytes), 0.0, 1.0);
      }
      operation->fraction = operation->succeeded ? 1.0 : operation->fraction;
      const std::string verb = operationName(data->kind);
      operation->resultTitle = verb + (operation->succeeded ? " completed" : operation->cancelled ? " cancelled" : " failed");
      if (operation->succeeded) operation->resultDescription = completedDescription(*operation);
      else if (operation->cancelled) {
        if (data->kind == Kind::Copy || data->kind == Kind::Move)
          operation->resultDescription = "Transfer to " + displayDestination(operation->destination) + " was cancelled";
        else operation->resultDescription = verb + " of " + quotedBasename(operation->source.empty() ? operation->destination : operation->source) + " was cancelled";
      } else {
        operation->resultDescription = failedDescription(*operation,
            result ? result->message : error ? error->message : "Operation failed");
      }
      operation->status = operation->resultDescription;
      if (!operation->succeeded && !operation->cancelled && !self->m_jobPaths.contains(operation->id))
        self->registerJob(*operation);
      self->publishJob(*operation);
      self->finishJob(*operation);
      if (self->m_changed) self->m_changed(*operation);
      auto it = std::ranges::find(self->m_cancellables, data->id, &std::pair<std::uint64_t, GCancellable*>::first);
      if (it != self->m_cancellables.end()) { g_object_unref(it->second); self->m_cancellables.erase(it); }
    }
  }
  g_clear_error(&error);
}
