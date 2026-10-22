#include "calendar/sync_failure.h"

#include "net/http_client.h"

namespace calendar {

  SyncFailure syncFailureFromResponse(const HttpResponse& response) {
    if (!response.transportOk) {
      return response.timedOut ? SyncFailure::Timeout : SyncFailure::Network;
    }
    if (response.status == 401 || response.status == 403) {
      return SyncFailure::Authentication;
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
      return "settings.schema.services.calendar-edit.failure-google-authorization";
    }
    switch (failure) {
    case SyncFailure::None:
    case SyncFailure::Cancelled:
      return {};
    case SyncFailure::Configuration:
      return "settings.schema.services.calendar-edit.failure-configuration";
    case SyncFailure::Credentials:
      return "settings.schema.services.calendar-edit.failure-credentials";
    case SyncFailure::CredentialsLocked:
      return "settings.schema.services.calendar-edit.failure-credentials-locked";
    case SyncFailure::ClientCertificate:
      return "settings.schema.services.calendar-edit.failure-client-certificate";
    case SyncFailure::Authentication:
      return "settings.schema.services.calendar-edit.failure-authentication";
    case SyncFailure::NotFound:
      return "settings.schema.services.calendar-edit.failure-not-found";
    case SyncFailure::NoCalendars:
      return "settings.schema.services.calendar-edit.failure-no-calendars";
    case SyncFailure::Timeout:
      return "settings.schema.services.calendar-edit.failure-timeout";
    case SyncFailure::Network:
      return "settings.schema.services.calendar-edit.failure-network";
    case SyncFailure::Server:
      return "settings.schema.services.calendar-edit.failure-server";
    case SyncFailure::InvalidResponse:
      return "settings.schema.services.calendar-edit.failure-invalid-response";
    }
    return {};
  }

} // namespace calendar
