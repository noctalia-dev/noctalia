#include "calendar/google_oauth.h"

#include "core/log.h"
#include "net/http_client.h"

#include <nlohmann/json.hpp>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-oauth");
    constexpr const char* kBrokerBase = "https://api.noctalia.dev/v1/calendar/oauth/google";

    std::chrono::system_clock::time_point expiryFromUnix(std::int64_t seconds) {
      return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
    }
  } // namespace

  GoogleOAuthBroker::GoogleOAuthBroker(HttpClient& http) : m_http(http) {}

  void GoogleOAuthBroker::start(std::function<void(bool, StartResult)> cb) {
    HttpRequest req;
    req.method = "POST";
    req.url = std::string(kBrokerBase) + "/start";
    req.headers = {"Content-Type: application/json"};
    req.body = R"({"client":"noctalia-shell"})";
    m_http.request(std::move(req), [cb = std::move(cb)](HttpResponse resp) {
      if (!resp.transportOk || resp.status != 200) {
        kLog.warn("oauth start failed http={}", resp.status);
        StartResult result;
        result.httpStatus = resp.status;
        cb(false, std::move(result));
        return;
      }
      try {
        const auto j = nlohmann::json::parse(resp.body);
        StartResult result;
        result.sessionId = j.at("session_id").get<std::string>();
        result.pollToken = j.at("poll_token").get<std::string>();
        result.authUrl = j.at("auth_url").get<std::string>();
        result.expiresIn = j.value("expires_in", 0);
        result.httpStatus = resp.status;
        cb(true, std::move(result));
      } catch (const std::exception& e) {
        kLog.warn("oauth start parse error: {}", e.what());
        StartResult result;
        result.httpStatus = resp.status;
        cb(false, std::move(result));
      }
    });
  }

  void GoogleOAuthBroker::poll(const std::string& pollToken, std::function<void(PollStatus, OAuthTokens)> cb) {
    HttpRequest req;
    req.method = "GET";
    req.url = std::string(kBrokerBase) + "/result";
    req.followRedirects = true;
    req.headers = {"Authorization: Bearer " + pollToken};
    m_http.request(std::move(req), [cb = std::move(cb)](HttpResponse resp) {
      if (!resp.transportOk) {
        cb(PollStatus::Error, {});
        return;
      }
      if (resp.status == 202) {
        cb(PollStatus::Pending, {});
        return;
      }
      if (resp.status == 410) {
        cb(PollStatus::Expired, {});
        return;
      }
      if (resp.status != 200) {
        cb(PollStatus::Error, {});
        return;
      }
      try {
        const auto j = nlohmann::json::parse(resp.body);
        OAuthTokens tokens;
        tokens.refreshToken = j.at("refresh_token").get<std::string>();
        tokens.accessToken = j.at("access_token").get<std::string>();
        tokens.expiry = expiryFromUnix(j.value("expires_at", std::int64_t{0}));
        cb(PollStatus::Complete, std::move(tokens));
      } catch (const std::exception& e) {
        kLog.warn("oauth result parse error: {}", e.what());
        cb(PollStatus::Error, {});
      }
    });
  }

  void GoogleOAuthBroker::refresh(const std::string& refreshToken, std::function<void(bool, bool, OAuthTokens)> cb) {
    HttpRequest req;
    req.method = "POST";
    req.url = std::string(kBrokerBase) + "/refresh";
    req.headers = {"Content-Type: application/json"};
    nlohmann::json body{{"refresh_token", refreshToken}};
    req.body = body.dump();
    m_http.request(std::move(req), [cb = std::move(cb)](HttpResponse resp) {
      if (resp.transportOk && resp.status == 400) {
        cb(false, true, {});
        return;
      }
      if (!resp.transportOk || resp.status != 200) {
        kLog.warn("oauth refresh failed http={}", resp.status);
        cb(false, false, {});
        return;
      }
      try {
        const auto j = nlohmann::json::parse(resp.body);
        OAuthTokens tokens;
        tokens.accessToken = j.at("access_token").get<std::string>();
        tokens.expiry = expiryFromUnix(j.value("expires_at", std::int64_t{0}));
        tokens.refreshToken = j.value("refresh_token", std::string{});
        cb(true, false, std::move(tokens));
      } catch (const std::exception& e) {
        kLog.warn("oauth refresh parse error: {}", e.what());
        cb(false, false, {});
      }
    });
  }

} // namespace calendar
