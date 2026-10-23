#include "calendar/sync_failure.h"

#include "net/http_client.h"

namespace calendar {

  SyncFailure syncFailureFromResponse(const HttpResponse& response) {
    if (!response.transportOk) {
      return response.timedOut ? SyncFailure::Timeout : SyncFailure::Network;
    }
    if (response.status == 401) {
      return SyncFailure::Authentication;
    }
    if (response.status == 403) {
      return SyncFailure::AccessDenied;
    }
    // 405: the address answers, but not as a CalDAV/WebDAV resource.
    if (response.status == 404 || response.status == 405 || response.status == 410) {
      return SyncFailure::NotFound;
    }
    if (response.status >= 500 && response.status < 600) {
      return SyncFailure::Server;
    }
    return SyncFailure::InvalidResponse;
  }

  std::string_view syncFailureMessageKey(SyncFailure failure, bool google) {
    if (google && (failure == SyncFailure::Authentication || failure == SyncFailure::Credentials)) {
      return "calendar.sync.failure.google-authorization";
    }
    switch (failure) {
    case SyncFailure::None:
    case SyncFailure::Cancelled:
      return {};
    case SyncFailure::Configuration:
      return "calendar.sync.failure.configuration";
    case SyncFailure::Credentials:
      return "calendar.sync.failure.credentials";
    case SyncFailure::CredentialsLocked:
      return "calendar.sync.failure.credentials-locked";
    case SyncFailure::ClientCertificate:
      return "calendar.sync.failure.client-certificate";
    case SyncFailure::Authentication:
      return "calendar.sync.failure.authentication";
    case SyncFailure::AccessDenied:
      return "calendar.sync.failure.access-denied";
    case SyncFailure::NotFound:
      return "calendar.sync.failure.not-found";
    case SyncFailure::NoCalendars:
      return "calendar.sync.failure.no-calendars";
    case SyncFailure::Timeout:
      return "calendar.sync.failure.timeout";
    case SyncFailure::Network:
      return "calendar.sync.failure.network";
    case SyncFailure::Server:
      return "calendar.sync.failure.server";
    case SyncFailure::InvalidResponse:
      return "calendar.sync.failure.invalid-response";
    }
    return {};
  }

} // namespace calendar
