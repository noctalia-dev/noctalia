#include "shell/wayland/clipboard_thumbnailer.h"

#include "core/deferred_call.h"
#include "render/core/image_decoder.h"
#include "render/core/image_resize.h"

#include <algorithm>
#include <cmath>
#include <utility>

ClipboardThumbnailer::ClipboardThumbnailer() = default;

ClipboardThumbnailer::~ClipboardThumbnailer() {
  {
    std::scoped_lock lock(m_mutex);
    m_shutdown = true;
    m_jobs.clear();
  }
  m_cv.notify_all();
  if (m_worker.joinable()) {
    m_worker.join();
  }
}

std::string ClipboardThumbnailer::cacheKey(const std::string& storageId, int targetPx) {
  return storageId + "@" + std::to_string(targetPx);
}

const ClipboardThumbnailer::Thumbnail* ClipboardThumbnailer::find(const std::string& storageId, int targetPx) const {
  const auto it = m_ready.find(cacheKey(storageId, targetPx));
  return it != m_ready.end() ? &it->second : nullptr;
}

bool ClipboardThumbnailer::failed(const std::string& storageId, int targetPx) const {
  return m_failed.contains(cacheKey(storageId, targetPx));
}

bool ClipboardThumbnailer::queued(const std::string& storageId, int targetPx) const {
  return m_queued.contains(cacheKey(storageId, targetPx));
}

void ClipboardThumbnailer::request(Request request) {
  if (request.storageId.empty() || request.key == nullptr || request.targetPx <= 0) {
    return;
  }
  std::string key = cacheKey(request.storageId, request.targetPx);
  if (m_ready.contains(key) || m_failed.contains(key) || !m_queued.insert(key).second) {
    return;
  }
  {
    std::scoped_lock lock(m_mutex);
    m_jobs.push_back(std::move(request));
    if (!m_worker.joinable()) {
      m_worker = std::thread([this] { workerLoop(); });
    }
  }
  m_cv.notify_one();
}

void ClipboardThumbnailer::setReadyCallback(std::function<void()> callback) { m_readyCallback = std::move(callback); }

void ClipboardThumbnailer::clear() {
  m_ready.clear();
  m_failed.clear();
}

std::optional<ClipboardThumbnailer::Thumbnail> ClipboardThumbnailer::makeThumbnail(const Request& request) {
  auto payload = security::readEncryptedFile(request.payloadPath, *request.key, request.context, request.maxBytes);
  if (!payload.succeeded() || payload.plaintext.empty()) {
    return std::nullopt;
  }
  auto decoded = decodeRasterImage(payload.plaintext.data(), payload.plaintext.size());
  payload.plaintext.clear();
  payload.plaintext.shrink_to_fit();
  if (!decoded || decoded->width <= 0 || decoded->height <= 0) {
    return std::nullopt;
  }

  // The list shows thumbnails cropped to a square, so the short side is what needs targetPx; the long side
  // is capped for extreme aspect ratios.
  const int shortSide = std::min(decoded->width, decoded->height);
  int width = decoded->width;
  int height = decoded->height;
  if (shortSide > request.targetPx) {
    const double scale = static_cast<double>(request.targetPx) / static_cast<double>(shortSide);
    width = std::max(1, static_cast<int>(std::lround(width * scale)));
    height = std::max(1, static_cast<int>(std::lround(height * scale)));
  }
  const ImageExtent capped = fitImageWithin(width, height, request.targetPx * 4, request.targetPx * 4);
  width = capped.width;
  height = capped.height;

  Thumbnail thumbnail;
  if (width == decoded->width && height == decoded->height) {
    thumbnail.rgba = std::move(decoded->pixels);
  } else {
    thumbnail.rgba =
        resizeToRgba(decoded->pixels.data(), decoded->width, decoded->height, ResizeSourceLayout::Rgba, width, height);
    if (thumbnail.rgba.empty()) {
      return std::nullopt;
    }
  }
  thumbnail.width = width;
  thumbnail.height = height;
  return thumbnail;
}

void ClipboardThumbnailer::workerLoop() {
  while (true) {
    Request request;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [this] { return m_shutdown || !m_jobs.empty(); });
      if (m_shutdown) {
        return;
      }
      request = std::move(m_jobs.front());
      m_jobs.pop_front();
    }

    Result result{.key = cacheKey(request.storageId, request.targetPx), .thumbnail = makeThumbnail(request)};
    DeferredCall::callLater([this, alive = std::weak_ptr<int>(m_lifetime), result = std::move(result)]() mutable {
      if (alive.expired()) {
        return;
      }
      onResult(std::move(result));
    });
  }
}

void ClipboardThumbnailer::onResult(Result result) {
  m_queued.erase(result.key);
  if (result.thumbnail.has_value()) {
    m_ready.insert_or_assign(std::move(result.key), std::move(*result.thumbnail));
  } else {
    m_failed.insert(std::move(result.key));
  }
  if (m_readyCallback) {
    m_readyCallback();
  }
}
