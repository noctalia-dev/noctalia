#include "filemanager/properties_window.h"

#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/places_model.h"
#include "filemanager/property_metadata.h"
#include "render/scene/input_area.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/controls/separator.h"
#include "ui/controls/spinner.h"
#include "ui/dialogs/file_icon.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <gio/gunixmounts.h>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <locale>
#include <sstream>
#include <sys/statvfs.h>
#include <xkbcommon/xkbcommon-keysyms.h>

// Properties can defer a compositor close request while confirming staged edits.
// Keep this policy local rather than changing other native window hosts.
class PropertiesSurface final : public ToplevelSurface {
public:
  using ToplevelSurface::ToplevelSurface;
  void keepOpen() { setRunning(true); }
};

namespace {
  struct QueryData {
    PropertiesWindow* window;
    std::weak_ptr<bool> alive;
    std::uint64_t generation;
    std::filesystem::path path;
  };

  std::string sizeText(std::uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit < std::size(units) - 1) {
      value /= 1024.0;
      ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
  }

  std::string groupedNumber(std::uint64_t value) {
    std::ostringstream out;
    try {
      out.imbue(std::locale(""));
    } catch (const std::runtime_error&) {
      // Keep a usable number if the active locale is unavailable.
    }
    out << value;
    return out.str();
  }

  std::string timeText(guint64 seconds) {
    const auto value = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_r(&value, &local) == nullptr)
      return {};
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M");
    return out.str();
  }

  std::string specialPermissionsText(std::uint32_t mode) {
    std::string bits;
    if (mode & 04000)
      bits += "Setuid  ";
    if (mode & 02000)
      bits += "Setgid  ";
    if (mode & 01000)
      bits += "Sticky";
    return bits.empty() ? "None" : bits;
  }

  GFile* fileForPath(const std::filesystem::path& path) {
    const auto text = path.string();
    return text.starts_with("trash:/") ? g_file_new_for_uri(text.c_str()) : g_file_new_for_path(text.c_str());
  }
} // namespace

PropertiesWindow::PropertiesWindow(
    WaylandConnection& wayland, RenderContext& renderer, ConfigService& config, FileOperationManager& operations,
    std::function<void(std::string)> copyText
)
    : m_wayland(wayland), m_renderer(renderer), m_config(config), m_operations(operations),
      m_copyText(std::move(copyText)) {}

PropertiesWindow::~PropertiesWindow() {
  *m_alive = false;
  close();
}

wl_surface* PropertiesWindow::wlSurface() const noexcept { return m_surface ? m_surface->wlSurface() : nullptr; }

bool PropertiesWindow::ownsSurface(wl_surface* surface) const noexcept {
  return surface == wlSurface() || (m_dropdown && m_dropdown->isSelectDropdownOpen());
}

bool PropertiesWindow::open(const std::filesystem::path& path, ToplevelSurface* parent) {
  return openTarget(path, parent, nullptr);
}

bool PropertiesWindow::openVolume(const filemanager::Place& place, ToplevelSurface* parent) {
  if (place.mounted)
    return !place.path.empty() && open(place.path, parent);
  return openTarget({}, parent, &place);
}

bool PropertiesWindow::openTarget(
    const std::filesystem::path& path, ToplevelSurface* parent, const filemanager::Place* volume
) {
  if (m_surface) {
    m_wayland.activateSurface(m_surface->wlSurface());
    return true;
  }
  m_volumeOnly = volume != nullptr;
  m_path = path;
  m_nameDraft = path.filename().string();
  m_itemAvailable = false;
  m_mountTarget = false;
  m_renaming = false;
  m_continueAfterRename = false;
  m_tooltipOwned = false;
  m_applySummary.clear();
  const auto generation = ++m_generation;
  m_tab = 0;
  m_renderedTab = -1;
  m_contentDirty = true;
  m_general.clear();
  m_permissions.clear();
  m_details.clear();
  m_filesystem = {"Filesystem", {}};
  m_filesystemId.clear();
  m_loading = true;
  m_directory = false;
  m_preferredWidth = 500;
  m_preferredHeight = 620;
  m_permissionEditable = false;
  m_renameError.clear();
  m_applicationError.clear();
  m_permissionError.clear();
  m_mime.clear();
  m_pendingOwner.clear();
  m_pendingGroup.clear();
  m_pendingApplication.reset();
  m_defaultApplication.reset();
  m_applications.clear();
  m_accounts = {};
  m_recursive = false;
  m_retainPending = false;
  m_expandedMetadata.clear();
  m_refreshing = false;
  m_choicesLoading = false;
  m_operation = 0;
  m_modeAvailable = false;
  m_applying = false;
  m_discardRequested = false;
  m_originalMode = m_editedMode = 0;
  m_permissionMessage.clear();
  m_capacity = -1.0F;
  m_iconNames = {"text-x-generic"};
  m_displayName = path.filename().empty() ? path.string() : path.filename().string();
  if (const auto saved = m_config.stateString("files", "properties_window_size")) {
    const auto separator = saved->find('x');
    if (separator != std::string::npos) {
      std::uint32_t width = 0, height = 0;
      const auto w = std::from_chars(saved->data(), saved->data() + separator, width);
      const auto h = std::from_chars(saved->data() + separator + 1, saved->data() + saved->size(), height);
      if (w.ec == std::errc{}
          && h.ec == std::errc{}
          && w.ptr == saved->data() + separator
          && h.ptr == saved->data() + saved->size()
          && width >= 440
          && height >= 400) {
        m_preferredWidth = width;
        m_preferredHeight = height;
      }
    }
  }
  if (volume) {
    m_loading = false;
    m_displayName = volume->label;
    m_iconNames = volume->iconNames;
    Section identity{"Identity", {{"Name", volume->label}, {"State", "Unmounted"}}};
    if (!volume->details.empty())
      identity.rows.emplace_back("Description", volume->details);
    m_general.push_back(std::move(identity));
    Section metadata{"Volume", {}};
    if (!volume->filesystemType.empty())
      metadata.rows.emplace_back("Filesystem type", volume->filesystemType);
    if (!volume->mountSource.empty())
      metadata.rows.emplace_back("Source", volume->mountSource);
    if (volume->volume) {
      for (const auto& [label, kind] :
           {std::pair{"UUID", G_VOLUME_IDENTIFIER_KIND_UUID},
            std::pair{"Device", G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE}}) {
        gchar* value = g_volume_get_identifier(volume->volume.get(), kind);
        if (value && *value)
          metadata.rows.emplace_back(label, value);
        g_free(value);
      }
    }
    if (!metadata.rows.empty())
      m_details.push_back(std::move(metadata));
  }
  m_surface = std::make_unique<PropertiesSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderer);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_surface->setClosedCallback([this, generation] {
    // xdg close is a request. Keep the surface drawable for discard confirmation
    // or while the already-authorised asynchronous Apply finishes.
    m_surface->keepOpen();
    auto* pending = new QueryData{this, m_alive, generation};
    g_idle_add(
        [](gpointer data) -> gboolean {
          std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
          const auto alive = request->alive.lock();
          if (alive
              && *alive
              && request->generation == request->window->m_generation
              && (request->path.empty() || request->path == request->window->m_path)) {
            // The compositor callback runs inside the surface event handler; defer its destruction.
            request->window->deferClose();
          }
          return G_SOURCE_REMOVE;
        },
        pending
    );
  });
  const auto parentOutput = parent ? m_wayland.outputForSurface(parent->wlSurface()) : nullptr;
  const auto foundOutput = std::ranges::find(m_wayland.outputs(), parentOutput, &WaylandOutput::output);
  const auto* outputInfo = foundOutput != m_wayland.outputs().end() ? &*foundOutput
      : m_wayland.outputs().empty()                                 ? nullptr
                                                                    : &m_wayland.outputs().front();
  wl_output* output = outputInfo ? outputInfo->output : nullptr;
  const auto outputWidth = outputInfo && outputInfo->logicalWidth > 0 ? outputInfo->logicalWidth : 500;
  const auto outputHeight = outputInfo && outputInfo->logicalHeight > 0 ? outputInfo->logicalHeight : 620;
  const auto minWidth = static_cast<std::uint32_t>(std::min(440, outputWidth));
  const auto minHeight = static_cast<std::uint32_t>(std::min(400, outputHeight));
  ToplevelSurfaceConfig config{
      .width = std::clamp(m_preferredWidth, minWidth, static_cast<std::uint32_t>(outputWidth)),
      .height = std::clamp(m_preferredHeight, minHeight, static_cast<std::uint32_t>(outputHeight)),
      .minWidth = minWidth,
      .minHeight = minHeight,
      .parent = parent ? parent->xdgToplevel() : nullptr,
      .title = "Properties — " + (volume ? volume->label : path.filename().string()),
      .appId = "dev.nythera.FileManager.Properties"
  };
  if (!m_surface->initialize(output, config)) {
    m_surface.reset();
    return false;
  }
  if (!volume)
    refreshMetadata();
  return true;
}

void PropertiesWindow::cancelFolderSize() {
  ++m_folderRevision;
  if (m_folderCancel) {
    g_cancellable_cancel(m_folderCancel);
    g_clear_object(&m_folderCancel);
  }
  m_folderScanning = false;
  m_folderComplete = false;
  m_folderRequest = {};
  m_folderResult = {};
}

void PropertiesWindow::updateFolderSizeRows() {
  auto size = std::ranges::find(m_general, "Size and storage", &Section::title);
  if (size == m_general.end() || !m_directory)
    return;
  std::erase_if(size->rows, [](const auto& row) {
    return row.first == "Folder size" || row.first == "Contents" || row.first == "Calculation" || row.first == "Status";
  });
  std::vector<std::pair<std::string, std::string>> rows;
  if (m_folderScanning || !m_folderComplete)
    rows.emplace_back("Folder size", "Calculating…");
  else {
    auto text = sizeText(m_folderResult.bytes);
    if (m_folderResult.bytes >= 1024)
      text += " (" + groupedNumber(m_folderResult.bytes) + " bytes)";
    rows.emplace_back(
        "Folder size",
        (m_folderResult.failed          ? "Failed (partial): "
             : m_folderResult.cancelled ? "Cancelled (partial): "
             : m_folderResult.partial() ? "Partial: "
                                        : "")
            + text
    );
    std::string counts =
        groupedNumber(m_folderResult.files) + " files · " + groupedNumber(m_folderResult.folders) + " folders";
    if (m_folderResult.links)
      counts += " · " + groupedNumber(m_folderResult.links) + " links";
    if (m_folderResult.other)
      counts += " · " + groupedNumber(m_folderResult.other) + " other entries";
    rows.emplace_back("Contents", std::move(counts));
    std::string note;
    if (m_folderResult.excludedMounts)
      note = groupedNumber(m_folderResult.excludedMounts) + " child mounts excluded. ";
    if (m_folderResult.errors
        && !m_folderResult.permissionErrors
        && !m_folderResult.vanishedEntries
        && !m_folderResult.ioErrors
        && !m_folderResult.unreadableDirectories)
      note += groupedNumber(m_folderResult.errors) + " entries could not be fully counted. ";
    if (m_folderResult.permissionErrors)
      note += groupedNumber(m_folderResult.permissionErrors) + " permission failures. ";
    if (m_folderResult.unreadableDirectories)
      note +=
          groupedNumber(m_folderResult.unreadableDirectories) + " subtrees incomplete; descendants may be omitted. ";
    if (m_folderResult.vanishedEntries)
      note += groupedNumber(m_folderResult.vanishedEntries) + " missing or stale entries. ";
    if (m_folderResult.ioErrors)
      note += groupedNumber(m_folderResult.ioErrors) + " read errors. ";
    if (m_folderResult.cancelled)
      note += "Calculation cancelled. ";
    if (!note.empty())
      rows.emplace_back("Status", std::move(note));
  }
  size->rows.insert(size->rows.begin(), rows.begin(), rows.end());
  m_contentDirty = true;
  if (m_surface)
    m_surface->requestLayout();
}

void PropertiesWindow::ensureFolderSize() {
  const filemanager::FolderSizeRequest target{m_path, m_device, m_inode};
  if (target == m_folderRequest && (m_folderScanning || m_folderComplete)) {
    updateFolderSizeRows();
    return;
  }
  cancelFolderSize();
  m_folderRequest = target;
  m_folderCancel = g_cancellable_new();
  m_folderScanning = true;
  updateFolderSizeRows();
  struct Completion {
    PropertiesWindow* window;
    std::weak_ptr<bool> alive;
    std::uint64_t generation;
    std::uint64_t revision;
    filemanager::FolderSizeRequest target;
  };
  struct Work {
    filemanager::FolderSizeRequest target;
    GCancellable* cancel;
  };
  auto* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Completion> request(static_cast<Completion*>(data));
        std::unique_ptr<filemanager::FolderSizeResult> size(
            static_cast<filemanager::FolderSizeResult*>(g_task_propagate_pointer(G_TASK(result), nullptr))
        );
        const auto alive = request->alive.lock();
        if (!alive || !*alive)
          return;
        auto* window = request->window;
        if (request->generation != window->m_generation
            || request->revision != window->m_folderRevision
            || request->target != window->m_folderRequest
            || request->target.path != window->m_path)
          return;
        window->m_folderResult = std::move(*size);
        window->m_folderScanning = false;
        window->m_folderComplete = true;
        g_clear_object(&window->m_folderCancel);
        window->updateFolderSizeRows();
      },
      new Completion{this, m_alive, m_generation, m_folderRevision, target}
  );
  g_task_set_task_data(task, new Work{target, G_CANCELLABLE(g_object_ref(m_folderCancel))}, [](gpointer data) {
    auto* work = static_cast<Work*>(data);
    g_object_unref(work->cancel);
    delete work;
  });
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    auto* work = static_cast<Work*>(data);
    auto* result = new filemanager::FolderSizeResult(filemanager::calculateFolderSize(work->target, [work] {
      return g_cancellable_is_cancelled(work->cancel);
    }));
    g_task_return_pointer(worker, result, [](gpointer value) {
      delete static_cast<filemanager::FolderSizeResult*>(value);
    });
  });
  g_object_unref(task);
}

void PropertiesWindow::refreshMetadata() {
  if (m_volumeOnly)
    return;
  auto* file = fileForPath(m_path);
  auto* queryData = new QueryData{this, m_alive, m_generation, m_path};
  g_file_query_info_async(
      file,
      "standard::name,standard::display-name,standard::type,standard::is-symlink,standard::symlink-target,standard::"
      "icon,standard::"
      "content-type,standard::size,standard::is-hidden,standard::is-backup,unix::mode,unix::uid,unix::gid,unix::inode,"
      "unix::blocks,unix::device,unix::nlink,"
      "owner::user,owner::group,id::filesystem,selinux::context,unix::block-size,time::modified,time::access,time::"
      "created",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        GError* error = nullptr;
        GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
        const auto alive = request->alive.lock();
        if (alive
            && *alive
            && request->generation == request->window->m_generation
            && (request->path.empty() || request->path == request->window->m_path))
          request->window->queryReady(info, error ? error->message : std::string{});
        if (info)
          g_object_unref(info);
        if (error)
          g_error_free(error);
      },
      queryData
  );
  g_object_unref(file);
}

void PropertiesWindow::loadChoices() {
  if (m_choicesLoading)
    return;
  m_choicesLoading = true;
  struct Choices {
    filemanager::PropertyAccounts accounts;
    std::string mime;
    std::vector<std::shared_ptr<GAppInfo>> apps;
    std::shared_ptr<GAppInfo> current;
  };
  auto* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        std::unique_ptr<Choices> choices(static_cast<Choices*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
        const auto alive = request->alive.lock();
        if (!alive
            || !*alive
            || request->generation != request->window->m_generation
            || (!request->path.empty() && request->path != request->window->m_path))
          return;
        auto* window = request->window;
        window->m_choicesLoading = false;
        window->m_accounts = std::move(choices->accounts);
        window->m_applications = std::move(choices->apps);
        window->m_defaultApplication = std::move(choices->current);
        if (window->m_pendingApplication
            && window->m_defaultApplication
            && g_app_info_equal(window->m_pendingApplication.get(), window->m_defaultApplication.get()))
          window->m_pendingApplication.reset();
        auto identity = std::ranges::find(window->m_general, "Identity", &Section::title);
        if (identity != window->m_general.end()) {
          auto row = std::ranges::find(identity->rows, "Open with", &std::pair<std::string, std::string>::first);
          if (row != identity->rows.end())
            row->second = window->m_defaultApplication ? g_app_info_get_display_name(window->m_defaultApplication.get())
                                                       : "No default application";
        }
        window->m_contentDirty = true;
        if (window->m_surface)
          window->m_surface->requestLayout();
      },
      new QueryData{this, m_alive, m_generation, m_path}
  );
  g_task_set_task_data(task, new Choices{{}, m_mime, {}, {}}, [](gpointer data) {
    delete static_cast<Choices*>(data);
  });
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    auto* choices = static_cast<Choices*>(data);
    choices->accounts = filemanager::propertyAccounts();
    if (!choices->mime.empty()) {
      choices->current =
          std::shared_ptr<GAppInfo>(g_app_info_get_default_for_type(choices->mime.c_str(), FALSE), g_object_unref);
      GList* apps = g_app_info_get_all_for_type(choices->mime.c_str());
      for (GList* item = apps; item; item = item->next) {
        auto* app = G_APP_INFO(item->data);
        if (g_app_info_get_id(app))
          choices->apps.emplace_back(app, g_object_unref);
        else
          g_object_unref(app);
      }
      g_list_free(apps);
    }
    g_task_return_pointer(worker, new Choices(std::move(*choices)), [](gpointer value) {
      delete static_cast<Choices*>(value);
    });
  });
  g_object_unref(task);
}

void PropertiesWindow::applyPendingChanges() {
  if (m_applying || m_refreshing)
    return;
  m_applySummary.clear();
  if (filemanager::propertyApplyStep(m_path.filename().string(), m_nameDraft, permissionsPending())
      == filemanager::PropertyApplyStep::Rename)
    confirmRename(true);
  else
    applyPermissions();
}

void PropertiesWindow::confirmRename(bool applyOtherChanges) {
  if (m_applying || m_refreshing || !m_itemAvailable || m_nameDraft == m_path.filename().string())
    return;
  if (!filemanager::validFileName(m_nameDraft)) {
    m_renameError = "Choose a valid filename (not empty, . or .., and without /).";
    m_contentDirty = true;
    m_surface->requestLayout();
    return;
  }
  const auto destination = m_path.parent_path() / m_nameDraft;
  m_continueAfterRename = applyOtherChanges && permissionsPending();
  m_applySummary.clear();
  m_renaming = m_applying = true;
  const auto alive = std::weak_ptr<bool>(m_alive);
  const auto generation = m_generation;
  m_operation = m_operations.renameChecked(
      m_path.string(), destination.string(), m_device, m_inode,
      [this, alive, generation, destination](bool success, const std::string& message) {
        const auto token = alive.lock();
        if (!token || !*token || generation != m_generation)
          return;
        m_operation = 0;
        m_renaming = m_applying = false;
        if (!success)
          m_renameError = message;
        else
          m_renameError.clear();
        if (!success)
          m_continueAfterRename = false;
        if (success) {
          if (m_continueAfterRename)
            m_applySummary = message + " ";
          cancelFolderSize();
          m_path = destination;
          m_nameDraft = destination.filename().string();
          xdg_toplevel_set_title(m_surface->xdgToplevel(), ("Properties — " + m_nameDraft).c_str());
          m_choicesLoading = false;
          m_refreshing = true;
          m_retainPending = true;
          m_mountTarget = false;
          m_filesystem.rows.clear();
          m_capacity = -1.0F;
          refreshMetadata();
        }
        m_contentDirty = true;
        m_surface->requestLayout();
      }
  );
  m_contentDirty = true;
  updatePermissionControls();
}

void PropertiesWindow::chooseApplication(Button* anchor) {
  if (!m_dropdown || m_applying || m_refreshing)
    return;
  const auto choices = m_applications;
  SelectPopupContext::DropdownRequest request;
  float left, top, right, bottom;
  Node::transformedBounds(anchor, left, top, right, bottom);
  request.anchorX = static_cast<std::int32_t>(left);
  request.anchorY = static_cast<std::int32_t>(top);
  request.anchorWidth = static_cast<std::int32_t>(right - left);
  request.anchorHeight = static_cast<std::int32_t>(bottom - top);
  request.menuWidth = 240;
  request.maxMenuWidth = 360;
  request.fontSize = Style::fontSizeBody;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    request.options.emplace_back(g_app_info_get_display_name(choices[i].get()));
    const auto& selected = m_pendingApplication ? m_pendingApplication : m_defaultApplication;
    if (selected && g_app_info_equal(choices[i].get(), selected.get()))
      request.selectedIndex = i;
  }
  m_dropdown->openSelectDropdown(
      request,
      {.onSelect =
           [this, choices](std::size_t index) {
             if (m_applying || index >= choices.size())
               return;
             m_pendingApplication =
                 m_defaultApplication && g_app_info_equal(choices[index].get(), m_defaultApplication.get())
                 ? nullptr
                 : choices[index];
             m_discardRequested = false;
             m_contentDirty = true;
             m_surface->requestLayout();
           },
       .onDismiss = {}}
  );
}

void PropertiesWindow::queryFilesystemMetadata() {
  if (!m_surface || !m_generation)
    return;
  const auto generation = m_generation;
  struct MountData {
    std::filesystem::path path;
    Section section{"Filesystem", {}};
    bool mountTarget = false;
  };
  auto* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        std::unique_ptr<MountData> metadata(static_cast<MountData*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
        const auto alive = request->alive.lock();
        if (!alive
            || !*alive
            || request->generation != request->window->m_generation
            || request->path != request->window->m_path
            || !metadata)
          return;
        auto* window = request->window;
        window->m_mountTarget = metadata->mountTarget;
        window->queryMountCapacity();
        for (auto& row : metadata->section.rows)
          window->m_filesystem.rows.push_back(std::move(row));
        window->syncFilesystemSections();
      },
      new QueryData{this, m_alive, generation, m_path}
  );
  g_task_set_task_data(task, new MountData{m_path}, [](gpointer data) { delete static_cast<MountData*>(data); });
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    auto* metadata = static_cast<MountData*>(data);
    if (auto* entry = g_unix_mount_entry_for(metadata->path.c_str(), nullptr)) {
      const auto add = [&](const char* key, const char* value) {
        if (value && *value)
          metadata->section.rows.emplace_back(key, value);
      };
      metadata->mountTarget =
          filemanager::propertyMountTarget(metadata->path, g_unix_mount_entry_get_mount_path(entry));
      if (metadata->mountTarget) {
        add("Mount point", g_unix_mount_entry_get_mount_path(entry));
        add("Mount options", g_unix_mount_entry_get_options(entry));
      }
      g_unix_mount_entry_free(entry);
    }
    struct statvfs state{};
    if (metadata->mountTarget && statvfs(metadata->path.c_str(), &state) == 0)
      metadata->section.rows.emplace_back("Block size", sizeText(state.f_frsize));
    g_task_return_pointer(worker, new MountData(*metadata), [](gpointer value) {
      delete static_cast<MountData*>(value);
    });
  });
  g_object_unref(task);
}

void PropertiesWindow::queryMountCapacity() {
  if (!m_surface)
    return;
  const auto generation = m_generation;
  auto* file = fileForPath(m_path);
  auto* filesystemData = new QueryData{this, m_alive, generation, m_path};
  g_file_query_filesystem_info_async(
      file,
      m_mountTarget ? G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
                                                       "," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE
                                                       "," G_FILE_ATTRIBUTE_FILESYSTEM_READONLY
                    : G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
      G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        GError* error = nullptr;
        GFileInfo* info = g_file_query_filesystem_info_finish(G_FILE(source), result, &error);
        const auto alive = request->alive.lock();
        if (alive
            && *alive
            && request->generation == request->window->m_generation
            && (request->path.empty() || request->path == request->window->m_path))
          request->window->filesystemReady(info);
        if (info)
          g_object_unref(info);
        if (error)
          g_error_free(error);
      },
      filesystemData
  );
  g_object_unref(file);
}

void PropertiesWindow::queryReady(GFileInfo* info, const std::string& error) {
  m_loading = false;
  const bool replaced = info
      && m_refreshing
      && (g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE) != m_inode
          || g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) != m_device);
  m_refreshing = false;
  m_itemAvailable = info && !replaced;
  if (!info || replaced) {
    cancelFolderSize();
    m_permissionEditable = false;
    m_mountTarget = false;
    m_filesystem.rows.clear();
    m_capacity = -1.0F;
    m_modeAvailable = false;
    m_permissions.clear();
    m_details.clear();
    m_general = {
        {"Item status",
         {{"State", "Unavailable"}, {"Reason", replaced ? "The item was replaced. Reopen Properties." : error}}}
    };
  } else {
    const auto name = g_file_info_get_display_name(info);
    if (name)
      m_displayName = name;
    const bool link = g_file_info_get_is_symlink(info);
    const auto type = g_file_info_get_file_type(info);
    m_directory = type == G_FILE_TYPE_DIRECTORY;
    Section identity{"Identity", {}};
    identity.rows.emplace_back("Name", name ? name : m_path.filename().string());
    identity.rows.emplace_back("Type", filemanager::propertyTypeDescription(info));
    identity.rows.emplace_back("Location", m_path.parent_path().string());
    identity.rows.emplace_back("Full path", m_path.string());
    if (link) {
      const auto target = g_file_info_get_symlink_target(info);
      if (target) {
        const std::filesystem::path targetPath(target);
        identity.rows.emplace_back(
            "Link target",
            (targetPath.is_absolute() ? targetPath : m_path.parent_path() / targetPath).lexically_normal().string()
        );
      }
    }
    const auto mime = g_file_info_get_content_type(info);
    m_mime = mime && !link ? mime : "";
    if (!m_mime.empty())
      identity.rows.emplace_back("Open with", "Loading applications…");
    m_general = {std::move(identity)};

    Section size{"Size and storage", {}};
    if (link) {
      size.rows.emplace_back(
          "Link size",
          g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ? sizeText(g_file_info_get_size(info))
                                                                          : "Unavailable"
      );
    } else if (type != G_FILE_TYPE_DIRECTORY && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
      size.rows.emplace_back("Size", sizeText(g_file_info_get_size(info)));
      if (!link && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_BLOCKS))
        size.rows.emplace_back(
            "Size on disk", sizeText(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_BLOCKS) * 512ULL)
        );
    } else if (type == G_FILE_TYPE_DIRECTORY) {
      size.rows.emplace_back("Folder size", "Calculating…");
    }
    m_general.push_back(std::move(size));

    Section timestamps{"Timestamps", {}};
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_CREATED))
      timestamps.rows.emplace_back(
          "Created", timeText(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED))
      );
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
      timestamps.rows.emplace_back(
          "Modified", timeText(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
      );
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS))
      timestamps.rows.emplace_back(
          "Last accessed", timeText(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_ACCESS))
      );
    if (!timestamps.rows.empty())
      m_general.push_back(std::move(timestamps));

    const bool hasUid = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_UID);
    const bool hasGid = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_GID);
    const bool hasMode = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    m_modeAvailable = hasMode;
    const auto mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    const auto uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
    const auto gid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);
    m_uid = uid;
    m_gid = gid;
    if (!m_retainPending) {
      m_pendingOwner.clear();
      m_pendingGroup.clear();
    } else {
      for (const auto& account : m_accounts.users)
        if (account.name == m_pendingOwner && account.id == uid)
          m_pendingOwner.clear();
      for (const auto& account : m_accounts.groups)
        if (account.name == m_pendingGroup && account.id == gid)
          m_pendingGroup.clear();
    }
    const auto pendingMask = (m_originalMode ^ m_editedMode) & 0777U;
    const auto desired = m_editedMode;
    m_originalMode = mode;
    m_editedMode = m_retainPending ? (mode & ~pendingMask) | (desired & pendingMask) : mode;
    m_retainPending = false;
    m_inode = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
    m_device = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    m_permissionEditable = hasMode
        && hasUid
        && !link
        && m_path.is_absolute()
        && (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_DIRECTORY)
        && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE)
        && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    const auto* ownerName = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER);
    const auto* groupName = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP);
    m_permissions = {
        {"Ownership",
         {{"Owner",
           hasUid ? std::string(ownerName ? ownerName : "Unknown") + " (UID " + std::to_string(uid) + ")"
                  : "Unavailable"},
          {"Group",
           hasGid ? std::string(groupName ? groupName : "Unknown") + " (GID " + std::to_string(gid) + ")"
                  : "Unavailable"}}}
    };
    m_permissionMessage = m_permissionEditable ? ""
        : link                                 ? "Link permissions are read-only; the target is never changed."
                                               : "Permissions are read-only for this item or current process.";
    if (hasMode) {
      Section special{"Special permissions", {}};
      special.rows.emplace_back("Bits", specialPermissionsText(mode));
      m_permissions.push_back(std::move(special));
    }

    Section technical{"File information", {}};
    if (mime)
      technical.rows.emplace_back("MIME type", mime);
    const auto extension = filemanager::propertyFilenameSuffix(m_path, m_directory);
    if (!extension.empty() && type != G_FILE_TYPE_DIRECTORY)
      technical.rows.emplace_back("Filename suffix", extension);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE))
      technical.rows.emplace_back(
          "Inode", std::to_string(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE))
      );
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE))
      technical.rows.emplace_back(
          "Device ID", std::to_string(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE))
      );
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_NLINK))
      technical.rows.emplace_back(
          "Hard links", std::to_string(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_NLINK))
      );
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN))
      technical.rows.emplace_back("Hidden", g_file_info_get_is_hidden(info) ? "Yes" : "No");
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP))
      technical.rows.emplace_back("Backup file", g_file_info_get_is_backup(info) ? "Yes" : "No");
    if (link)
      technical.rows.emplace_back("Link metadata", "Metadata describes the link itself; its target was not followed");
    if (const auto* id = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM))
      m_filesystemId = id;
    m_details = {std::move(technical)};
    {
      std::vector<std::string> iconNames;
      GIcon* icon = g_file_info_get_icon(info);
      if (icon && G_IS_EMBLEMED_ICON(icon))
        icon = g_emblemed_icon_get_icon(G_EMBLEMED_ICON(icon));
      if (icon && G_IS_THEMED_ICON(icon)) {
        const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
        for (auto iconName = names; iconName && *iconName; ++iconName)
          iconNames.emplace_back(*iconName);
      }
      if (iconNames.empty())
        iconNames.push_back(m_directory ? "folder" : "text-x-generic");
      m_iconNames = std::move(iconNames);
      if (m_icon) {
        m_icon->setFile(m_path, m_directory);
        m_icon->setThemedIconNames(m_iconNames);
      }
    }
    Section other{"Other metadata", {}};
    if (const auto* context = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_SELINUX_CONTEXT))
      other.rows.emplace_back("SELinux context", context);
    if (!other.rows.empty())
      m_details.push_back(std::move(other));
    if (!link)
      queryFilesystemMetadata();
    loadChoices();
    if (m_directory && !link) {
      m_details.push_back(
          {"Folder calculation",
           {{"Counting policy",
             "Regular files, folders, links and special entries are counted separately; the selected root is excluded. "
             "Hidden entries are included."},
            {"Logical bytes",
             "Regular-file and link sizes; hard links count once per name. Directory inode sizes and allocated storage "
             "are excluded."},
            {"Boundaries",
             "Symbolic links are not followed. Child mounts and unsafe or unreadable subtrees are excluded. One failed "
             "subtree may omit many descendants."},
            {"Limits",
             "Depth 128; 1,000,000 entries; relative paths 4,096 bytes. Results are observations, not a filesystem "
             "snapshot."}}}
      );
      ensureFolderSize();
    }
  }
  syncFilesystemSections();
  if (m_surface)
    m_surface->requestLayout();
  m_contentDirty = true;
  if (m_continueAfterRename) {
    m_continueAfterRename = false;
    if (m_itemAvailable)
      applyPermissions();
    else {
      m_renameError = "Incomplete apply. "
          + m_applySummary
          + "Could not refresh the renamed item; remaining changes were not applied.";
      m_applySummary.clear();
    }
  }
}

void PropertiesWindow::filesystemReady(GFileInfo* info) {
  if (!info)
    return;
  if (g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY)) {
    m_permissionEditable = false;
    m_permissionMessage = "This filesystem is read-only.";
    m_contentDirty = true;
    updatePermissionControls();
  }
  if (!m_mountTarget)
    return;
  if (const auto* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE))
    m_filesystem.rows.emplace_back("Filesystem type", type);
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
    const auto total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
      const auto free = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
      const auto used = total - std::min(total, free);
      std::ostringstream capacity;
      capacity << sizeText(used) << " / " << sizeText(total) << " used";
      if (total)
        capacity
            << " ("
            << std::fixed
            << std::setprecision(1)
            << static_cast<double>(used) / static_cast<double>(total) * 100.0
            << "%)";
      m_filesystem.rows.emplace_back("Capacity", capacity.str());
      m_filesystem.rows.emplace_back("Free space", sizeText(free));
      if (total)
        m_capacity = static_cast<float>(static_cast<double>(used) / static_cast<double>(total));
    } else {
      m_filesystem.rows.emplace_back("Capacity", sizeText(total));
    }
  }
  syncFilesystemSections();
}

void PropertiesWindow::syncFilesystemSections() {
  // Keep the overview compact; technical filesystem fields belong to Details.
  auto storage = std::ranges::find(m_general, "Size and storage", &Section::title);
  if (storage != m_general.end()) {
    std::erase_if(storage->rows, [](const auto& row) {
      return row.first == "Filesystem type"
          || row.first == "Mount point"
          || row.first == "Capacity"
          || row.first == "Free space";
    });
    if (m_mountTarget)
      for (const auto* name : {"Filesystem type", "Mount point", "Capacity", "Free space"})
        for (const auto& row : m_filesystem.rows)
          if (row.first == name)
            storage->rows.push_back(row);
  }
  Section details{"Filesystem", {}};
  if (m_mountTarget && !m_filesystemId.empty())
    details.rows.emplace_back("Filesystem ID", m_filesystemId);
  if (m_mountTarget)
    for (const auto& row : m_filesystem.rows)
      if (row.first != "Capacity" && row.first != "Free space")
        details.rows.push_back(row);
  auto found = std::ranges::find(m_details, "Filesystem", &Section::title);
  if (found != m_details.end())
    *found = std::move(details);
  else if (!details.rows.empty())
    m_details.insert(m_details.begin() + std::min<std::size_t>(1, m_details.size()), std::move(details));
  if (!m_mountTarget)
    std::erase_if(m_details, [](const auto& section) { return section.title == "Filesystem"; });
  m_contentDirty = true;
  if (m_surface)
    m_surface->requestLayout();
}

bool PropertiesWindow::permissionsPending() const {
  return ((m_originalMode ^ m_editedMode) & 0777) != 0
      || !m_pendingOwner.empty()
      || !m_pendingGroup.empty()
      || m_pendingApplication != nullptr;
}

void PropertiesWindow::updateFooter() {
  if (!m_closeButton || !m_applyButton)
    return;
  const auto step = filemanager::propertyApplyStep(m_path.filename().string(), m_nameDraft, permissionsPending());
  const bool dirty = step != filemanager::PropertyApplyStep::None;
  const bool busy = m_applying || m_continueAfterRename;
  m_closeButton->setText(dirty ? (m_discardRequested ? "Discard & Close" : "Cancel") : "Close");
  if (m_footerStatus)
    m_footerStatus->setText(
        busy                     ? "Working…"
            : m_discardRequested ? "Discard changes?"
            : dirty              ? "Unapplied changes"
                                 : ""
    );
  m_closeButton->setTooltip("");
  m_closeButton->setEnabled(!m_applying || m_operation != 0 || m_continueAfterRename);
  if (busy)
    m_closeButton->setText(m_operation || m_continueAfterRename ? "Cancel operation" : "Saving…");
  m_applyButton->setVisible(dirty);
  m_applyButton->setTooltip(
      permissionsPending() && step == filemanager::PropertyApplyStep::Rename
          ? "Apply rename and all other pending changes"
          : ""
  );
  m_applyButton->setEnabled(
      dirty
      && (step == filemanager::PropertyApplyStep::Rename ? m_itemAvailable
                                                         : (m_permissionEditable || m_pendingApplication != nullptr))
      && !busy
      && !m_refreshing
  );
  m_applyButton->setText(m_applying ? "Applying…" : "Apply");
}

void PropertiesWindow::updatePermissionControls() {
  for (std::size_t i = 0; i < m_ownershipSelects.size(); ++i)
    if (auto* select = m_ownershipSelects[i])
      select->setEnabled(
          m_permissionEditable
          && !m_applying
          && !m_refreshing
          && !m_choicesLoading
          && !(i == 0 ? m_accounts.users : m_accounts.groups).empty()
      );
  if (m_recursiveCheck) {
    m_recursiveCheck->setChecked(m_recursive);
    m_recursiveCheck->setEnabled(!m_applying && !m_refreshing && m_pendingOwner.empty() && m_pendingGroup.empty());
  }
  for (std::size_t row = 0; row < 3; ++row) {
    const auto bits = (m_editedMode >> ((2 - row) * 3)) & 7U;
    for (std::size_t bit = 0; bit < 3; ++bit)
      if (auto* check = m_permissionChecks[row][bit]) {
        check->setChecked((bits & (4U >> bit)) != 0);
        check->setEnabled(m_permissionEditable && !m_applying && !m_refreshing);
      }
  }
  updateFooter();
  if (m_surface)
    m_surface->requestLayout();
}

void PropertiesWindow::applyPermissions() {
  if (m_applying || m_refreshing || !permissionsPending())
    return;
  const bool filesystemChange =
      ((m_originalMode ^ m_editedMode) & 0777) != 0 || !m_pendingOwner.empty() || !m_pendingGroup.empty();
  if (!filesystemChange) {
    applyDefaultApplication();
    return;
  }
  if (!m_permissionEditable) {
    m_permissionError = "Incomplete apply. "
        + m_applySummary
        + "Permission or ownership changes are unavailable; remaining edits stay staged.";

    m_applySummary.clear();
    m_contentDirty = true;
    updatePermissionControls();
    return;
  }
  const filemanager::PropertyRequest request{
      m_path,         m_device,       m_inode,    m_originalMode & 07777U, m_editedMode & 07777U,
      m_pendingOwner, m_pendingGroup, m_recursive
  };
  m_applying = true;
  m_contentDirty = true;
  m_discardRequested = false;
  updatePermissionControls();
  const auto alive = std::weak_ptr<bool>(m_alive);
  const auto generation = m_generation;
  m_operation = m_operations.modifyProperties(request, m_uid, [this, alive, generation](const auto& result) {
    const auto token = alive.lock();
    if (!token || !*token || generation != m_generation)
      return;
    m_operation = 0;
    m_permissionError = result.needsFeedback() ? m_applySummary + result.summary() : "";
    if (result.ok() && m_pendingApplication) {
      m_applySummary += result.summary() + " ";
      applyDefaultApplication();
      return;
    }
    finishChanges(!result.ok());
  });
  if (!m_operation) {
    m_applying = false;
    m_permissionError = "An operation on this item is already active.";
    m_contentDirty = true;
    updatePermissionControls();
  } else
    updatePermissionControls();
}

void PropertiesWindow::finishChanges(bool retainPending) {
  m_contentDirty = true;
  m_retainPending = retainPending;
  m_applying = false;
  m_applySummary.clear();
  m_recursive = false;
  m_refreshing = true;
  m_filesystem.rows.clear();
  refreshMetadata();
  updatePermissionControls();
}

void PropertiesWindow::applyDefaultApplication() {
  if (!m_pendingApplication || m_mime.empty()) {
    finishChanges();
    return;
  }
  struct ApplicationChange {
    std::shared_ptr<GAppInfo> app;
    std::string mime;
  };
  m_applying = true;
  m_contentDirty = true;
  updatePermissionControls();
  auto* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        GError* error = nullptr;
        const bool ok = g_task_propagate_boolean(G_TASK(result), &error);
        const auto alive = request->alive.lock();
        if (alive
            && *alive
            && request->generation == request->window->m_generation
            && (request->path.empty() || request->path == request->window->m_path)) {
          auto* window = request->window;
          if (ok) {
            window->m_pendingApplication.reset();
            window->m_applicationError.clear();
          } else
            window->m_applicationError = window->m_applySummary
                + "Could not save default application: "
                + (error ? error->message : "Unknown error");
          window->finishChanges();
        }
        g_clear_error(&error);
      },
      new QueryData{this, m_alive, m_generation, m_path}
  );
  g_task_set_task_data(task, new ApplicationChange{m_pendingApplication, m_mime}, [](gpointer data) {
    delete static_cast<ApplicationChange*>(data);
  });
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    const auto* change = static_cast<ApplicationChange*>(data);
    GError* error = nullptr;
    if (filemanager::savePropertyDefaultApplication(change->app.get(), change->mime, &error))
      g_task_return_boolean(worker, true);
    else if (error)
      g_task_return_error(worker, error);
    else
      g_task_return_new_error(worker, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not save default application");
  });
  g_object_unref(task);
}

void PropertiesWindow::close() {
  cancelFolderSize();
  if (m_tooltipOwned)
    TooltipManager::instance().forceDestroy();
  m_tooltipOwned = false;
  m_continueAfterRename = false;
  if (m_operation)
    m_operations.cancel(m_operation);
  if (m_dropdown)
    m_dropdown->closeSelectDropdown();
  m_dropdown.reset();
  if (m_surface && m_surface->width() >= 440 && m_surface->height() >= 400) {
    m_preferredWidth = m_surface->width();
    m_preferredHeight = m_surface->height();
    m_config.setStateString(
        "files", "properties_window_size", std::to_string(m_preferredWidth) + "x" + std::to_string(m_preferredHeight)
    );
  }
  ++m_generation;
  m_input.setSceneRoot(nullptr);
  if (m_surface)
    m_surface->setSceneRoot(nullptr);
  m_root.reset();
  m_sizeSpinner = nullptr;
  m_sizeRefreshButton = nullptr;
  m_sizeStopButton = nullptr;
  m_animations.cancelAll();
  m_body = nullptr;
  m_icon = nullptr;
  m_resizeEdges = {};
  m_nameInput = nullptr;
  m_recursiveCheck = nullptr;
  m_closeButton = m_applyButton = nullptr;
  m_footerStatus = nullptr;
  m_ownershipSelects = {};
  m_permissionChecks = {};
  m_tabButtons.fill(nullptr);
  m_surface.reset();
}

void PropertiesWindow::deferClose(bool discard) {
  if (m_continueAfterRename && m_refreshing) {
    m_continueAfterRename = false;
    m_permissionError = "Remaining changes cancelled; they remain staged.";
    m_contentDirty = true;
    m_applySummary.clear();
    updatePermissionControls();
    return;
  }
  // Only the explicit footer discard action drops staged edits. An active Apply must finish.
  if (m_applying) {
    if (!m_operation)
      return;
    m_continueAfterRename = false;
    m_operations.cancel(m_operation);
    m_permissionError = "Cancellation requested. Previously applied changes are not rolled back.";
    m_contentDirty = true;
    updatePermissionControls();
    return;
  }
  if ((permissionsPending() || m_nameDraft != m_path.filename().string()) && !discard) {
    m_discardRequested = true;
    updateFooter();
    if (m_surface)
      m_surface->requestLayout();
    return;
  }
  auto* pending = new QueryData{this, m_alive, m_generation, m_path};
  g_idle_add(
      [](gpointer data) -> gboolean {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        const auto alive = request->alive.lock();
        if (alive
            && *alive
            && request->generation == request->window->m_generation
            && (request->path.empty() || request->path == request->window->m_path))
          request->window->close();
        return G_SOURCE_REMOVE;
      },
      pending
  );
}

void PropertiesWindow::rebuildTab() {
  m_sizeSpinner = nullptr;
  m_sizeRefreshButton = nullptr;
  m_sizeStopButton = nullptr;
  auto focus = m_input.captureTabFocus();
  m_input.setFocus(nullptr);
  if (m_tooltipOwned)
    TooltipManager::instance().forceDestroy();
  m_tooltipOwned = false;
  m_ownershipSelects = {};
  m_permissionChecks = {};
  m_nameInput = nullptr;
  m_icon = nullptr;
  m_recursiveCheck = nullptr;
  if (m_dropdown)
    m_dropdown->closeSelectDropdown();
  while (!m_body->children().empty())
    m_body->removeChild(m_body->children().back().get());
  const auto label = [](const std::string& text, bool subdued = false) {
    auto result = std::make_unique<Label>();
    result->setText(text);
    result->setFontSize(Style::fontSizeBody * 0.92F);
    if (subdued)
      result->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    return result;
  };
  const auto section = [&](const std::string& text) {
    auto result = std::make_unique<Flex>();
    result->setDirection(FlexDirection::Vertical);
    result->setAlign(FlexAlign::Stretch);
    result->setGap(7.0F);
    result->setPadding(3.0F, 2.0F);
    auto title = label(text);
    title->setFontWeight(FontWeight::SemiBold);
    title->setColor(colorSpecFromRole(ColorRole::Primary));
    result->addChild(std::move(title));
    result->addChild(std::make_unique<Separator>());
    return result;
  };
  const auto errorNote = [&](Flex& parent, const std::string& message, std::function<void()> dismiss) {
    if (message.empty())
      return;
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Start);
    row->setGap(6.0F);
    auto text = label(message);
    text->setColor(colorSpecFromRole(ColorRole::Error));
    text->setFlexGrow(1.0F);
    text->setMaxLines(0);
    row->addChild(std::move(text));
    auto button = std::make_unique<Button>();
    button->setText("Dismiss");
    button->setVariant(ButtonVariant::Ghost);
    button->setControlHeight(Style::controlHeightSm);
    button->setOnClick([this, dismiss = std::move(dismiss)] {
      dismiss();
      m_contentDirty = true;
      m_surface->requestLayout();
    });
    row->addChild(std::move(button));
    parent.addChild(std::move(row));
  };
  const auto& lines = m_tab == 0 ? m_general : (m_tab == 1 ? m_permissions : m_details);
  if (m_loading) {
    m_body->addChild(label("Reading item metadata…", true));
  } else {
    for (const auto& sectionData : lines) {
      if (sectionData.rows.empty())
        continue;
      auto group = section(sectionData.title == "Size and storage" && !m_mountTarget ? "Size" : sectionData.title);
      auto rows = std::make_unique<Flex>();
      rows->setDirection(FlexDirection::Vertical);
      rows->setAlign(FlexAlign::Stretch);
      rows->setGap(7.0F);
      for (const auto& [name, value] : sectionData.rows) {
        auto row = std::make_unique<Flex>();
        row->setDirection(FlexDirection::Horizontal);
        row->setAlign(FlexAlign::Center);
        row->setGap(12.0F);
        auto key = label(name, true);
        const float keyWidth = sectionData.title == "Identity" ? 84.0F : 126.0F;
        key->setMinWidth(keyWidth);
        key->setMaxWidth(keyWidth);
        row->addChild(std::move(key));
        if (m_tab == 0 && sectionData.title == "Identity" && name == "Name" && !m_volumeOnly) {
          auto editor = std::make_unique<Input>();
          editor->setValue(m_nameDraft);
          editor->setControlHeight(Style::controlHeightSm);
          editor->setFlexGrow(1.0F);
          editor->setEnabled(!m_applying && !m_refreshing && m_itemAvailable && m_path != m_path.root_path());
          editor->setOnChange([this](const std::string& draft) {
            m_nameDraft = draft;
            if (m_nameInput)
              m_nameInput->setInvalid(!filemanager::validFileName(draft));
            m_discardRequested = false;
            updateFooter();
          });
          editor->setOnCopyText(m_copyText);
          editor->setOnSubmit([this](const std::string&) { confirmRename(); });
          editor->setOnKeyEvent([this](std::uint32_t symbol, std::uint32_t) {
            if (symbol != XKB_KEY_Escape)
              return false;
            m_nameDraft = m_path.filename().string();
            m_renameError.clear();
            m_contentDirty = true;
            m_discardRequested = false;
            if (m_nameInput)
              m_nameInput->setValue(m_nameDraft);
            updateFooter();
            return true;
          });
          m_nameInput = editor.get();
          row->addChild(std::move(editor));
          rows->addChild(std::move(row));
          errorNote(*rows, m_renameError, [this] { m_renameError.clear(); });
          continue;
        }
        if (m_tab == 1
            && m_permissionEditable
            && sectionData.title == "Ownership"
            && (name == "Owner" || name == "Group")) {
          const bool groupChoice = name == "Group";
          const auto choices = groupChoice ? m_accounts.groups : m_accounts.users;
          const auto current = groupChoice ? m_gid : m_uid;
          const auto pending = groupChoice ? m_pendingGroup : m_pendingOwner;
          auto select = std::make_unique<Select>();
          select->setControlHeight(Style::controlHeightSm);
          select->setFlexGrow(1.0F);
          select->setFontSize(Style::fontSizeBody * 0.92F);
          std::vector<std::string> options;
          std::size_t selected = static_cast<std::size_t>(-1);
          for (std::size_t i = 0; i < choices.size(); ++i) {
            options.push_back(
                choices[i].name + (groupChoice ? " (GID " : " (UID ") + std::to_string(choices[i].id) + ")"
            );
            if ((!pending.empty() && choices[i].name == pending) || (pending.empty() && choices[i].id == current))
              selected = i;
          }
          select->setOptions(std::move(options));
          if (selected != static_cast<std::size_t>(-1))
            select->setSelectedIndexSilently(selected);
          else {
            select->clearSelection();
            select->setPlaceholder(value);
          }
          select->setEnabled(
              m_permissionEditable && !m_applying && !m_refreshing && !m_choicesLoading && !choices.empty()
          );
          select->setOnSelectionChanged([this, choices, groupChoice, current](std::size_t index, std::string_view) {
            if (m_applying || index >= choices.size())
              return;
            auto& staged = groupChoice ? m_pendingGroup : m_pendingOwner;
            staged = choices[index].id == current ? "" : choices[index].name;
            m_recursive = false;
            m_discardRequested = false;
            updatePermissionControls();
          });
          m_ownershipSelects[groupChoice ? 1 : 0] = select.get();
          row->addChild(std::move(select));
          rows->addChild(std::move(row));
          continue;
        }
        auto val = std::make_unique<Input>();
        val->setValue(
            m_tab == 0 && name == "Open with" && m_pendingApplication
                ? std::string(g_app_info_get_display_name(m_pendingApplication.get())) + " (pending)"
                : value.empty() ? "Unavailable"
                                : value
        );
        val->setFontSize(Style::fontSizeBody * 0.92F);
        val->setReadOnly(true);
        val->setFrameVisible(false);
        val->setHorizontalPadding(0);
        val->setMinLayoutWidth(0);
        val->setFlexGrow(1.0F);
        const bool expanded = m_tab == 2 && m_expandedMetadata.contains(name);
        val->setMultiline(expanded);
        val->setControlHeight(Style::fontSizeBody * (expanded ? 6.0F : 1.25F));
        val->setOnCopyText(m_copyText);
        auto* valueArea = val->inputArea();
        row->addChild(std::move(val));
        if (m_tab == 0 && name == "Folder size" && m_folderScanning) {
          auto spinner = std::make_unique<Spinner>();
          spinner->setSpinnerSize(12.0F);
          spinner->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
          m_sizeSpinner = spinner.get();
          row->addChild(std::move(spinner));
        }
        if (m_tab == 2 && value.size() > 36) {
          auto expand = std::make_unique<Button>();
          expand->setText(expanded ? "Less" : "More");
          expand->setVariant(ButtonVariant::Ghost);
          expand->setControlHeight(Style::controlHeightSm);
          expand->setOnClick([this, name] {
            if (!m_expandedMetadata.erase(name))
              m_expandedMetadata.insert(name);
            m_contentDirty = true;
            m_surface->requestLayout();
          });
          row->addChild(std::move(expand));
        }
        const bool application = m_tab == 0 && name == "Open with";
        if (application) {
          auto change = std::make_unique<Button>();
          change->setText("Change…");
          change->setVariant(ButtonVariant::Outline);
          change->setControlHeight(Style::controlHeightSm);
          change->setEnabled(!m_applying && !m_refreshing && !m_choicesLoading && !m_applications.empty());
          auto* action = change.get();
          change->setOnClick([this, action] { chooseApplication(action); });
          row->addChild(std::move(change));
        }
        if (m_copyText && (name == "Location" || name == "Full path" || name == "Link target") && value.size() > 48) {
          auto copy = std::make_unique<Button>();
          copy->setGlyph("copy");
          copy->setGlyphSize(12.0F);
          copy->setVariant(ButtonVariant::Ghost);
          copy->setControlHeight(24.0F);
          copy->setPadding(3.0F);
          copy->setTooltip("Copy " + name);
          copy->setOpacity(0.0F);
          copy->setTabStop(false);
          auto* copyAction = copy.get();
          valueArea->setOnEnter([copyAction](const InputArea::PointerData&) { copyAction->setOpacity(1.0F); });
          valueArea->setOnLeave([copyAction] { copyAction->setOpacity(0.0F); });
          copy->setOnEnter([copyAction] { copyAction->setOpacity(1.0F); });
          copy->setOnLeave([copyAction] { copyAction->setOpacity(0.0F); });
          copy->setOnClick([this, value] { m_copyText(value); });
          row->addChild(std::move(copy));
        }
        rows->addChild(std::move(row));
        if (application)
          errorNote(*rows, m_applicationError, [this] { m_applicationError.clear(); });
      }
      if (m_tab == 1 && sectionData.title == "Ownership") {
        if (!m_permissionEditable && !m_permissionMessage.empty()) {
          auto note = label(m_permissionMessage, true);
          note->setMaxLines(0);
          rows->addChild(std::move(note));
        }
        if (!m_modeAvailable)
          errorNote(*rows, m_permissionError, [this] { m_permissionError.clear(); });
      }
      if (m_tab == 0 && sectionData.title == "Identity") {
        auto identity = std::make_unique<Flex>();
        identity->setDirection(FlexDirection::Horizontal);
        identity->setAlign(FlexAlign::Center);
        identity->setGap(14.0F);
        auto icon = std::make_unique<FileIcon>();
        if (!m_volumeOnly)
          icon->setFile(m_path, m_directory);
        icon->setThemedIconNames(m_iconNames);
        icon->setGlyphSize(64.0F);
        m_icon = icon.get();
        identity->addChild(std::move(icon));
        rows->setFlexGrow(1.0F);
        identity->addChild(std::move(rows));
        group->addChild(std::move(identity));
      } else {
        group->addChild(std::move(rows));
      }
      if (m_tab == 0 && sectionData.title == "Identity") {
        if (!m_mime.empty()) {
          auto scope = label("Default application changes apply to all " + m_mime + " items.", true);
          scope->setMaxLines(2);
          group->addChild(std::move(scope));
        }
      }
      if (m_tab == 0 && sectionData.title == "Size and storage" && m_mountTarget && m_capacity >= 0) {
        auto capacity = std::make_unique<ProgressBar>();
        capacity->setProgress(m_capacity);
        capacity->setSize(0.0F, 8.0F);
        capacity->setRadius(4.0F);
        group->addChild(std::move(capacity));
      }
      if (m_tab == 0 && sectionData.title == "Size and storage" && m_directory) {
        auto controls = std::make_unique<Flex>();
        controls->setDirection(FlexDirection::Horizontal);
        controls->setAlign(FlexAlign::Center);
        controls->setJustify(FlexJustify::End);
        controls->setGap(4.0F);
        auto refresh = std::make_unique<Button>();
        refresh->setGlyph("refresh");
        refresh->setGlyphSize(14.0F);
        refresh->setVariant(ButtonVariant::Ghost);
        refresh->setControlHeight(26.0F);
        refresh->setPadding(5.0F);
        refresh->setTooltip("Refresh folder size");
        refresh->setEnabled(!m_folderScanning);
        refresh->setOnClick([this] {
          if (m_folderScanning)
            return;
          cancelFolderSize();
          ensureFolderSize();
        });
        m_sizeRefreshButton = refresh.get();
        controls->addChild(std::move(refresh));
        auto stop = std::make_unique<Button>();
        stop->setGlyph("stop");
        stop->setGlyphSize(13.0F);
        stop->setVariant(ButtonVariant::Ghost);
        stop->setControlHeight(26.0F);
        stop->setPadding(5.0F);
        stop->setTooltip("Stop folder size calculation");
        stop->setEnabled(m_folderScanning);
        stop->setOnClick([this] {
          if (m_folderScanning && m_folderCancel)
            g_cancellable_cancel(m_folderCancel);
        });
        m_sizeStopButton = stop.get();
        controls->addChild(std::move(stop));
        group->addChild(std::move(controls));
      }
      m_body->addChild(std::move(group));
    }
    if (m_tab == 1 && !m_permissions.empty()) {
      if (m_modeAvailable) {
        auto access = section("Access permissions");
        const char* names[] = {"Owner", "Group", "Others"};
        const char* headings[] = {"Read", "Write", m_directory ? "Traverse" : "Execute"};
        auto columns = std::make_unique<Flex>();
        columns->setDirection(FlexDirection::Horizontal);
        columns->setAlign(FlexAlign::Center);
        columns->setGap(8.0F);
        auto blank = std::make_unique<Flex>();
        blank->setFlexGrow(1.0F);
        columns->addChild(std::move(blank));
        for (const auto* heading : headings) {
          auto column = std::make_unique<Flex>();
          column->setMinWidth(70);
          column->setMaxWidth(70);
          column->setJustify(FlexJustify::Center);
          column->addChild(label(heading, true));
          columns->addChild(std::move(column));
        }
        access->addChild(std::move(columns));
        for (std::size_t rowIndex = 0; rowIndex < 3; ++rowIndex) {
          auto row = std::make_unique<Flex>();
          row->setDirection(FlexDirection::Horizontal);
          row->setAlign(FlexAlign::Center);
          row->setGap(6.0F);
          auto name = label(names[rowIndex]);
          name->setFlexGrow(1.0F);
          row->setGap(8.0F);
          row->addChild(std::move(name));
          for (std::size_t bit = 0; bit < 3; ++bit) {
            auto check = std::make_unique<Checkbox>();
            const auto mask = (4U >> bit) << ((2 - rowIndex) * 3);
            check->setOnChange([this, mask](bool checked) {
              m_editedMode = checked ? m_editedMode | mask : m_editedMode & ~mask;
              m_discardRequested = false;
              updatePermissionControls();
            });
            m_permissionChecks[rowIndex][bit] = check.get();
            auto cell = std::make_unique<Flex>();
            cell->setMinWidth(70);
            cell->setMaxWidth(70);
            cell->setJustify(FlexJustify::Center);
            cell->addChild(std::move(check));
            row->addChild(std::move(cell));
          }
          access->addChild(std::move(row));
          if (rowIndex != 2)
            access->addChild(std::make_unique<Separator>());
        }
        errorNote(*access, m_permissionError, [this] { m_permissionError.clear(); });
        m_body->addChild(std::move(access));
      }
      if (m_directory && m_permissionEditable) {
        auto contents = section("Apply to contents");
        auto row = std::make_unique<Flex>();
        row->setDirection(FlexDirection::Horizontal);
        row->setAlign(FlexAlign::Center);
        row->setGap(6.0F);
        auto recursive = std::make_unique<Checkbox>();
        m_recursiveCheck = recursive.get();
        recursive->setOnChange([this](bool checked) {
          m_recursive = checked;
          m_discardRequested = false;
          updatePermissionControls();
        });
        row->addChild(std::move(recursive));
        auto text = label("Apply changes recursively to all files and folders");
        text->setFlexGrow(1.0F);
        text->setMaxLines(2);
        row->addChild(std::move(text));
        contents->addChild(std::move(row));
        m_body->addChild(std::move(contents));
      }

    } else if (lines.empty()) {
      m_body->addChild(label("No metadata available for this item.", true));
    }
  }
  updatePermissionControls();
  if (m_sizeSpinner) {
    m_sizeSpinner->setAnimationManager(&m_animations);
    m_sizeSpinner->start();
  }
  m_renderedTab = m_tab;
  m_contentDirty = false;
  m_input.restoreTabFocus(std::move(focus));
}

void PropertiesWindow::prepareFrame(bool, bool needsLayout) {
  if (!m_surface || !m_surface->width() || !m_surface->height())
    return;
  m_renderer.makeCurrent(m_surface->renderTarget());
  auto& renderer = m_surface->renderTarget().renderer();
  if (!m_root) {
    m_dropdown = std::make_unique<SelectDropdownPopup>(m_wayland, m_renderer);
    m_dropdown->setParent(
        m_surface->xdgSurface(), m_surface->wlSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
    );
    m_root = std::make_unique<Flex>();
    m_root->setPopupContext(m_dropdown.get());
    m_root->setDirection(FlexDirection::Vertical);
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(10);
    m_root->setPadding(18);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.96F));
    m_root->clearBorder();
    auto tabs = std::make_unique<Flex>();
    tabs->setDirection(FlexDirection::Horizontal);
    tabs->setGap(8);
    const char* names[] = {"General", "Permissions", "Details"};
    for (int i = 0; i < 3; ++i) {
      auto tab = std::make_unique<Button>();
      tab->setText(names[i]);
      tab->setControlHeight(Style::controlHeightSm);
      tab->setVariant(i == m_tab ? ButtonVariant::TabActive : ButtonVariant::Tab);
      m_tabButtons[static_cast<std::size_t>(i)] = tab.get();
      tab->setOnClick([this, i] {
        m_tab = i;
        m_contentDirty = true;
        for (std::size_t tabIndex = 0; tabIndex < m_tabButtons.size(); ++tabIndex)
          if (m_tabButtons[tabIndex])
            m_tabButtons[tabIndex]->setVariant(
                tabIndex == static_cast<std::size_t>(m_tab) ? ButtonVariant::TabActive : ButtonVariant::Tab
            );
        if (m_surface)
          m_surface->requestLayout();
      });
      tabs->addChild(std::move(tab));
    }
    auto dragSpace = std::make_unique<Flex>();
    dragSpace->setFlexGrow(1.0F);
    tabs->addChild(std::move(dragSpace));
    auto headerClose = std::make_unique<Button>();
    headerClose->setText("×");
    headerClose->setVariant(ButtonVariant::Ghost);
    headerClose->setControlHeight(Style::controlHeightSm);
    headerClose->setTooltip("Close Properties");
    headerClose->setOnClick([this] { deferClose(); });
    tabs->addChild(std::move(headerClose));
    m_root->addChild(std::move(tabs));
    auto scroll = std::make_unique<ScrollView>();
    scroll->setFillHeight(true);
    scroll->setFlexGrow(1);
    m_body = scroll->content();
    m_body->setDirection(FlexDirection::Vertical);
    m_body->setAlign(FlexAlign::Stretch);
    m_body->setGap(12.0F);
    m_body->setPadding(8.0F, 2.0F);
    m_root->addChild(std::move(scroll));
    auto footerSeparator = std::make_unique<Separator>();
    m_root->addChild(std::move(footerSeparator));
    auto footer = std::make_unique<Flex>();
    footer->setDirection(FlexDirection::Horizontal);
    footer->setJustify(FlexJustify::End);
    footer->setAlign(FlexAlign::Center);
    footer->setGap(8.0F);
    auto footerStatus = std::make_unique<Label>();
    footerStatus->setFontSize(Style::fontSizeBody * 0.85F);
    footerStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    footerStatus->setFlexGrow(1.0F);
    footerStatus->setMaxLines(1);
    footerStatus->setEllipsize(TextEllipsize::End);
    m_footerStatus = footerStatus.get();
    footer->addChild(std::move(footerStatus));
    auto closeButton = std::make_unique<Button>();
    closeButton->setText("Close");
    closeButton->setControlHeight(Style::controlHeightSm);
    m_closeButton = closeButton.get();
    closeButton->setVariant(ButtonVariant::Outline);
    closeButton->setOnClick([this] { deferClose(m_discardRequested); });
    footer->addChild(std::move(closeButton));
    auto apply = std::make_unique<Button>();
    apply->setText("Apply");
    apply->setVariant(ButtonVariant::Primary);
    apply->setControlHeight(Style::controlHeightSm);
    apply->setOnClick([this] { applyPendingChanges(); });
    m_applyButton = apply.get();
    footer->addChild(std::move(apply));
    updateFooter();
    m_root->addChild(std::move(footer));
    const std::array<std::uint32_t, 8> edges{
        XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,      XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
        XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
        XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
    };
    const std::array<std::uint32_t, 8> cursors{
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
    };
    for (std::size_t i = 0; i < edges.size(); ++i) {
      auto edge = std::make_unique<InputArea>();
      edge->setParticipatesInLayout(false);
      edge->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      edge->setCursorShape(cursors[i]);
      edge->setOnPress([this, side = edges[i]](const InputArea::PointerData& data) {
        if (data.pressed && m_surface)
          xdg_toplevel_resize(m_surface->xdgToplevel(), m_wayland.seat(), data.serial, side);
      });
      m_resizeEdges[i] = edge.get();
      m_root->addChild(std::move(edge));
    }
    m_input.setTextInputContext(m_surface->wlSurface(), m_wayland.textInputService());
    m_input.setSceneRoot(m_root.get());
    m_input.setFocus(m_tabButtons[0]->inputArea());
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_wayland.setCursorShape(serial, shape);
    });
    m_input.setHoverChangeCallback([this](InputArea*, InputArea* next) {
      if (!m_surface)
        return;
      if (m_tooltipOwned)
        TooltipManager::instance().forceDestroy();
      m_tooltipOwned = next && next->hasTooltip();
      if (m_tooltipOwned)
        TooltipManager::instance().onHoverChange(
            next, m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
        );
    });
    m_surface->setSceneRoot(m_root.get());
    needsLayout = true;
  }
  if (m_contentDirty || m_renderedTab != m_tab)
    rebuildTab();
  if (needsLayout || m_root->layoutDirty()) {
    UiPhaseScope phase(UiPhase::Layout);
    m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    m_root->layout(renderer);
    const float w = static_cast<float>(m_surface->width()), h = static_cast<float>(m_surface->height());
    const std::array<LayoutRect, 8> bounds{
        {{10, 0, w - 20, 5},
         {10, h - 5, w - 20, 5},
         {0, 10, 5, h - 20},
         {w - 5, 10, 5, h - 20},
         {0, 0, 10, 10},
         {w - 10, 0, 10, 10},
         {0, h - 10, 10, 10},
         {w - 10, h - 10, 10, 10}}
    };
    for (std::size_t i = 0; i < bounds.size(); ++i) {
      m_resizeEdges[i]->setPosition(bounds[i].x, bounds[i].y);
      m_resizeEdges[i]->setSize(bounds[i].width, bounds[i].height);
      m_resizeEdges[i]->layout(renderer);
    }
  }
}

void PropertiesWindow::handleKeyboard(const KeyboardEvent& event) {
  if (m_dropdown && m_dropdown->isSelectDropdownOpen()) {
    m_dropdown->onKeyboardEvent(event);
    return;
  }
  if (!m_surface || !event.pressed || event.preedit || m_wayland.lastKeyboardSurface() != m_surface->wlSurface())
    return;
  if ((event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter)
      && m_nameInput
      && m_input.focusedArea() == m_nameInput->inputArea()) {
    confirmRename();
    return;
  }
  if (event.sym == XKB_KEY_Escape && !m_applying && m_nameInput && m_input.focusedArea() == m_nameInput->inputArea()) {
    m_nameDraft = m_path.filename().string();
    m_renameError.clear();
    m_contentDirty = true;
    m_discardRequested = false;
    m_nameInput->setValue(m_nameDraft);
    updateFooter();
    return;
  }
  if (event.sym == XKB_KEY_Escape) {
    if (m_discardRequested) {
      m_discardRequested = false;
      updateFooter();
      m_surface->requestLayout();
      return;
    }
    deferClose();
    return;
  }
  m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

void PropertiesWindow::handlePointer(const PointerEvent& event) {
  if (m_dropdown
      && m_dropdown->isSelectDropdownOpen()
      && event.surface != m_dropdown->wlSurface()
      && event.type == PointerEvent::Type::Button
      && event.pressed) {
    m_dropdown->closeSelectDropdown();
    return;
  }
  if (m_dropdown && m_dropdown->onPointerEvent(event))
    return;
  if (!m_surface || event.surface != m_surface->wlSurface())
    return;
  if (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)
    TooltipManager::instance().setPointerPosition(static_cast<float>(event.sx), static_cast<float>(event.sy));
  if ((event.type == PointerEvent::Type::Axis || event.type == PointerEvent::Type::Leave) && m_tooltipOwned) {
    TooltipManager::instance().forceDestroy();
    m_tooltipOwned = false;
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_input.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_input.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_input.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Button:
    if (event.pressed
        && event.button == BTN_LEFT
        && event.sy < 52.0
        && m_input.inputAreaAt(static_cast<float>(event.sx), static_cast<float>(event.sy)) == nullptr) {
      m_surface->beginMove(event.serial);
      break;
    }
    m_input.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
        event.time, false
    );
    break;
  case PointerEvent::Type::Axis:
    m_input.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines, event.axisGestureSerial
    );
    break;
  }
}
