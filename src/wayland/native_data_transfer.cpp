#include "wayland/native_data_transfer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <wayland-client.h>

namespace {
  constexpr std::size_t kMaximumPayload = 16 * 1024 * 1024;
  constexpr auto kTransferTimeout = std::chrono::seconds(10);
  // A target can close a pipe at any time. Do not change process-wide signal policy.
  ssize_t writePipe(int fd, const char* data, std::size_t size) {
    sigset_t mask, previous, pending;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &mask, &previous) != 0)
      return -1;
    sigpending(&pending);
    const bool alreadyPending = sigismember(&pending, SIGPIPE) == 1;
    const auto result = write(fd, data, size);
    const int savedError = errno;
    if (result < 0 && savedError == EPIPE && !alreadyPending) {
      timespec timeout{};
      while (sigtimedwait(&mask, nullptr, &timeout) < 0 && errno == EINTR) {
      }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = savedError;
    return result;
  }
} // namespace

struct NativeDataTransfer::Impl {
  struct Offer {
    wl_data_offer* proxy;
    std::vector<std::string> mimes;
    std::uint32_t sourceActions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    std::uint32_t action = 0;
    Impl* owner = nullptr;
    bool assigned = false;
    ~Offer() {
      if (proxy)
        wl_data_offer_destroy(proxy);
    }
  };
  struct DragIcon {
    wl_surface* surface = nullptr;
    wl_buffer* buffer = nullptr;
    ~DragIcon() {
      if (surface)
        wl_surface_destroy(surface);
      if (buffer)
        wl_buffer_destroy(buffer);
    }
  };
  struct Source {
    Impl* owner;
    wl_data_source* proxy;
    Payload payload;
    std::uint32_t action = 0;
    bool drag = false;
    std::unique_ptr<DragIcon> icon;
    wl_surface* externalIcon = nullptr;
    ~Source() { wl_data_source_destroy(proxy); }
  };
  struct Read {
    int fd;
    std::string mime;
    std::string data;
  };
  struct Request {
    std::shared_ptr<Offer> offer;
    std::uint64_t generation;
    std::vector<Read> reads;
    ReadCallback callback;
    bool failed = false;
    bool selectionRequest = true;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kTransferTimeout;
  };
  struct Write {
    int fd;
    std::string data;
    std::size_t offset = 0;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kTransferTimeout;
  };
  wl_compositor* iconCompositor = nullptr; // connection-owned
  wl_shm* iconShm = nullptr;
  wl_data_device_manager* manager = nullptr; // connection-owned
  wl_data_device* device = nullptr;
  std::map<wl_data_offer*, std::shared_ptr<Offer>> offers;
  std::shared_ptr<Offer> selection;
  std::shared_ptr<Offer> dragOffer;
  wl_surface* dragSurface = nullptr;
  float dragX = 0, dragY = 0;
  std::uint32_t dragSerial = 0;
  std::uint32_t preferredAction = 0;
  bool dropStarted = false;
  wl_data_source* dragSource = nullptr;
  bool dragReleased = false;
  DragMotion onMotion;
  std::function<void()> onLeave;
  DropHandler onDrop;
  std::function<void(bool)> onFinished;
  std::function<void()> onSourceChanged;
  std::shared_ptr<int> alive = std::make_shared<int>(0);
  std::vector<std::unique_ptr<Source>> sources;
  wl_data_source* ownedSelection = nullptr;
  bool selectionPending = false;
  std::uint64_t generation = 0;
  std::vector<Request> requests;
  std::vector<Write> writes;

  static const wl_data_source_listener sourceListener;
  static const wl_data_offer_listener offerListener;
  static const wl_data_device_listener deviceListener;

  std::unique_ptr<DragIcon> createIcon(const DragIconImage* image) {
    if (!image
        || !iconCompositor
        || !iconShm
        || image->width <= 0
        || image->height <= 0
        || image->width > 2048
        || image->height > 1024
        || image->scale < 1
        || image->scale > 4
        || image->width % image->scale
        || image->height % image->scale
        || image->pixels.size() != static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height))
      return {};
    const auto bytes = image->pixels.size() * sizeof(std::uint32_t);
    const int fd = memfd_create("nythera-file-drag-icon", MFD_CLOEXEC);
    if (fd < 0)
      return {};
    if (ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
      close(fd);
      return {};
    }
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      close(fd);
      return {};
    }
    std::memcpy(mapping, image->pixels.data(), bytes);
    munmap(mapping, bytes);
    auto icon = std::make_unique<DragIcon>();
    wl_shm_pool* pool = wl_shm_create_pool(iconShm, fd, static_cast<std::int32_t>(bytes));
    close(fd);
    if (!pool)
      return {};
    icon->buffer =
        wl_shm_pool_create_buffer(pool, 0, image->width, image->height, image->width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!icon->buffer)
      return {};
    icon->surface = wl_compositor_create_surface(iconCompositor);
    if (!icon->surface)
      return {};
    if (image->scale > 1) {
      if (wl_surface_get_version(icon->surface) < WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        return {};
      wl_surface_set_buffer_scale(icon->surface, image->scale);
    }
    if (auto* region = wl_compositor_create_region(iconCompositor)) {
      wl_surface_set_input_region(icon->surface, region);
      wl_region_destroy(region);
    }
    return icon;
  }
  void pruneOffers() {
    std::erase_if(offers, [this](const auto& item) {
      return item.second->assigned
          && item.second != selection
          && item.second != dragOffer
          && item.second.use_count() == 1;
    });
  }
  void receive(Request& request, const std::string& mime) {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) {
      request.failed = true;
      return;
    }
    const int flags = fcntl(pipes[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) < 0) {
      close(pipes[0]);
      close(pipes[1]);
      request.failed = true;
      return;
    }
    wl_data_offer_receive(request.offer->proxy, mime.c_str(), pipes[1]);
    close(pipes[1]);
    request.reads.push_back({pipes[0], mime, {}});
  }
  void finishSource(wl_data_source* proxy, bool success) {
    const bool drag = dragSource == proxy;
    if (ownedSelection == proxy) {
      ownedSelection = nullptr;
      ++generation;
    }
    if (drag)
      dragSource = nullptr;
    std::erase_if(sources, [proxy](const auto& source) { return source->proxy == proxy; });
    if (drag && onFinished)
      onFinished(success);
  }
  void updateTarget() {
    if (!dragOffer || !dragOffer->proxy || dropStarted)
      return;
    const auto& mimes = dragOffer->mimes;
    const bool uri = std::ranges::find(mimes, "text/uri-list") != mimes.end();
    std::uint32_t actions = dragOffer->sourceActions;
    if (wl_data_offer_get_version(dragOffer->proxy) < 3)
      actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    auto action = uri && onMotion ? onMotion(dragSurface, dragX, dragY, actions) : 0U;
    if ((action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY && action != WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
        || !(actions & action))
      action = 0;
    preferredAction = action;
    wl_data_offer_accept(dragOffer->proxy, dragSerial, action ? "text/uri-list" : nullptr);
    if (wl_data_offer_get_version(dragOffer->proxy) >= 3)
      wl_data_offer_set_actions(dragOffer->proxy, action, action);
  }
  void finishOffer(const std::shared_ptr<Offer>& offer, bool success) {
    if (!offer->proxy)
      return;
    auto* proxy = offer->proxy;
    if (success && wl_data_offer_get_version(proxy) >= 3)
      wl_data_offer_finish(proxy);
    wl_data_offer_destroy(proxy);
    offer->proxy = nullptr;
    if (dragOffer == offer) {
      dragOffer.reset();
      dragSurface = nullptr;
      preferredAction = 0;
      if (onLeave)
        onLeave();
    }
    offers.erase(proxy);
  }
  void drop() {
    auto offer = dragOffer;
    if (!offer || dropStarted)
      return;
    dropStarted = true;
    const auto action = wl_data_offer_get_version(offer->proxy) < 3
        ? static_cast<std::uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
        : offer->action;
    if (!preferredAction
        || !onDrop
        || (action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY && action != WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)) {
      finishOffer(offer, false);
      return;
    }
    auto receiver = onDrop(action);
    if (!receiver) {
      finishOffer(offer, false);
      return;
    }
    const std::weak_ptr<int> lifetime = alive;
    Request request{
        offer,
        generation,
        {},
        [this, receiver = std::move(receiver), offer, lifetime](std::optional<Payload> payload) mutable {
          receiver(std::move(payload), [this, offer, lifetime](bool success) {
            if (!lifetime.expired())
              finishOffer(offer, success);
          });
        }
    };
    request.selectionRequest = false;
    receive(request, "text/uri-list");
    requests.push_back(std::move(request));
  }
  void cleanup() {
    // Destruction never calls consumers whose own state may already be gone.
    for (auto& request : requests)
      for (auto& read : request.reads)
        if (read.fd >= 0)
          close(read.fd);
    requests.clear();
    for (auto& write : writes)
      close(write.fd);
    writes.clear();
    alive.reset();
    sources.clear();
    ownedSelection = nullptr;
    dragSource = nullptr;
    dragOffer.reset();
    dragSurface = nullptr;
    selection.reset();
    offers.clear();
    if (device) {
      if (wl_data_device_get_version(device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(device);
      else
        wl_data_device_destroy(device);
    }
    device = nullptr;
    manager = nullptr;
  }
};

const wl_data_source_listener NativeDataTransfer::Impl::sourceListener = {
    .target = [](void*, wl_data_source*, const char*) {},
    .send =
        [](void* data, wl_data_source*, const char* mime, int fd) {
          auto& source = *static_cast<Source*>(data);
          const auto it = source.payload.find(mime ? mime : "");
          const int flags = fcntl(fd, F_GETFL, 0);
          if (it == source.payload.end() || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            return;
          }
          source.owner->writes.push_back({fd, it->second});
        },
    .cancelled = [](void* data,
                    wl_data_source* proxy) { static_cast<Source*>(data)->owner->finishSource(proxy, false); },
    .dnd_drop_performed =
        [](void* data, wl_data_source* proxy) {
          auto* source = static_cast<Source*>(data);
          auto* owner = source->owner;
          source->icon.reset();
          if (owner->dragSource == proxy) {
            owner->dragReleased = true;
            if (owner->onSourceChanged)
              owner->onSourceChanged();
          }
        },
    .dnd_finished =
        [](void* data, wl_data_source* proxy) {
          auto* source = static_cast<Source*>(data);
          source->owner->finishSource(proxy, source->action != 0);
        },
    .action =
        [](void* data, wl_data_source* proxy, std::uint32_t action) {
          auto* source = static_cast<Source*>(data);
          source->action = action;
          if (source->owner->dragSource == proxy && source->owner->onSourceChanged)
            source->owner->onSourceChanged();
        },
};
const wl_data_offer_listener NativeDataTransfer::Impl::offerListener = {
    .offer =
        [](void* data, wl_data_offer*, const char* mime) {
          if (mime)
            static_cast<Offer*>(data)->mimes.emplace_back(mime);
        },
    .source_actions =
        [](void* data, wl_data_offer*, std::uint32_t actions) {
          auto* offer = static_cast<Offer*>(data);
          offer->sourceActions = actions;
          if (offer->owner)
            offer->owner->updateTarget();
        },
    .action = [](void* data, wl_data_offer*, std::uint32_t action) { static_cast<Offer*>(data)->action = action; },
};
const wl_data_device_listener NativeDataTransfer::Impl::deviceListener = {
    .data_offer =
        [](void* data, wl_data_device*, wl_data_offer* proxy) {
          auto& self = *static_cast<Impl*>(data);
          auto offer = std::make_shared<Offer>(proxy, std::vector<std::string>{});
          offer->owner = &self;
          wl_data_offer_add_listener(proxy, &offerListener, offer.get());
          self.offers.emplace(proxy, std::move(offer));
        },
    .enter =
        [](void* data, wl_data_device*, std::uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
           wl_data_offer* offer) {
          auto& self = *static_cast<Impl*>(data);
          self.dropStarted = false;
          self.dragOffer = self.offers.contains(offer) ? self.offers.at(offer) : nullptr;
          if (self.dragOffer)
            self.dragOffer->assigned = true;
          self.dragSurface = surface;
          self.dragSerial = serial;
          self.dragX = static_cast<float>(wl_fixed_to_double(x));
          self.dragY = static_cast<float>(wl_fixed_to_double(y));
          self.updateTarget();
        },
    .leave =
        [](void* data, wl_data_device*) {
          auto& self = *static_cast<Impl*>(data);
          self.dragOffer.reset();
          self.dragSurface = nullptr;
          self.preferredAction = 0;
          if (self.onLeave)
            self.onLeave();
          self.pruneOffers();
        },
    .motion =
        [](void* data, wl_data_device*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
          auto& self = *static_cast<Impl*>(data);
          self.dragX = static_cast<float>(wl_fixed_to_double(x));
          self.dragY = static_cast<float>(wl_fixed_to_double(y));
          self.updateTarget();
        },
    .drop = [](void* data, wl_data_device*) { static_cast<Impl*>(data)->drop(); },
    .selection =
        [](void* data, wl_data_device*, wl_data_offer* proxy) {
          auto& self = *static_cast<Impl*>(data);
          // Incoming offers are authoritative even when old-source cancellation
          // arrives later. Local snapshots bridge only the publish/echo gap.
          self.selectionPending = false;
          ++self.generation;
          self.selection = self.offers.contains(proxy) ? self.offers.at(proxy) : nullptr;
          if (self.selection)
            self.selection->assigned = true;
          self.pruneOffers();
        },
};

NativeDataTransfer::NativeDataTransfer() : m_impl(std::make_unique<Impl>()) {}
NativeDataTransfer::~NativeDataTransfer() { cleanup(); }
void NativeDataTransfer::cleanup() { m_impl->cleanup(); }
void NativeDataTransfer::bind(wl_data_device_manager* manager, wl_seat* seat) {
  cleanup();
  m_impl->alive = std::make_shared<int>(0);
  m_impl->manager = manager;
  if (manager && seat) {
    m_impl->device = wl_data_device_manager_get_data_device(manager, seat);
    wl_data_device_add_listener(m_impl->device, &Impl::deviceListener, m_impl.get());
  }
}
bool NativeDataTransfer::available() const { return m_impl->device != nullptr; }
bool NativeDataTransfer::ownsSelection() const { return m_impl->ownedSelection != nullptr; }
bool NativeDataTransfer::busy() const {
  return std::ranges::any_of(m_impl->sources, [](const auto& source) { return source->drag; })
      || !m_impl->writes.empty()
      || !m_impl->requests.empty();
}
std::uint64_t NativeDataTransfer::selectionGeneration() const { return m_impl->generation; }
bool NativeDataTransfer::setSelection(Payload payload, std::uint32_t serial) {
  if (!available() || !serial || payload.empty())
    return false;
  auto source = std::make_unique<Impl::Source>(
      m_impl.get(), wl_data_device_manager_create_data_source(m_impl->manager), std::move(payload)
  );
  wl_data_source_add_listener(source->proxy, &Impl::sourceListener, source.get());
  for (const auto& [mime, bytes] : source->payload)
    wl_data_source_offer(source->proxy, mime.c_str());
  m_impl->ownedSelection = source->proxy;
  m_impl->selectionPending = true;
  wl_data_device_set_selection(m_impl->device, source->proxy, serial);
  m_impl->sources.push_back(std::move(source));
  ++m_impl->generation;
  return true;
}
void NativeDataTransfer::consumeSelection(std::uint64_t generation) {
  if (generation != m_impl->generation)
    return;
  for (auto& source : m_impl->sources)
    if (source->proxy == m_impl->ownedSelection)
      source->payload = {
          {"text/uri-list", ""}, {"x-special/gnome-copied-files", "copy\n"}, {"application/x-kde-cutselection", "0"}
      };
}
bool NativeDataTransfer::dragDropped() const { return dragging() && m_impl->dragReleased; }
bool NativeDataTransfer::dragIconVisible() const {
  if (dragDropped())
    return false;
  return std::ranges::any_of(m_impl->sources, [this](const auto& source) {
    return source->proxy == m_impl->dragSource && (source->icon != nullptr || source->externalIcon != nullptr);
  });
}
std::uint32_t NativeDataTransfer::dragAction() const {
  for (const auto& source : m_impl->sources)
    if (source->proxy == m_impl->dragSource)
      return source->action;
  return 0;
}
bool NativeDataTransfer::dragging() const { return m_impl->dragSource != nullptr; }
void NativeDataTransfer::setDragHandlers(
    DragMotion motion, std::function<void()> leave, DropHandler drop, std::function<void(bool)> finished,
    std::function<void()> sourceChanged
) {
  m_impl->onMotion = std::move(motion);
  m_impl->onLeave = std::move(leave);
  m_impl->onDrop = std::move(drop);
  m_impl->onFinished = std::move(finished);
  m_impl->onSourceChanged = std::move(sourceChanged);
}
void NativeDataTransfer::updateDragTarget() { m_impl->updateTarget(); }
void NativeDataTransfer::setDragIconResources(wl_compositor* compositor, wl_shm* shm) {
  m_impl->iconCompositor = compositor;
  m_impl->iconShm = shm;
}
bool NativeDataTransfer::startDrag(
    Payload payload, wl_surface* origin, std::uint32_t pressSerial, const DragIconImage* icon, wl_surface* externalIcon
) {
  if (!available()
      || (dragging() && !dragDropped())
      || !origin
      || !pressSerial
      || !payload.contains("text/uri-list")
      || payload.at("text/uri-list").empty()
      || wl_data_device_manager_get_version(m_impl->manager) < 3)
    return false;
  auto source = std::make_unique<Impl::Source>(
      m_impl.get(), wl_data_device_manager_create_data_source(m_impl->manager), std::move(payload)
  );
  wl_data_source_add_listener(source->proxy, &Impl::sourceListener, source.get());
  for (const auto& [mime, bytes] : source->payload)
    wl_data_source_offer(source->proxy, mime.c_str());
  source->drag = true;
  source->externalIcon = externalIcon;
  if (!externalIcon)
    source->icon = m_impl->createIcon(icon);
  wl_data_source_set_actions(
      source->proxy, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
  );
  m_impl->dragReleased = false;
  m_impl->dragSource = source->proxy;
  wl_data_device_start_drag(
      m_impl->device, source->proxy, origin,
      externalIcon       ? externalIcon
          : source->icon ? source->icon->surface
                         : nullptr,
      pressSerial
  );
  if (externalIcon && wl_surface_get_version(externalIcon) >= WL_SURFACE_OFFSET_SINCE_VERSION)
    wl_surface_offset(externalIcon, 18, 20);
  if (source->icon) {
    // start_drag assigns the icon role before its first buffer commit.
    const bool hasOffset = wl_surface_get_version(source->icon->surface) >= WL_SURFACE_OFFSET_SINCE_VERSION;
    if (hasOffset)
      wl_surface_offset(source->icon->surface, 18, 20);
    wl_surface_attach(source->icon->surface, source->icon->buffer, hasOffset ? 0 : 18, hasOffset ? 0 : 20);
    wl_surface_damage(source->icon->surface, 0, 0, icon->width / icon->scale, icon->height / icon->scale);
    wl_surface_commit(source->icon->surface);
  }
  m_impl->sources.push_back(std::move(source));
  return true;
}
void NativeDataTransfer::cancelDrag() {
  if (m_impl->dragSource)
    m_impl->finishSource(m_impl->dragSource, false);
}
void NativeDataTransfer::readSelection(ReadCallback callback) {
  auto& self = *m_impl;
  // Ctrl+V can arrive before the compositor echoes our Ctrl+C selection.
  for (const auto& source : self.sources)
    if (self.selectionPending && source->proxy == self.ownedSelection) {
      callback(source->payload);
      return;
    }
  if (!self.selection) {
    callback(std::nullopt);
    return;
  }
  Impl::Request request{self.selection, self.generation, {}, std::move(callback)};
  const auto has = [&](const char* mime) {
    return std::ranges::find(request.offer->mimes, mime) != request.offer->mimes.end();
  };
  if (has("text/uri-list")) {
    self.receive(request, "text/uri-list");
    if (has("application/x-kde-cutselection"))
      self.receive(request, "application/x-kde-cutselection");
    else if (has("x-special/gnome-copied-files"))
      self.receive(request, "x-special/gnome-copied-files");
  } else if (has("x-special/gnome-copied-files"))
    self.receive(request, "x-special/gnome-copied-files");
  else {
    request.callback(std::nullopt);
    return;
  }
  self.requests.push_back(std::move(request));
}
void NativeDataTransfer::doAddPollFds(std::vector<pollfd>& fds) {
  for (const auto& request : m_impl->requests)
    for (const auto& read : request.reads)
      if (read.fd >= 0)
        fds.push_back({read.fd, POLLIN, 0});
  for (const auto& write : m_impl->writes)
    fds.push_back({write.fd, POLLOUT, 0});
}
int NativeDataTransfer::pollTimeoutMs() const { return m_impl->requests.empty() && m_impl->writes.empty() ? -1 : 100; }
void NativeDataTransfer::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  auto& self = *m_impl;
  const auto ready = [&](int fd) {
    return std::ranges::any_of(fds.begin() + static_cast<std::ptrdiff_t>(startIdx), fds.end(), [fd](const auto& entry) {
      return entry.fd == fd && entry.revents;
    });
  };
  const auto now = std::chrono::steady_clock::now();
  for (auto& output : self.writes) {
    bool failed = now >= output.deadline;
    if (!failed && ready(output.fd)) {
      while (output.offset < output.data.size()) {
        const auto count = writePipe(output.fd, output.data.data() + output.offset, output.data.size() - output.offset);
        if (count > 0)
          output.offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR)
          continue;
        else {
          failed = count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
          break;
        }
      }
    }
    if (failed || output.offset == output.data.size()) {
      close(output.fd);
      output.fd = -1;
    }
  }
  std::erase_if(self.writes, [](const auto& output) { return output.fd < 0; });
  std::vector<std::pair<ReadCallback, std::optional<Payload>>> completed;
  for (auto& request : self.requests) {
    request.failed |= (request.selectionRequest && request.generation != self.generation) || now >= request.deadline;
    for (auto& input : request.reads) {
      if (input.fd >= 0 && !request.failed && ready(input.fd)) {
        std::array<char, 8192> buffer{};
        for (;;) {
          const auto count = read(input.fd, buffer.data(), buffer.size());
          if (count > 0) {
            if (input.data.size() + static_cast<std::size_t>(count) > kMaximumPayload) {
              request.failed = true;
              break;
            }
            input.data.append(buffer.data(), static_cast<std::size_t>(count));
          } else if (count < 0 && errno == EINTR)
            continue;
          else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
          else {
            request.failed |= count < 0;
            close(input.fd);
            input.fd = -1;
            break;
          }
        }
      }
      if (request.failed && input.fd >= 0) {
        close(input.fd);
        input.fd = -1;
      }
    }
    if (std::ranges::all_of(request.reads, [](const auto& input) { return input.fd < 0; })) {
      std::optional<Payload> payload;
      if (!request.failed) {
        payload.emplace();
        for (auto& input : request.reads)
          payload->emplace(input.mime, std::move(input.data));
      }
      completed.emplace_back(std::move(request.callback), std::move(payload));
    }
  }
  std::erase_if(self.requests, [](const auto& request) { return !request.callback; });
  self.pruneOffers();
  for (auto& [callback, payload] : completed)
    callback(std::move(payload));
}
