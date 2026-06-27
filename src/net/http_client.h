#pragma once

#include <array>
#include <chrono>
#include <curl/curl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct HttpRequest {
  std::string method = "GET"; // GET/POST/PUT/PROPFIND/REPORT/… -> CURLOPT_CUSTOMREQUEST
  std::string url;
  std::vector<std::string> headers; // e.g. "Authorization: Bearer …", "Depth: 1"
  std::string body;                 // sent as the request body when non-empty
  bool followRedirects = false;
  bool allowRedirectAuth = false; // continue auth across redirect hosts; use only for trusted provider redirects
  std::string basicUsername;
  std::string basicPassword;
};

struct HttpResponse {
  bool transportOk = false; // true when the request completed without a transport error
  long status = 0;          // HTTP status code (0 when transportOk is false)
  std::string effectiveUrl; // final URL after redirects (empty when unavailable)
  std::string body;
};

class HttpClient {
public:
  using CompletionCallback = std::function<void(bool success)>;
  using ResponseCallback = std::function<void(HttpResponse)>;

  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  void setOfflineMode(bool offline) { m_offlineMode = offline; }

  // Start an async download of url to destPath.
  // cb is always invoked on the main loop thread on a later iteration — never
  // synchronously from inside this call, even when offline mode or an early
  // local error means the request is never issued. Callers can rely on this
  // to avoid reentrant state mutation.
  void download(std::string_view url, const std::filesystem::path& destPath, CompletionCallback cb);

  // Try each url in order, stopping at the first that downloads successfully to destPath.
  // cb(true) once one succeeds; cb(false) if all fail. Same deferred-callback semantics as
  // the single-url overload.
  void download(std::vector<std::string> urls, const std::filesystem::path& destPath, CompletionCallback cb);

  // Fire-and-forget async POST. Same callback semantics as download().
  void post(std::string_view url, std::string body, std::string_view contentType, CompletionCallback cb);

  // General async request with custom method/headers/body. The response body and HTTP status are
  // delivered in memory to cb. cb is always invoked on the main loop thread on a later iteration,
  // never synchronously — including in offline mode or on an early local error. Unlike download()
  // and post(), non-2xx HTTP responses are reported as transportOk=true with their status/body, so
  // callers can read 401/3xx/4xx bodies.
  void request(HttpRequest req, ResponseCallback cb);

  // PollSource integration — called by HttpClientPollSource.
  void addPollFds(std::vector<pollfd>& fds);
  [[nodiscard]] int timeoutMs() const;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx);

private:
  struct Transfer {
    std::filesystem::path destPath;
    std::filesystem::path tempPath;
    FILE* file = nullptr;
    std::vector<CompletionCallback> callbacks;
    std::string destKey;
    std::string url;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
  };

  struct PostTransfer {
    std::string body;
    curl_slist* headers = nullptr;
    CompletionCallback callback;
    std::string url;
    std::string response;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
  };

  struct RequestTransfer {
    std::string body;
    curl_slist* headers = nullptr;
    ResponseCallback callback;
    std::string url;
    std::string basicUsername;
    std::string basicPassword;
    std::string response;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
  };

  struct SequentialDownload {
    std::vector<std::string> urls;
    std::filesystem::path destPath;
    CompletionCallback callback;
    std::size_t index = 0;
  };

  void downloadSequential(std::shared_ptr<SequentialDownload> state);
  void finishTransfer(CURL* easy, CURLcode result);
  void finishPostTransfer(CURL* easy, CURLcode result);
  void finishRequestTransfer(CURL* easy, CURLcode result);
  void performMulti(const char* reason);
  [[nodiscard]] bool hasActiveTransfers() const;

  CURLM* m_multi = nullptr;
  int m_running = 0;
  std::chrono::steady_clock::time_point m_lastServiceAt{};
  bool m_offlineMode = false;
  std::unordered_map<CURL*, Transfer> m_transfers;
  std::unordered_map<CURL*, PostTransfer> m_postTransfers;
  std::unordered_map<CURL*, RequestTransfer> m_requestTransfers;
  std::unordered_map<std::string, CURL*> m_activeByDest;
};
