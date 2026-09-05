#pragma once

#include "app/poll_source.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <vector>

// Sets volume/mute for device sinks/sources through WirePlumber's mixer-api plugin
// (the same path `wpctl` uses internally), so pipewire-pulse / pavucontrol stay in sync
// without forking an external process or racing writes. Owns a private WpCore on a
// dedicated GMainContext, bridged into the poll loop as a PollSource.
class WirePlumberMixer final : public PollSource {
public:
  WirePlumberMixer();
  ~WirePlumberMixer() override;

  WirePlumberMixer(const WirePlumberMixer&) = delete;
  WirePlumberMixer& operator=(const WirePlumberMixer&) = delete;

  // True once connected and the mixer-api plugin is active. Writes made before this are queued
  // and flushed on activation, so callers need not gate on it.
  [[nodiscard]] bool ready() const noexcept;

  // Perceptual volume in [0, 1.5] (cubic scale), applied to a node by global id.
  void setVolume(std::uint32_t id, float volume);
  void setMuted(std::uint32_t id, bool muted);

  // Proactively fetch the current volume/mute for a single device node that appeared
  // after mixer activation (e.g. Bluetooth earbuds). No-ops if not ready.
  void refreshVolume(std::uint32_t id);

  // Authoritative read side: fires with a node's current perceptual volume + mute whenever mixer-api
  // reports a change (including our own writes and external changes from pavucontrol/pulse), plus once
  // per device node right after activation. This is the single source of truth for device volume/mute.
  using ChangeCallback = std::function<void(std::uint32_t id, float volume, bool muted)>;
  void setChangeCallback(ChangeCallback callback);

  // Selects the configured default device by node id, mirroring `wpctl set-default`: looks the node
  // up in our own WpCore, reads its media.class + node.name, and drives default-nodes-api. WirePlumber
  // applies it live and persists it across reboots.
  void setDefaultNode(std::uint32_t id);

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
