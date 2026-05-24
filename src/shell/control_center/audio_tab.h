#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class Button;
class ContextMenuPopup;
class Flex;
class Label;
class MprisService;
class PipeWireService;
class RenderContext;
class Renderer;
class ScrollView;
class Slider;
class WaylandConnection;

class AudioTab : public Tab {
public:
  AudioTab(
      PipeWireService* audio, MprisService* mpris, ConfigService* config, WaylandConnection* wayland,
      RenderContext* renderContext
  );
  ~AudioTab() override;

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  bool dismissTransientUi() override;
  [[nodiscard]] bool dragging() const noexcept;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildLists(Renderer& renderer);
  void rebuildProgramVolumes(Renderer& renderer);
  void syncValueLabelWidths(Renderer& renderer);
  void syncProgramVolumeRows();
  void queueProgramSinkVolume(std::uint32_t id, float value);
  void flushPendingProgramVolumes(bool force = false);
  [[nodiscard]] float sliderMaxPercent() const;
  void queueSinkVolume(float value);
  void queueSourceVolume(float value);
  void flushPendingVolumes(bool force = false);

  void openDeviceMenu(bool isOutput);

  PipeWireService* m_audio = nullptr;
  MprisService* m_mpris = nullptr;
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_deviceColumn = nullptr;
  Flex* m_outputCard = nullptr;
  Flex* m_inputCard = nullptr;
  ScrollView* m_outputScroll = nullptr;
  ScrollView* m_inputScroll = nullptr;
  Flex* m_outputList = nullptr;
  Flex* m_inputList = nullptr;
  Flex* m_volumeColumn = nullptr;
  Flex* m_outputVolumeCard = nullptr;
  Flex* m_inputVolumeCard = nullptr;
  Flex* m_programCard = nullptr;
  ScrollView* m_programScroll = nullptr;
  Flex* m_programList = nullptr;
  std::vector<Flex*> m_programRows;
  std::string m_lastProgramListKey;
  float m_lastProgramSliderMax = -1.0f;
  float m_syncedPercentLabelMinWidth = -1.0f;
  float m_lastSyncedPercentLabelSliderMax = -1.0f;
  Label* m_outputDeviceLabel = nullptr;
  Label* m_inputDeviceLabel = nullptr;
  Flex* m_outputDeviceMenuAnchor = nullptr;
  Flex* m_inputDeviceMenuAnchor = nullptr;
  Button* m_outputDeviceMenuButton = nullptr;
  Button* m_inputDeviceMenuButton = nullptr;
  std::unique_ptr<ContextMenuPopup> m_deviceMenuPopup;
  bool m_deviceMenuIsOutput = true;
  Slider* m_outputSlider = nullptr;
  Label* m_outputValue = nullptr;
  Button* m_outputMuteButton = nullptr;
  Slider* m_inputSlider = nullptr;
  Label* m_inputValue = nullptr;
  Button* m_inputMuteButton = nullptr;

  float m_lastOutputWidth = -1.0f;
  float m_lastInputWidth = -1.0f;
  std::string m_lastOutputListKey;
  std::string m_lastInputListKey;
  float m_lastSinkVolume = -1.0f;
  float m_lastSourceVolume = -1.0f;
  std::uint32_t m_pendingSinkId = 0;
  std::uint32_t m_pendingSourceId = 0;
  float m_pendingSinkVolume = -1.0f;
  float m_pendingSourceVolume = -1.0f;
  float m_lastSentSinkVolume = -1.0f;
  float m_lastSentSourceVolume = -1.0f;
  std::chrono::steady_clock::time_point m_lastSinkCommitAt{};
  std::chrono::steady_clock::time_point m_lastSourceCommitAt{};
  std::chrono::steady_clock::time_point m_ignoreSinkStateUntil{};
  std::chrono::steady_clock::time_point m_ignoreSourceStateUntil{};
  Timer m_sinkVolumeDebounceTimer;
  Timer m_sourceVolumeDebounceTimer;
  Timer m_programSinkDebounceTimer;
  std::uint32_t m_pendingProgramSinkId = 0;
  float m_pendingProgramSinkVolume = -1.0f;
  bool m_syncingOutputSlider = false;
  bool m_syncingInputSlider = false;
};
