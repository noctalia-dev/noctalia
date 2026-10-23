#pragma once

#include <cstdint>
#include <string_view>

struct HttpResponse;

namespace calendar {

  // Why an account's sync failed, reported by every fetch path so the failure can be shown to the user (the
  // account's row in Settings) rather than only logged.
  enum class SyncFailure : std::uint8_t {
    None,
    Cancelled,         // the sync was abandoned (shutdown, a superseded request); never shown
    Configuration,     // a required setting is missing (server_url, username, path), or the type is unknown
    Credentials,       // no password is stored, or the password file can't be read
    CredentialsLocked, // the keyring is locked or Secret Service is unavailable
    ClientCertificate, // the mTLS client certificate or key file (client_cert_file, client_key_file) can't be read
    Authentication,    // the server rejected the credentials (HTTP 401, a revoked Google grant)
    AccessDenied,      // authenticated, but server denied access (HTTP 403): no permission, Google quota or rate limit
    NotFound,          // nothing at the configured address: HTTP 404/405/410, a discovery step without an answer, a
                       // missing vdir directory
    NoCalendars,       // calendars were looked for, and none that hold events was found or selected
    Timeout,           // the transfer didn't finish in time
    Network,           // the server couldn't be reached (DNS, connection, TLS)
    Server,            // HTTP 5xx
    InvalidResponse,   // another HTTP status, or data that couldn't be parsed
  };

  // The failure of an HTTP response that wasn't the expected success.
  [[nodiscard]] SyncFailure syncFailureFromResponse(const HttpResponse& response);

  // The translation key of the failure's explanation. A Google account has no password: its rejected or missing
  // credentials are fixed by reconnecting it.
  [[nodiscard]] std::string_view syncFailureMessageKey(SyncFailure failure, bool google = false);

} // namespace calendar
