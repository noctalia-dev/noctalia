#pragma once

#include <cstdint>
#include <memory>

struct wl_surface;

class RenderBackend;
class RenderContext;
class RenderSurfaceTarget;

class RenderTarget {
public:
  RenderTarget();
  ~RenderTarget();

  RenderTarget(const RenderTarget&) = delete;
  RenderTarget& operator=(const RenderTarget&) = delete;

  void create(wl_surface* surface, RenderContext& context);
  void create(wl_surface* surface, RenderBackend& backend);
  void resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight);
  void destroy();

  [[nodiscard]] std::uint32_t bufferWidth() const noexcept { return m_bufferWidth; }
  [[nodiscard]] std::uint32_t bufferHeight() const noexcept { return m_bufferHeight; }
  [[nodiscard]] std::uint32_t logicalWidth() const noexcept { return m_logicalWidth; }
  [[nodiscard]] std::uint32_t logicalHeight() const noexcept { return m_logicalHeight; }
  [[nodiscard]] bool isReady() const noexcept;
  [[nodiscard]] RenderSurfaceTarget* surfaceTarget() noexcept { return m_surfaceTarget.get(); }
  [[nodiscard]] const RenderSurfaceTarget* surfaceTarget() const noexcept { return m_surfaceTarget.get(); }

  void setLogicalSize(std::uint32_t w, std::uint32_t h) noexcept {
    m_logicalWidth = w;
    m_logicalHeight = h;
  }

private:
  std::unique_ptr<RenderSurfaceTarget> m_surfaceTarget;
  std::uint32_t m_bufferWidth = 0;
  std::uint32_t m_bufferHeight = 0;
  std::uint32_t m_logicalWidth = 0;
  std::uint32_t m_logicalHeight = 0;
};
