#pragma once

#include "config/config_service.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Flex;
class Button;
class Label;
class Node;
class FileIcon;
class WaylandConnection;

class PropertiesWindow {
public:
  PropertiesWindow(WaylandConnection& wayland, RenderContext& renderer, ConfigService& config);
  ~PropertiesWindow();
  bool open(const std::filesystem::path& path, ToplevelSurface* parent);
  void close();
  void prepareFrame(bool update, bool layout);
  void handleKeyboard(const KeyboardEvent& event);
  void handlePointer(const PointerEvent& event);
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
  [[nodiscard]] wl_surface* wlSurface() const noexcept;

private:
  struct Section {
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows;
  };
  void rebuildTab();
  void deferClose();
  void queryReady(GFileInfo* info, const std::string& error);
  void queryFilesystemMetadata();
  void filesystemReady(GFileInfo* info);
  void mountReady(GMount* mount);
  WaylandConnection& m_wayland;
  RenderContext& m_renderer;
  ConfigService& m_config;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Flex> m_root;
  Flex* m_body = nullptr;
  FileIcon* m_icon = nullptr;
  std::array<Button*, 3> m_tabButtons{};
  InputDispatcher m_input;
  std::filesystem::path m_path;
  std::vector<Section> m_general;
  std::vector<Section> m_permissions;
  std::vector<Section> m_details;
  Section m_filesystem;
  std::string m_filesystemId;
  int m_tab = 0;
  int m_renderedTab = -1;
  bool m_contentDirty = true;
  bool m_loading = true;
  bool m_directory = false;
  std::uint32_t m_preferredWidth = 620;
  std::uint32_t m_preferredHeight = 480;
  std::uint64_t m_generation = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
