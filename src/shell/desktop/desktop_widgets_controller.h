#pragma once

#include "config/config_service.h"

#include <memory>

class DesktopWidgetsEditor;
class DesktopWidgetsHost;
class HttpClient;
class IpcService;
class MprisService;
class PipeWireSpectrum;
class RenderContext;
class SystemMonitorService;
class WaylandConnection;
class WeatherService;
struct KeyboardEvent;
struct PointerEvent;

using DesktopWidgetsSnapshot = DesktopWidgetsConfig;

class DesktopWidgetsController {
public:
  DesktopWidgetsController();
  ~DesktopWidgetsController();

  DesktopWidgetsController(const DesktopWidgetsController&) = delete;
  DesktopWidgetsController& operator=(const DesktopWidgetsController&) = delete;

  void initialize(
      WaylandConnection& wayland, ConfigService* config, PipeWireSpectrum* pipewireSpectrum,
      const WeatherService* weather, RenderContext* renderContext, MprisService* mpris, HttpClient* httpClient,
      SystemMonitorService* sysmon
  );

  void registerIpc(IpcService& ipc);
  void onOutputChange();
  void onSecondTick();
  void requestLayout();
  void requestRedraw();

  void enterEdit();
  void exitEdit();
  void toggleEdit();

  [[nodiscard]] bool isEditing() const noexcept;
  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

private:
  void loadSnapshotFromConfig();
  void saveSnapshotToConfig();
  void applyVisibility();
  void handleConfigReload();
  void normalizeSnapshot();

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;

  DesktopWidgetsSnapshot m_snapshot;
  bool m_initialized = false;
  std::unique_ptr<DesktopWidgetsHost> m_host;
  std::unique_ptr<DesktopWidgetsEditor> m_editor;
};
