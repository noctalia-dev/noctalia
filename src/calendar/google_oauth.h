#pragma once

#include <chrono>
#include <functional>
#include <string>

class HttpClient;

namespace calendar {

  struct OAuthTokens {
    std::string refreshToken;
    std::string accessToken;
    std::chrono::system_clock::time_point expiry{};
  };

  // Client for the api.noctalia.dev Google OAuth broker. The broker holds the OAuth client_secret
  // and performs the code/token exchange; this client never sees it. See the broker hand-off spec.
  class GoogleOAuthBroker {
  public:
    struct StartResult {
      std::string sessionId;
      std::string pollToken;
      std::string authUrl;
      int expiresIn = 0;
      long httpStatus = 0;
    };
    enum class PollStatus { Pending, Complete, Expired, Error };

    explicit GoogleOAuthBroker(HttpClient& http);

    // Begin a Connect flow. On success the caller opens authUrl in a browser and polls.
    void start(std::function<void(bool ok, StartResult)> cb);

    // One poll of the broker result endpoint. Complete carries the tokens.
    void poll(const std::string& pollToken, std::function<void(PollStatus, OAuthTokens)> cb);

    // Exchange a refresh token for a fresh access token. invalidGrant=true means the refresh token
    // is revoked/expired and the account must reconnect.
    void refresh(const std::string& refreshToken, std::function<void(bool ok, bool invalidGrant, OAuthTokens)> cb);

  private:
    HttpClient& m_http;
  };

} // namespace calendar
