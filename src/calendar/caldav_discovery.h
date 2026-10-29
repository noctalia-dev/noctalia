#pragma once

#include "calendar/sync_failure.h"
#include "net/http_client.h"
#include "security/secure_buffer.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class HttpClient;

namespace calendar {

  struct CalDavCollection {
    std::string id;
    std::string name;
    std::string url;
    std::string color;
  };

  // Finds the event calendars of an account from its discovery root (current-user-principal, then
  // calendar-home-set, then the collections there). tls may carry mTLS client-certificate material;
  // pass nullptr when the server does not require a client certificate. cb gets SyncFailure::None and
  // the collections, or why discovery failed.
  void discoverCalDavCollections(
      HttpClient& http, const std::string& serverUrl, const std::string& username,
      std::shared_ptr<const security::SecureBuffer> password, bool allowRedirectAuth,
      std::shared_ptr<const HttpTlsClientCert> tls,
      std::function<void(SyncFailure failure, std::vector<CalDavCollection>)> cb
  );

} // namespace calendar
