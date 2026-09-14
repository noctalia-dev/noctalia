#pragma once

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

  // Discover the calendar collections of a CalDAV account via
  // current-user-principal -> calendar-home-set -> collection PROPFINDs.
  // tls may carry mTLS client-certificate material; pass nullptr when the
  // server does not require a client certificate.
  void discoverCalDavCollections(
      HttpClient& http, const std::string& serverUrl, const std::string& username,
      std::shared_ptr<const security::SecureBuffer> password, bool allowRedirectAuth,
      std::shared_ptr<const HttpTlsClientCert> tls, std::function<void(bool ok, std::vector<CalDavCollection>)> cb
  );

} // namespace calendar
