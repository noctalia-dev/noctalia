#include "filemanager/properties_window.h"

#include "core/ui_phase.h"
#include "render/scene/input_area.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/dialogs/file_icon.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <grp.h>
#include <iomanip>
#include <pwd.h>
#include <sstream>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  struct QueryData {
    PropertiesWindow* window;
    std::weak_ptr<bool> alive;
    std::uint64_t generation;
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

  std::string timeText(guint64 seconds) {
    const auto value = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_r(&value, &local) == nullptr)
      return {};
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S %Z");
    return out.str();
  }

  std::string userName(guint32 uid) {
    const auto* entry = getpwuid(static_cast<uid_t>(uid));
    return entry ? entry->pw_name : "UID " + std::to_string(uid);
  }

  std::string groupName(guint32 gid) {
    const auto* entry = getgrgid(static_cast<gid_t>(gid));
    return entry ? entry->gr_name : "GID " + std::to_string(gid);
  }

  std::string fileType(GFileType type, bool symlink) {
    if (symlink)
      return "Symbolic link";
    switch (type) {
    case G_FILE_TYPE_DIRECTORY:
      return "Folder";
    case G_FILE_TYPE_REGULAR:
      return "Regular file";
    case G_FILE_TYPE_SPECIAL:
      return "Special file";
    case G_FILE_TYPE_SHORTCUT:
      return "Shortcut";
    case G_FILE_TYPE_MOUNTABLE:
      return "Mountable item";
    default:
      return "Unknown";
    }
  }

  GFile* fileForPath(const std::filesystem::path& path) {
    const auto text = path.string();
    return text.starts_with("trash:/") ? g_file_new_for_uri(text.c_str()) : g_file_new_for_path(text.c_str());
  }
} // namespace

PropertiesWindow::PropertiesWindow(WaylandConnection& wayland, RenderContext& renderer, ConfigService& config)
    : m_wayland(wayland), m_renderer(renderer), m_config(config) {}

PropertiesWindow::~PropertiesWindow() {
  *m_alive = false;
  close();
}

wl_surface* PropertiesWindow::wlSurface() const noexcept { return m_surface ? m_surface->wlSurface() : nullptr; }

bool PropertiesWindow::open(const std::filesystem::path& path, ToplevelSurface* parent) {
  if (m_surface) {
    m_wayland.activateSurface(m_surface->wlSurface());
    return true;
  }
  m_path = path;
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
  m_preferredWidth = 620;
  m_preferredHeight = 480;
  if (const auto saved = m_config.stateString("files", "properties_window_size")) {
    const auto separator = saved->find('x');
    if (separator != std::string::npos) {
      std::uint32_t width = 0, height = 0;
      const auto w = std::from_chars(saved->data(), saved->data() + separator, width);
      const auto h = std::from_chars(saved->data() + separator + 1, saved->data() + saved->size(), height);
      if (w.ec == std::errc{} && h.ec == std::errc{} && width >= 480 && height >= 380) {
        m_preferredWidth = width;
        m_preferredHeight = height;
      }
    }
  }
  m_surface = std::make_unique<ToplevelSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderer);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_surface->setClosedCallback([this, generation] {
    auto* pending = new QueryData{this, m_alive, generation};
    g_idle_add(
        [](gpointer data) -> gboolean {
          std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
          const auto alive = request->alive.lock();
          if (alive && *alive && request->generation == request->window->m_generation) {
            // The compositor callback runs inside the surface event handler; defer its destruction.
            request->window->close();
          }
          return G_SOURCE_REMOVE;
        },
        pending
    );
  });
  const auto* outputInfo = m_wayland.outputs().empty() ? nullptr : &m_wayland.outputs().front();
  wl_output* output = outputInfo ? outputInfo->output : nullptr;
  const auto outputWidth = outputInfo && outputInfo->logicalWidth > 0 ? outputInfo->logicalWidth : 620;
  const auto outputHeight = outputInfo && outputInfo->logicalHeight > 0 ? outputInfo->logicalHeight : 480;
  const auto minWidth = static_cast<std::uint32_t>(std::min(480, outputWidth));
  const auto minHeight = static_cast<std::uint32_t>(std::min(380, outputHeight));
  ToplevelSurfaceConfig config{
      .width = std::clamp(m_preferredWidth, minWidth, static_cast<std::uint32_t>(outputWidth)),
      .height = std::clamp(m_preferredHeight, minHeight, static_cast<std::uint32_t>(outputHeight)),
      .minWidth = minWidth,
      .minHeight = minHeight,
      .parent = parent ? parent->xdgToplevel() : nullptr,
      .title = "Properties — " + path.filename().string(),
      .appId = "dev.nythera.FileManager.Properties"
  };
  if (!m_surface->initialize(output, config)) {
    m_surface.reset();
    return false;
  }
  auto* file = fileForPath(path);
  auto* queryData = new QueryData{this, m_alive, generation};
  g_file_query_info_async(
      file,
      "standard::name,standard::display-name,standard::type,standard::is-symlink,standard::symlink-target,standard::"
      "icon,standard::"
      "content-type,standard::size,standard::is-hidden,standard::is-backup,unix::mode,unix::uid,unix::gid,unix::inode,"
      "unix::blocks,unix::device,unix::nlink,"
      "time::modified,time::access,time::"
      "created",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        GError* error = nullptr;
        GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
        const auto alive = request->alive.lock();
        if (alive && *alive && request->generation == request->window->m_generation)
          request->window->queryReady(info, error ? error->message : std::string{});
        if (info)
          g_object_unref(info);
        if (error)
          g_error_free(error);
      },
      queryData
  );
  g_object_unref(file);
  return true;
}

void PropertiesWindow::queryFilesystemMetadata() {
  if (!m_surface || !m_generation)
    return;
  const auto generation = m_generation;
  auto* file = fileForPath(m_path);
  auto* filesystemData = new QueryData{this, m_alive, generation};
  g_file_query_filesystem_info_async(
      file,
      G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
                                     "," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
      G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        GError* error = nullptr;
        GFileInfo* info = g_file_query_filesystem_info_finish(G_FILE(source), result, &error);
        const auto alive = request->alive.lock();
        if (alive && *alive && request->generation == request->window->m_generation)
          request->window->filesystemReady(info);
        if (info)
          g_object_unref(info);
        if (error)
          g_error_free(error);
      },
      filesystemData
  );
  g_object_unref(file);
  file = fileForPath(m_path);
  auto* mountData = new QueryData{this, m_alive, generation};
  g_file_find_enclosing_mount_async(
      file, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        GError* error = nullptr;
        GMount* mount = g_file_find_enclosing_mount_finish(G_FILE(source), result, &error);
        const auto alive = request->alive.lock();
        if (alive && *alive && request->generation == request->window->m_generation)
          request->window->mountReady(mount);
        if (mount)
          g_object_unref(mount);
        if (error)
          g_error_free(error);
      },
      mountData
  );
  g_object_unref(file);
}

void PropertiesWindow::queryReady(GFileInfo* info, const std::string& error) {
  m_loading = false;
  if (!info) {
    m_general = {{"Item status", {{"State", "Unavailable"}, {"Reason", error}}}};
  } else {
    const auto name = g_file_info_get_display_name(info);
    const bool link = g_file_info_get_is_symlink(info);
    const auto type = g_file_info_get_file_type(info);
    m_directory = type == G_FILE_TYPE_DIRECTORY;
    Section identity{"Identity", {}};
    identity.rows.emplace_back("Name", name ? name : m_path.filename().string());
    identity.rows.emplace_back("Type", fileType(type, link));
    identity.rows.emplace_back("Location", m_path.parent_path().string());
    if (link) {
      const auto target = g_file_info_get_symlink_target(info);
      if (target) {
        const std::filesystem::path targetPath(target);
        identity.rows.emplace_back(
            "Link target",
            (targetPath.is_absolute() ? targetPath : m_path.parent_path() / targetPath).lexically_normal().string()
        );
      }
    } else {
      identity.rows.emplace_back("Full path", m_path.string());
    }
    const auto mime = g_file_info_get_content_type(info);
    if (mime) {
      GAppInfo* defaultApp = g_app_info_get_default_for_type(mime, FALSE);
      if (defaultApp) {
        identity.rows.emplace_back("Open with", g_app_info_get_display_name(defaultApp));
        g_object_unref(defaultApp);
      }
    }
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
      size.rows.emplace_back("Folder size", "Not calculated recursively");
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
    const auto mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    const auto uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
    const auto gid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);
    m_permissions.clear();
    Section owner{"Owner", {}};
    owner.rows.emplace_back("User", hasUid ? userName(uid) + " (UID " + std::to_string(uid) + ")" : "Unavailable");
    const auto permissionSummary = [type](guint32 bits) {
      std::string result;
      if (bits & 4)
        result += "Read";
      if (bits & 2)
        result += result.empty() ? "Write" : "  Write";
      if (bits & 1)
        result += result.empty() ? (type == G_FILE_TYPE_DIRECTORY ? "Traverse" : "Execute")
                                 : (type == G_FILE_TYPE_DIRECTORY ? "  Traverse" : "  Execute");
      return result.empty() ? std::string("None") : result;
    };
    owner.rows.emplace_back("Permissions", hasMode ? permissionSummary((mode >> 6) & 7) : "Unavailable");
    Section group{"Group", {}};
    group.rows.emplace_back("Group", hasGid ? groupName(gid) + " (GID " + std::to_string(gid) + ")" : "Unavailable");
    group.rows.emplace_back("Permissions", hasMode ? permissionSummary((mode >> 3) & 7) : "Unavailable");
    Section others{"Others", {}};
    others.rows.emplace_back("Permissions", hasMode ? permissionSummary(mode & 7) : "Unavailable");
    m_permissions = {std::move(owner), std::move(group), std::move(others)};
    if (hasMode) {
      Section special{"Special permissions", {}};
      std::string bits;
      if (mode & 04000)
        bits += "Setuid  ";
      if (mode & 02000)
        bits += "Setgid  ";
      if (mode & 01000)
        bits += "Sticky";
      special.rows.emplace_back("Bits", bits.empty() ? "None" : bits);
      m_permissions.push_back(std::move(special));
    }

    Section technical{"File", {}};
    if (mime)
      technical.rows.emplace_back("MIME type", mime);
    const auto extension = m_path.extension().string();
    if (!extension.empty() && type != G_FILE_TYPE_DIRECTORY)
      technical.rows.emplace_back("Extension", extension);
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
    if (!m_filesystemId.empty())
      technical.rows.emplace_back("Filesystem ID", m_filesystemId);
    m_details = {std::move(technical)};
    if (m_icon) {
      m_icon->setFile(m_path, m_directory);
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
      m_icon->setThemedIconNames(std::move(iconNames));
    }
    if (!link)
      queryFilesystemMetadata();
  }
  if (!m_filesystem.rows.empty())
    m_general.push_back(m_filesystem);
  if (m_surface)
    m_surface->requestLayout();
  m_contentDirty = true;
}

void PropertiesWindow::filesystemReady(GFileInfo* info) {
  if (!info)
    return;
  Section storage{"Filesystem", {}};
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE)) {
    const auto type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
    if (type)
      storage.rows.emplace_back("Type", type);
  }
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM)) {
    const auto id = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
    if (id)
      m_filesystemId = id;
  }
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
    storage.rows.emplace_back(
        "Capacity", sizeText(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
    );
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    storage.rows.emplace_back(
        "Free space", sizeText(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    );
  if (!storage.rows.empty()) {
    for (const auto& row : m_filesystem.rows)
      if (row.first == "Mount point")
        storage.rows.push_back(row);
    m_filesystem = std::move(storage);
    const auto found = std::ranges::find(m_general, "Filesystem", &Section::title);
    if (found == m_general.end())
      m_general.push_back(m_filesystem);
    else
      *found = m_filesystem;
    m_contentDirty = true;
    if (m_surface)
      m_surface->requestLayout();
  }
  if (!m_filesystemId.empty()) {
    const auto details = std::ranges::find(m_details, "File", &Section::title);
    if (details != m_details.end()
        && std::ranges::none_of(details->rows, [](const auto& row) { return row.first == "Filesystem ID"; }))
      details->rows.emplace_back("Filesystem ID", m_filesystemId);
    m_contentDirty = true;
    if (m_surface)
      m_surface->requestLayout();
  }
}

void PropertiesWindow::mountReady(GMount* mount) {
  if (!mount)
    return;
  GFile* root = g_mount_get_root(mount);
  if (root) {
    gchar* path = g_file_get_path(root);
    gchar* uri = path ? nullptr : g_file_get_uri(root);
    m_filesystem.rows.emplace_back("Mount point", path ? path : (uri ? uri : "Unavailable"));
    g_free(path);
    g_free(uri);
    g_object_unref(root);
    const auto found = std::ranges::find(m_general, "Filesystem", &Section::title);
    if (found == m_general.end())
      m_general.push_back(m_filesystem);
    else
      *found = m_filesystem;
    m_contentDirty = true;
    if (m_surface)
      m_surface->requestLayout();
  }
}

void PropertiesWindow::close() {
  if (m_surface && m_surface->width() >= 480 && m_surface->height() >= 380) {
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
  m_body = nullptr;
  m_tabButtons.fill(nullptr);
  m_surface.reset();
}

void PropertiesWindow::deferClose() {
  auto* pending = new QueryData{this, m_alive, m_generation};
  g_idle_add(
      [](gpointer data) -> gboolean {
        std::unique_ptr<QueryData> request(static_cast<QueryData*>(data));
        const auto alive = request->alive.lock();
        if (alive && *alive && request->generation == request->window->m_generation)
          request->window->close();
        return G_SOURCE_REMOVE;
      },
      pending
  );
}

void PropertiesWindow::rebuildTab() {
  while (!m_body->children().empty())
    m_body->removeChild(m_body->children().back().get());
  const auto& lines = m_tab == 0 ? m_general : (m_tab == 1 ? m_permissions : m_details);
  if (m_loading) {
    auto label = std::make_unique<Label>();
    label->setText("Reading item metadata…");
    m_body->addChild(std::move(label));
  } else {
    for (const auto& sectionData : lines) {
      if (sectionData.rows.empty())
        continue;
      auto section = std::make_unique<Flex>();
      section->setDirection(FlexDirection::Vertical);
      section->setAlign(FlexAlign::Stretch);
      section->setGap(7.0F);
      section->setPadding(5.0F, 2.0F);

      auto title = std::make_unique<Label>();
      title->setText(sectionData.title);
      title->setFontWeight(FontWeight::SemiBold);
      title->setFontSize(Style::fontSizeBody * 0.92F);
      title->setColor(colorSpecFromRole(ColorRole::Primary));
      section->addChild(std::move(title));
      auto separator = std::make_unique<Separator>();
      section->addChild(std::move(separator));

      for (const auto& [name, value] : sectionData.rows) {
        auto row = std::make_unique<Flex>();
        row->setDirection(FlexDirection::Horizontal);
        row->setAlign(FlexAlign::Center);
        row->setGap(16.0F);
        auto key = std::make_unique<Label>();
        key->setText(name);
        key->setMinWidth(142.0F);
        key->setMaxWidth(142.0F);
        key->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        row->addChild(std::move(key));
        auto val = std::make_unique<Label>();
        val->setText(value.empty() ? "Unavailable" : value);
        val->setFlexGrow(1.0F);
        val->setMaxLines(1);
        val->setEllipsize(TextEllipsize::End);
        val->setTooltip(value);
        row->addChild(std::move(val));
        section->addChild(std::move(row));
      }
      m_body->addChild(std::move(section));
    }
  }
  m_renderedTab = m_tab;
  m_contentDirty = false;
}

void PropertiesWindow::prepareFrame(bool, bool needsLayout) {
  if (!m_surface || !m_surface->width() || !m_surface->height())
    return;
  m_renderer.makeCurrent(m_surface->renderTarget());
  auto& renderer = m_surface->renderTarget().renderer();
  if (!m_root) {
    m_root = std::make_unique<Flex>();
    m_root->setDirection(FlexDirection::Vertical);
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(10);
    m_root->setPadding(18);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.96F));
    m_root->clearBorder();
    auto heading = std::make_unique<Flex>();
    heading->setDirection(FlexDirection::Horizontal);
    heading->setAlign(FlexAlign::Center);
    heading->setGap(14.0F);
    auto icon = std::make_unique<FileIcon>();
    icon->setFile(m_path, m_directory);
    icon->setThemedIconNames({"text-x-generic"});
    icon->setGlyphSize(40.0F);
    m_icon = icon.get();
    heading->addChild(std::move(icon));
    auto title = std::make_unique<Label>();
    title->setText(m_path.filename().empty() ? m_path.string() : m_path.filename().string());
    title->setFontWeight(FontWeight::SemiBold);
    title->setFontSize(20);
    title->setFlexGrow(1.0F);
    title->setMaxLines(1);
    title->setEllipsize(TextEllipsize::End);
    title->setTooltip(m_path.string());
    heading->addChild(std::move(title));
    m_root->addChild(std::move(heading));
    auto tabs = std::make_unique<Flex>();
    tabs->setDirection(FlexDirection::Horizontal);
    tabs->setGap(8);
    const char* names[] = {"General", "Permissions", "Details"};
    for (int i = 0; i < 3; ++i) {
      auto tab = std::make_unique<Button>();
      tab->setText(names[i]);
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
    auto closeButton = std::make_unique<Button>();
    closeButton->setText("Close");
    closeButton->setVariant(ButtonVariant::Outline);
    closeButton->setOnClick([this] { deferClose(); });
    footer->addChild(std::move(closeButton));
    m_root->addChild(std::move(footer));
    m_input.setSceneRoot(m_root.get());
    m_input.setFocus(m_tabButtons[0]->inputArea());
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_wayland.setCursorShape(serial, shape);
    });
    m_input.setHoverChangeCallback([this](InputArea*, InputArea* next) {
      if (m_surface)
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
  }
}

void PropertiesWindow::handleKeyboard(const KeyboardEvent& event) {
  if (!m_surface || !event.pressed || event.preedit || m_wayland.lastKeyboardSurface() != m_surface->wlSurface())
    return;
  if (event.sym == XKB_KEY_Escape) {
    deferClose();
    return;
  }
  m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

void PropertiesWindow::handlePointer(const PointerEvent& event) {
  if (!m_surface || event.surface != m_surface->wlSurface())
    return;
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
