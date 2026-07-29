#include "net/http_client.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
  constexpr Logger kLog("http");
  constexpr float kSlowCurlOperationDebugMs = 50.0f;
  constexpr float kSlowCurlOperationWarnMs = 1000.0f;
  constexpr float kCurlServiceGapWarnMs = 5000.0f;

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  std::string urlForLog(std::string_view url) {
    const std::size_t redactedPos = url.find_first_of("?#");
    if (redactedPos != std::string_view::npos) {
      const char suffix = url[redactedPos] == '?' ? '?' : '#';
      return std::string(url.substr(0, redactedPos))
          + suffix
          + (suffix == '?' ? "<query redacted>" : "<fragment redacted>");
    }

    return std::string(url);
  }

  void deferFailure(HttpClient::CompletionCallback cb) {
    if (!cb) {
      return;
    }
    DeferredCall::callLater([cb = std::move(cb)]() { cb(false); });
  }

  void deferFailures(std::vector<HttpClient::CompletionCallback> callbacks) {
    if (callbacks.empty()) {
      return;
    }
    DeferredCall::callLater([callbacks = std::move(callbacks)]() mutable {
      for (auto& callback : callbacks) {
        if (callback) {
          callback(false);
        }
      }
    });
  }

  void deferResponse(HttpClient::ResponseCallback cb) {
    if (!cb) {
      return;
    }
    DeferredCall::callLater([cb = std::move(cb)]() mutable { cb(HttpResponse{}); });
  }

  void deferStreamClose(HttpClient::StreamCloseCallback cb) {
    if (!cb) {
      return;
    }
    DeferredCall::callLater([cb = std::move(cb)]() mutable { cb(HttpStreamResult{}); });
  }

  std::size_t captureResponse(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    if (userdata == nullptr || ptr == nullptr || bytes == 0) {
      return bytes;
    }

    auto* response = static_cast<std::string*>(userdata);
    response->append(ptr, bytes);
    return bytes;
  }

  std::size_t forwardStreamChunk(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    if (userdata == nullptr || ptr == nullptr || bytes == 0) {
      return bytes;
    }

    auto* onData = static_cast<HttpClient::StreamDataCallback*>(userdata);
    if (*onData) {
      (*onData)(std::string_view(ptr, bytes));
    }
    return bytes;
  }
} // namespace

HttpClient::HttpClient() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  m_multi = curl_multi_init();
}

HttpClient::~HttpClient() {
  for (auto& [easy, transfer] : m_transfers) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    if (transfer.file != nullptr) {
      std::fclose(transfer.file);
    }
    std::filesystem::remove(transfer.tempPath);
  }
  for (auto& [easy, post] : m_postTransfers) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    if (post.headers != nullptr) {
      curl_slist_free_all(post.headers);
    }
  }
  for (auto& [easy, req] : m_requestTransfers) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    if (req.headers != nullptr) {
      curl_slist_free_all(req.headers);
    }
  }
  for (auto& [easy, stream] : m_streamTransfers) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    if (stream.headers != nullptr) {
      curl_slist_free_all(stream.headers);
    }
  }
  curl_multi_cleanup(m_multi);
  curl_global_cleanup();
}

bool HttpClient::hasActiveTransfers() const {
  return !m_transfers.empty() || !m_postTransfers.empty() || !m_requestTransfers.empty() || !m_streamTransfers.empty();
}

void HttpClient::download(std::string_view url, const std::filesystem::path& destPath, CompletionCallback cb) {
  if (m_offlineMode) {
    kLog.warn("download skipped in offline mode url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  const std::string destKey = destPath.string();
  if (auto activeIt = m_activeByDest.find(destKey); activeIt != m_activeByDest.end()) {
    if (auto transferIt = m_transfers.find(activeIt->second); transferIt != m_transfers.end()) {
      transferIt->second.callbacks.push_back(std::move(cb));
      return;
    }
    m_activeByDest.erase(activeIt);
  }

  const auto tempPath = destPath.parent_path() / (destPath.filename().string() + ".part");

  FILE* f = std::fopen(tempPath.c_str(), "wb");
  if (f == nullptr) {
    kLog.warn("download failed to open temp file {} for url={}", tempPath.string(), urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  CURL* easy = curl_easy_init();
  if (easy == nullptr) {
    std::fclose(f);
    std::filesystem::remove(tempPath);
    kLog.warn("download failed to create curl handle url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  const std::string urlStr(url);
  curl_easy_setopt(easy, CURLOPT_URL, urlStr.c_str());
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);

  Transfer transfer{};
  transfer.destPath = destPath;
  transfer.tempPath = tempPath;
  transfer.file = f;
  transfer.callbacks.push_back(std::move(cb));
  transfer.destKey = destKey;
  transfer.url = urlStr;
  m_transfers[easy] = std::move(transfer);
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, m_transfers[easy].errorBuffer.data());
  const CURLMcode addResult = curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    Transfer failedTransfer = std::move(m_transfers[easy]);
    m_transfers.erase(easy);
    curl_easy_cleanup(easy);
    if (failedTransfer.file != nullptr) {
      std::fclose(failedTransfer.file);
    }
    std::filesystem::remove(failedTransfer.tempPath);
    kLog.warn("download failed to add curl handle url={} error={}", urlForLog(urlStr), curl_multi_strerror(addResult));
    deferFailures(std::move(failedTransfer.callbacks));
    return;
  }
  m_activeByDest[destKey] = easy;
  performMulti("download start");
}

void HttpClient::download(std::vector<std::string> urls, const std::filesystem::path& destPath, CompletionCallback cb) {
  if (urls.empty()) {
    deferFailure(std::move(cb));
    return;
  }
  auto state = std::make_shared<SequentialDownload>();
  state->urls = std::move(urls);
  state->destPath = destPath;
  state->callback = std::move(cb);
  downloadSequential(std::move(state));
}

void HttpClient::downloadSequential(std::shared_ptr<SequentialDownload> state) {
  const std::size_t index = state->index;
  // The captured shared state keeps urls/dest/callback alive across async hops;
  // advance to the next candidate only when the current one fails.
  download(state->urls[index], state->destPath, [this, state](bool success) {
    if (success || state->index + 1 >= state->urls.size()) {
      state->callback(success);
      return;
    }
    state->index += 1;
    downloadSequential(state);
  });
}

void HttpClient::post(std::string_view url, std::string body, std::string_view contentType, CompletionCallback cb) {
  if (m_offlineMode) {
    kLog.warn("post skipped in offline mode url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  CURL* easy = curl_easy_init();
  if (easy == nullptr) {
    kLog.warn("post failed to create curl handle url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  PostTransfer post{};
  post.body = std::move(body);
  post.callback = std::move(cb);

  const std::string header = "Content-Type: " + std::string(contentType);
  post.headers = curl_slist_append(nullptr, header.c_str());

  const std::string urlStr(url);
  post.url = urlStr;
  curl_easy_setopt(easy, CURLOPT_URL, urlStr.c_str());
  curl_easy_setopt(easy, CURLOPT_POST, 1L);
  curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(post.body.size()));
  curl_easy_setopt(easy, CURLOPT_POSTFIELDS, post.body.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, post.headers);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);

  m_postTransfers[easy] = std::move(post);
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, m_postTransfers[easy].errorBuffer.data());
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, captureResponse);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &m_postTransfers[easy].response);
  const CURLMcode addResult = curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    PostTransfer failedPost = std::move(m_postTransfers[easy]);
    m_postTransfers.erase(easy);
    curl_easy_cleanup(easy);
    if (failedPost.headers != nullptr) {
      curl_slist_free_all(failedPost.headers);
    }
    kLog.warn("post failed to add curl handle url={} error={}", urlForLog(urlStr), curl_multi_strerror(addResult));
    deferFailure(std::move(failedPost.callback));
    return;
  }
  performMulti("post start");
}

void HttpClient::request(HttpRequest req, ResponseCallback cb) {
  if (m_offlineMode) {
    kLog.warn("request skipped in offline mode url={}", urlForLog(req.url));
    deferResponse(std::move(cb));
    return;
  }

  CURL* easy = curl_easy_init();
  if (easy == nullptr) {
    kLog.warn("request failed to create curl handle url={}", urlForLog(req.url));
    deferResponse(std::move(cb));
    return;
  }

  RequestTransfer transfer{};
  transfer.callback = std::move(cb);
  transfer.url = req.url;
  transfer.body = std::move(req.body);
  transfer.basicUsername = std::move(req.basicUsername);
  transfer.basicPassword = std::move(req.basicPassword);
  for (const auto& header : req.headers) {
    transfer.headers = curl_slist_append(transfer.headers, header.c_str());
  }

  m_requestTransfers[easy] = std::move(transfer);
  RequestTransfer& stored = m_requestTransfers[easy];

  curl_easy_setopt(easy, CURLOPT_URL, stored.url.c_str());
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  if (req.allowInsecureTls) {
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  if (req.followRedirects) {
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    if (req.allowRedirectAuth) {
      curl_easy_setopt(easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    }
  }
  if (req.freshConnection) {
    curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
  }
  if (!stored.basicUsername.empty() || !stored.basicPassword.empty()) {
    curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(easy, CURLOPT_USERNAME, stored.basicUsername.c_str());
    curl_easy_setopt(easy, CURLOPT_PASSWORD, stored.basicPassword.c_str());
  }
  // No CURLOPT_FAILONERROR: callers need the status and body for non-2xx responses.
  if (req.method == "POST") {
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
  } else if (req.method != "GET") {
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }
  if (stored.headers != nullptr) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, stored.headers);
  }
  if (!stored.body.empty()) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(stored.body.size()));
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, stored.body.c_str());
  }
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, stored.errorBuffer.data());
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, captureResponse);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &stored.response);

  const CURLMcode addResult = curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    RequestTransfer failed = std::move(m_requestTransfers[easy]);
    m_requestTransfers.erase(easy);
    curl_easy_cleanup(easy);
    if (failed.headers != nullptr) {
      curl_slist_free_all(failed.headers);
    }
    kLog.warn(
        "request failed to add curl handle url={} error={}", urlForLog(failed.url), curl_multi_strerror(addResult)
    );
    deferResponse(std::move(failed.callback));
    return;
  }
  performMulti("request start");
}

HttpClient::StreamId HttpClient::startStream(HttpRequest req, StreamDataCallback onData, StreamCloseCallback onClose) {
  if (m_offlineMode) {
    kLog.warn("stream skipped in offline mode url={}", urlForLog(req.url));
    deferStreamClose(std::move(onClose));
    return 0;
  }

  CURL* easy = curl_easy_init();
  if (easy == nullptr) {
    kLog.warn("stream failed to create curl handle url={}", urlForLog(req.url));
    deferStreamClose(std::move(onClose));
    return 0;
  }

  StreamTransfer transfer{};
  transfer.id = m_nextStreamId++;
  transfer.onData = std::move(onData);
  transfer.onClose = std::move(onClose);
  transfer.url = req.url;
  transfer.body = std::move(req.body);
  transfer.basicUsername = std::move(req.basicUsername);
  transfer.basicPassword = std::move(req.basicPassword);
  for (const auto& header : req.headers) {
    transfer.headers = curl_slist_append(transfer.headers, header.c_str());
  }

  m_streamTransfers[easy] = std::move(transfer);
  StreamTransfer& stored = m_streamTransfers[easy];
  m_streamsById[stored.id] = easy;

  curl_easy_setopt(easy, CURLOPT_URL, stored.url.c_str());
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  // No CURLOPT_TIMEOUT: the transfer is expected to stay open indefinitely.
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  if (req.allowInsecureTls) {
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  if (req.followRedirects) {
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    if (req.allowRedirectAuth) {
      curl_easy_setopt(easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    }
  }
  if (req.freshConnection) {
    curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
  }
  if (!stored.basicUsername.empty() || !stored.basicPassword.empty()) {
    curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(easy, CURLOPT_USERNAME, stored.basicUsername.c_str());
    curl_easy_setopt(easy, CURLOPT_PASSWORD, stored.basicPassword.c_str());
  }
  // No CURLOPT_FAILONERROR: error responses stream their body and report status on close.
  if (req.method == "POST") {
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
  } else if (req.method != "GET") {
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }
  if (stored.headers != nullptr) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, stored.headers);
  }
  if (!stored.body.empty()) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(stored.body.size()));
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, stored.body.c_str());
  }
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, stored.errorBuffer.data());
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, forwardStreamChunk);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &stored.onData);

  const CURLMcode addResult = curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    StreamTransfer failed = std::move(m_streamTransfers[easy]);
    m_streamsById.erase(failed.id);
    m_streamTransfers.erase(easy);
    curl_easy_cleanup(easy);
    if (failed.headers != nullptr) {
      curl_slist_free_all(failed.headers);
    }
    kLog.warn(
        "stream failed to add curl handle url={} error={}", urlForLog(failed.url), curl_multi_strerror(addResult)
    );
    deferStreamClose(std::move(failed.onClose));
    return 0;
  }
  const StreamId id = stored.id;
  performMulti("stream start");
  return id;
}

void HttpClient::cancelStream(StreamId id) {
  const auto byId = m_streamsById.find(id);
  if (byId == m_streamsById.end()) {
    return;
  }
  CURL* easy = byId->second;
  m_streamsById.erase(byId);

  const auto it = m_streamTransfers.find(easy);
  if (it != m_streamTransfers.end()) {
    if (it->second.headers != nullptr) {
      curl_slist_free_all(it->second.headers);
    }
    m_streamTransfers.erase(it);
  }
  curl_multi_remove_handle(m_multi, easy);
  curl_easy_cleanup(easy);
}

void HttpClient::addPollFds(std::vector<pollfd>& fds) {
  if (!hasActiveTransfers()) {
    return;
  }

  fd_set readfds, writefds, errfds;
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&errfds);
  int maxfd = -1;
  curl_multi_fdset(m_multi, &readfds, &writefds, &errfds, &maxfd);

  for (int fd = 0; fd <= maxfd; ++fd) {
    short events = 0;
    if (FD_ISSET(fd, &readfds)) {
      events |= POLLIN;
    }
    if (FD_ISSET(fd, &writefds)) {
      events |= POLLOUT;
    }
    if (events != 0) {
      fds.push_back({fd, events, 0});
    }
  }
}

int HttpClient::timeoutMs() const {
  if (!hasActiveTransfers()) {
    return -1;
  }
  long timeout = -1;
  curl_multi_timeout(m_multi, &timeout);
  // curl_multi_timeout can report a deadline up to the per-transfer timeout, but
  // curl needs servicing at least once per second to advance its timeout
  // handling, so cap the wait while transfers are active.
  if (timeout < 0 || timeout > 1000) {
    return 1000;
  }
  return static_cast<int>(timeout);
}

void HttpClient::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  if (!hasActiveTransfers()) {
    return;
  }

  performMulti("poll dispatch");

  CURLMsg* msg = nullptr;
  int msgsLeft = 0;
  while ((msg = curl_multi_info_read(m_multi, &msgsLeft)) != nullptr) {
    if (msg->msg == CURLMSG_DONE) {
      CURL* easy = msg->easy_handle;
      if (m_postTransfers.contains(easy)) {
        finishPostTransfer(easy, msg->data.result);
      } else if (m_requestTransfers.contains(easy)) {
        finishRequestTransfer(easy, msg->data.result);
      } else if (m_streamTransfers.contains(easy)) {
        finishStreamTransfer(easy, msg->data.result);
      } else {
        finishTransfer(easy, msg->data.result);
      }
    }
  }

  if (!hasActiveTransfers()) {
    m_lastServiceAt = {};
  }
}

void HttpClient::performMulti(const char* reason) {
  const auto now = std::chrono::steady_clock::now();
  if (m_lastServiceAt != std::chrono::steady_clock::time_point{}) {
    const float serviceGapMs = std::chrono::duration<float, std::milli>(now - m_lastServiceAt).count();
    if (serviceGapMs >= kCurlServiceGapWarnMs) {
      kLog.warn(
          "http client was not serviced for {:.1f}ms before {} (downloads={} posts={} running={})", serviceGapMs,
          reason, m_transfers.size(), m_postTransfers.size(), m_running
      );
    }
  }

  const auto opStart = std::chrono::steady_clock::now();
  curl_multi_perform(m_multi, &m_running);
  const float ms = elapsedSince(opStart);
  if (ms >= kSlowCurlOperationWarnMs) {
    kLog.warn("curl_multi_perform took {:.1f}ms during {}", ms, reason);
  } else if (ms >= kSlowCurlOperationDebugMs) {
    kLog.debug("curl_multi_perform took {:.1f}ms during {}", ms, reason);
  }
  m_lastServiceAt = std::chrono::steady_clock::now();
}

void HttpClient::finishTransfer(CURL* easy, CURLcode result) {
  auto it = m_transfers.find(easy);
  if (it == m_transfers.end()) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    return;
  }

  Transfer transfer = std::move(it->second);
  m_transfers.erase(it);
  m_activeByDest.erase(transfer.destKey);

  long responseCode = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_multi_remove_handle(m_multi, easy);
  curl_easy_cleanup(easy);

  if (transfer.file != nullptr) {
    std::fclose(transfer.file);
    transfer.file = nullptr;
  }

  bool success = result == CURLE_OK;
  if (success) {
    std::error_code ec;
    std::filesystem::rename(transfer.tempPath, transfer.destPath, ec);
    if (ec) {
      std::filesystem::copy_file(
          transfer.tempPath, transfer.destPath, std::filesystem::copy_options::overwrite_existing, ec
      );
      std::filesystem::remove(transfer.tempPath);
      success = !ec;
      if (!success) {
        kLog.warn(
            "download failed to move {} to {}: {}", transfer.tempPath.string(), transfer.destPath.string(), ec.message()
        );
      }
    }
  } else {
    const char* detail = transfer.errorBuffer[0] != '\0' ? transfer.errorBuffer.data() : curl_easy_strerror(result);
    kLog.warn(
        "download failed url={} curl={} http={} error={}", urlForLog(transfer.url), static_cast<int>(result),
        responseCode, detail
    );
    std::filesystem::remove(transfer.tempPath);
  }

  for (auto& callback : transfer.callbacks) {
    callback(success);
  }
}

void HttpClient::finishPostTransfer(CURL* easy, CURLcode result) {
  auto it = m_postTransfers.find(easy);
  if (it == m_postTransfers.end()) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    return;
  }

  PostTransfer post = std::move(it->second);
  m_postTransfers.erase(it);

  long responseCode = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_multi_remove_handle(m_multi, easy);
  curl_easy_cleanup(easy);

  if (post.headers != nullptr) {
    curl_slist_free_all(post.headers);
  }

  const bool success = result == CURLE_OK;
  if (!success) {
    const char* detail = post.errorBuffer[0] != '\0' ? post.errorBuffer.data() : curl_easy_strerror(result);
    kLog.warn(
        "post failed url={} curl={} http={} error={}", urlForLog(post.url), static_cast<int>(result), responseCode,
        detail
    );
  }

  if (post.callback) {
    post.callback(success);
  }
}

void HttpClient::finishRequestTransfer(CURL* easy, CURLcode result) {
  auto it = m_requestTransfers.find(easy);
  if (it == m_requestTransfers.end()) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    return;
  }

  RequestTransfer transfer = std::move(it->second);
  m_requestTransfers.erase(it);

  long responseCode = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_multi_remove_handle(m_multi, easy);
  curl_easy_cleanup(easy);

  if (transfer.headers != nullptr) {
    curl_slist_free_all(transfer.headers);
  }

  HttpResponse response;
  response.transportOk = result == CURLE_OK;
  response.status = responseCode;
  response.body = std::move(transfer.response);
  if (char* effectiveUrl = nullptr; curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl) == CURLE_OK
      && effectiveUrl != nullptr
      && effectiveUrl[0] != '\0') {
    response.effectiveUrl = effectiveUrl;
  }
  if (!response.transportOk) {
    const char* detail = transfer.errorBuffer[0] != '\0' ? transfer.errorBuffer.data() : curl_easy_strerror(result);
    kLog.warn(
        "request failed url={} curl={} http={} error={}", urlForLog(transfer.url), static_cast<int>(result),
        responseCode, detail
    );
  }

  if (transfer.callback) {
    transfer.callback(std::move(response));
  }
}

void HttpClient::finishStreamTransfer(CURL* easy, CURLcode result) {
  auto it = m_streamTransfers.find(easy);
  if (it == m_streamTransfers.end()) {
    curl_multi_remove_handle(m_multi, easy);
    curl_easy_cleanup(easy);
    return;
  }

  StreamTransfer transfer = std::move(it->second);
  m_streamTransfers.erase(it);
  m_streamsById.erase(transfer.id);

  long responseCode = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_multi_remove_handle(m_multi, easy);
  curl_easy_cleanup(easy);

  if (transfer.headers != nullptr) {
    curl_slist_free_all(transfer.headers);
  }

  HttpStreamResult streamResult;
  streamResult.transportOk = result == CURLE_OK;
  streamResult.status = responseCode;
  if (!streamResult.transportOk) {
    const char* detail = transfer.errorBuffer[0] != '\0' ? transfer.errorBuffer.data() : curl_easy_strerror(result);
    kLog.warn(
        "stream ended url={} curl={} http={} error={}", urlForLog(transfer.url), static_cast<int>(result), responseCode,
        detail
    );
  }

  if (transfer.onClose) {
    transfer.onClose(streamResult);
  }
}
