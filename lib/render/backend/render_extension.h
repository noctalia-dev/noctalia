#pragma once

// GPU state owned outside the backend, such as an app-specific shader program. The backend
// creates one instance per type on first use (RenderBackend::extension<T>()) and forwards its
// GPU lifecycle, so extensions follow the same teardown and graphics-reset rules as built-in programs.
class RenderExtension {
public:
  RenderExtension() = default;
  virtual ~RenderExtension() = default;

  RenderExtension(const RenderExtension&) = delete;
  RenderExtension& operator=(const RenderExtension&) = delete;

  // Releases GPU objects; the backend's context is current.
  virtual void destroy() = 0;
  // Forgets GPU objects of a lost context without issuing GL calls.
  virtual void abandon() noexcept = 0;
};
