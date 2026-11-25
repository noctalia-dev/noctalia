#pragma once

#include <cstdint>
#include <optional>

// The runtime night-light temperature shared by plugins and the CLI, together with who set it. A
// plugin owner is identified by its script host id; the CLI sets it without an owner, so no plugin
// teardown can clear a temperature set from the command line.
class NightLightTemperatureOverride {
public:
  // Returns whether the temperature changed. A plugin setting the temperature already in effect
  // changes nothing, ownership included, so it can call this on a timer. The CLI setting it always
  // takes it over, so the plugin's teardown leaves it alone.
  bool set(std::optional<int> kelvin, std::optional<std::uint64_t> owner) {
    if (kelvin == m_kelvin) {
      if (!owner.has_value()) {
        m_owner.reset();
      }
      return false;
    }
    m_kelvin = kelvin;
    m_owner = kelvin.has_value() ? owner : std::nullopt;
    return true;
  }

  // Clears the temperature when `owner` set it and nothing has taken it over since. Returns whether
  // the temperature changed.
  bool release(std::uint64_t owner) {
    if (!m_kelvin.has_value() || m_owner != owner) {
      return false;
    }
    return reset();
  }

  // Clears the temperature whoever set it. Returns whether the temperature changed.
  bool reset() {
    const bool hadKelvin = m_kelvin.has_value();
    m_kelvin.reset();
    m_owner.reset();
    return hadKelvin;
  }

  std::optional<int> kelvin() const noexcept { return m_kelvin; }

private:
  std::optional<int> m_kelvin;
  std::optional<std::uint64_t> m_owner;
};
