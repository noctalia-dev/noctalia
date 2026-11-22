#pragma once

#include "app/poll_source.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

struct wl_compositor;
struct wl_shm;
struct wl_data_device_manager;
struct wl_seat;
struct wl_surface;

// Application-client transport. MIME representations share an owner, not bytes.
class NativeDataTransfer final : public PollSource {
public:
  struct DragIconImage {
    int width = 0;
    int height = 0;
    int scale = 1;
    std::vector<std::uint32_t> pixels; // premultiplied ARGB8888, tightly packed
  };
  using Payload = std::map<std::string, std::string>;
  using ReadCallback = std::function<void(std::optional<Payload>)>;
  using FinishDrop = std::function<void(bool)>;
  using DropReceiver = std::function<void(std::optional<Payload>, FinishDrop)>;
  // Motion returns one supported action, or zero to reject the target.
  using DragMotion = std::function<std::uint32_t(wl_surface*, float, float, std::uint32_t)>;
  // Called at physical drop, allowing the recipient to capture its destination.
  // The receiver must call FinishDrop exactly once after success/failure; returning
  // an empty receiver rejects the drop without requesting any data.
  using DropHandler = std::function<DropReceiver(std::uint32_t)>;
  NativeDataTransfer();
  ~NativeDataTransfer() override;
  NativeDataTransfer(const NativeDataTransfer&) = delete;
  NativeDataTransfer& operator=(const NativeDataTransfer&) = delete;
  void bind(wl_data_device_manager* manager, wl_seat* seat);
  void cleanup();
  [[nodiscard]] bool available() const;
  [[nodiscard]] bool ownsSelection() const;
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::uint64_t selectionGeneration() const;
  bool setSelection(Payload payload, std::uint32_t serial);
  void readSelection(ReadCallback callback, bool readAllFileFormats = false);
  void readTextSelection(std::function<void(std::optional<std::string>)> callback);
  void consumeSelection(std::uint64_t generation);
  void setSelectionChangedCallback(std::function<void()> callback);
  [[nodiscard]] const Payload* ownedSelectionPayload() const noexcept;
  void updateOwnedSelection(Payload payload);
  [[nodiscard]] bool selectionOffersText() const noexcept;
  [[nodiscard]] bool selectionOffersFileCut() const noexcept;
  void setDragIconResources(wl_compositor* compositor, wl_shm* shm);
  // An external icon is borrowed; its host retains it until physical drop or cancellation.
  bool startDrag(
      Payload payload, wl_surface* origin, std::uint32_t pressSerial, const DragIconImage* icon = nullptr,
      wl_surface* externalIcon = nullptr
  );
  void cancelDrag();
  [[nodiscard]] bool dragging() const;
  [[nodiscard]] bool dragDropped() const;
  [[nodiscard]] bool dragIconVisible() const;
  [[nodiscard]] std::uint32_t dragAction() const;
  void setDragHandlers(
      DragMotion motion, std::function<void()> leave, DropHandler drop, std::function<void(bool)> finished,
      std::function<void()> sourceChanged = {}
  );
  void updateDragTarget();
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;
  [[nodiscard]] int pollTimeoutMs() const override;

private:
  void doAddPollFds(std::vector<pollfd>& fds) override;
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
