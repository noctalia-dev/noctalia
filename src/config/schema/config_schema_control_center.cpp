#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace noctalia::config::schema {

  namespace {
    const Schema<ShortcutConfig>& shortcutSchema() {
      static const Schema<ShortcutConfig> s = {field(&ShortcutConfig::type, "type")};
      return s;
    }

    const Schema<ControlCenterConfig::CalendarTabConfig>& calendarTabSchema() {
      static const Schema<ControlCenterConfig::CalendarTabConfig> s = {
          field(&ControlCenterConfig::CalendarTabConfig::showEventsCard, "show_events_card"),
          field(&ControlCenterConfig::CalendarTabConfig::showWeekNumbers, "show_week_numbers"),
          field(&ControlCenterConfig::CalendarTabConfig::showEventsLocation, "show_events_location"),
          field(&ControlCenterConfig::CalendarTabConfig::hidePastEvents, "hide_past_events"),
          field(
              &ControlCenterConfig::CalendarTabConfig::pastEventsThresholdMinutes, "past_events_threshold_minutes",
              kPastEventsThresholdMinutesRange
          ),
      };
      return s;
    }

    const Schema<ControlCenterConfig::MediaTabConfig>& mediaTabSchema() {
      static const Schema<ControlCenterConfig::MediaTabConfig> s = {
          field(&ControlCenterConfig::MediaTabConfig::showVisualizer, "show_visualizer"),
      };
      return s;
    }
  } // namespace

  const Schema<ControlCenterConfig>& controlCenterSchema() {
    static const Schema<ControlCenterConfig> s = {
        enumField(&ControlCenterConfig::sidebarMode, "sidebar", kControlCenterSidebarModes),
        enumField(&ControlCenterConfig::sidebarSectionMode, "sidebar_section", kControlCenterSidebarModes),
        field(&ControlCenterConfig::width, "width", kControlCenterWidthRange),
        field(&ControlCenterConfig::showShortcutLabels, "show_shortcut_labels"),
        field(&ControlCenterConfig::showSessionButton, "show_session_button"),
        field(&ControlCenterConfig::showAvatarBorder, "show_avatar_border"),
        field(&ControlCenterConfig::hiddenTabs, "hidden_tabs"),
        subTable(&ControlCenterConfig::calendarTab, "calendar", calendarTabSchema()),
        subTable(&ControlCenterConfig::mediaTab, "media", mediaTabSchema()),
        arrayOf<ControlCenterConfig, ShortcutConfig>(
            &ControlCenterConfig::shortcuts, "shortcuts", shortcutSchema(),
            [](const ShortcutConfig& sc) { return !sc.type.empty(); }
        ),
    };
    return s;
  }

  namespace {
    // TOML key is "name" but the field is displayName.
    const Schema<CalendarConfig::Account>& calendarAccountSchema() {
      static const Schema<CalendarConfig::Account> s = {
          field(&CalendarConfig::Account::type, "type"),
          field(&CalendarConfig::Account::displayName, "name"),
          field(&CalendarConfig::Account::color, "color"),
          field(&CalendarConfig::Account::provider, "provider"),
          field(&CalendarConfig::Account::serverUrl, "server_url"),
          field(&CalendarConfig::Account::username, "username"),
          field(&CalendarConfig::Account::calendars, "calendars"),
          custom<CalendarConfig::Account>(
              "credential_source",
              [](const toml::table& table, CalendarConfig::Account& out, std::string_view parentPath,
                 Diagnostics& diag) {
                const auto value = table["credential_source"].value<std::string>();
                if (!value.has_value()) {
                  warnWrongType(table, "credential_source", parentPath, diag, "a string");
                  return;
                }
                const std::string source = StringUtils::trim(*value);
                if (source == "secret-service") {
                  out.credentialSource = CalendarCredentialSource::SecretService;
                } else if (source == "file") {
                  out.credentialSource = CalendarCredentialSource::File;
                } else {
                  diag.error(
                      joinPath(parentPath, "credential_source"),
                      R"(credential_source must be "secret-service" or "file")"
                  );
                }
              },
              [](toml::table& table, const CalendarConfig::Account& in) {
                table.insert_or_assign(
                    "credential_source",
                    in.credentialSource == CalendarCredentialSource::File ? "file" : "secret-service"
                );
              }
          ),
          pathStringField(&CalendarConfig::Account::passwordFile, "password_file"),
          pathStringField(&CalendarConfig::Account::clientCertFile, "client_cert_file"),
          pathStringField(&CalendarConfig::Account::clientKeyFile, "client_key_file"),
          pathStringField(&CalendarConfig::Account::path, "path"),
          finalize<CalendarConfig::Account>([](CalendarConfig::Account& out, std::string_view parentPath,
                                               Diagnostics& diag) {
            if (out.type != "caldav") {
              if (!out.clientCertFile.empty()) {
                diag.error(joinPath(parentPath, "client_cert_file"), "client_cert_file is only valid for caldav");
              }
              if (!out.clientKeyFile.empty()) {
                diag.error(joinPath(parentPath, "client_key_file"), "client_key_file is only valid for caldav");
              }
            }
            if (out.type == "vdir") {
              if (out.credentialSource != CalendarCredentialSource::SecretService) {
                diag.error(joinPath(parentPath, "credential_source"), "credential_source is only valid for caldav");
              }
              if (!out.passwordFile.empty()) {
                diag.error(joinPath(parentPath, "password_file"), "password_file is only valid for caldav");
              }
              if (!out.username.empty()) {
                diag.error(joinPath(parentPath, "username"), "username is only valid for caldav");
              }
              if (!out.provider.empty()) {
                diag.error(joinPath(parentPath, "provider"), "provider is only valid for caldav");
              }
              if (!out.serverUrl.empty()) {
                diag.error(joinPath(parentPath, "server_url"), "server_url is not used for vdir accounts (use path)");
              }
              return;
            }
            if (out.type == "ics") {
              if (out.serverUrl.empty()) {
                diag.error(joinPath(parentPath, "server_url"), "ics accounts require server_url (.ics file URL)");
              }
              if (out.credentialSource != CalendarCredentialSource::SecretService) {
                diag.error(joinPath(parentPath, "credential_source"), "credential_source is only valid for caldav");
              }
              if (!out.passwordFile.empty()) {
                diag.error(joinPath(parentPath, "password_file"), "password_file is only valid for caldav");
              }
              if (!out.username.empty()) {
                diag.error(joinPath(parentPath, "username"), "username is only valid for caldav");
              }
              if (!out.provider.empty()) {
                diag.error(joinPath(parentPath, "provider"), "provider is only valid for caldav");
              }
              return;
            }
            if (out.type != "caldav") {
              if (out.credentialSource != CalendarCredentialSource::SecretService) {
                diag.error(joinPath(parentPath, "credential_source"), "credential_source is only valid for caldav");
              }
              if (!out.passwordFile.empty()) {
                diag.error(joinPath(parentPath, "password_file"), "password_file is only valid for caldav");
              }
              return;
            }
            if (out.credentialSource == CalendarCredentialSource::File) {
              if (out.passwordFile.empty()) {
                diag.error(
                    joinPath(parentPath, "password_file"),
                    R"(caldav accounts with credential_source = "file" require password_file)"
                );
              } else if (!std::filesystem::path(out.passwordFile).is_absolute()) {
                diag.error(joinPath(parentPath, "password_file"), "password_file must resolve to an absolute path");
              }
            } else if (!out.passwordFile.empty()) {
              diag.error(joinPath(parentPath, "password_file"), R"(password_file requires credential_source = "file")");
            }
            if (!out.clientCertFile.empty() || !out.clientKeyFile.empty()) {
              if (out.clientCertFile.empty()) {
                diag.error(joinPath(parentPath, "client_key_file"), "client_key_file without client_cert_file");
              } else if (!std::filesystem::path(out.clientCertFile).is_absolute()) {
                diag.error(
                    joinPath(parentPath, "client_cert_file"), "client_cert_file must resolve to an absolute path"
                );
              }
              if (out.clientKeyFile.empty()) {
                diag.error(joinPath(parentPath, "client_cert_file"), "client_cert_file without client_key_file");
              } else if (!std::filesystem::path(out.clientKeyFile).is_absolute()) {
                diag.error(joinPath(parentPath, "client_key_file"), "client_key_file must resolve to an absolute path");
              }
            }
            if (out.provider.empty()) {
              diag.error(
                  joinPath(parentPath, "provider"), R"(caldav accounts require provider = "icloud" or "custom")"
              );
              return;
            }
            if (out.provider == "icloud") {
              if (!out.serverUrl.empty()) {
                diag.error(joinPath(parentPath, "server_url"), "icloud accounts use the built-in CalDAV server URL");
              }
              return;
            }
            if (out.provider == "custom") {
              if (out.serverUrl.empty()) {
                diag.error(joinPath(parentPath, "server_url"), "custom caldav accounts require server_url");
              }
              return;
            }
            diag.error(joinPath(parentPath, "provider"), "unknown caldav provider \"" + out.provider + "\"");
          }),
      };
      return s;
    }
  } // namespace

  const Schema<CalendarConfig::Reminders>& calendarRemindersSchema() {
    static const Schema<CalendarConfig::Reminders> s = {
        field(&CalendarConfig::Reminders::enabled, "enabled"),
        field(&CalendarConfig::Reminders::useEventReminders, "use_event_reminders"),
        field(&CalendarConfig::Reminders::defaultLeadMinutes, "default_lead_minutes", kReminderLeadMinutesRange),
        field(&CalendarConfig::Reminders::allDayDigestTime, "all_day_digest_time"),
    };
    return s;
  }

  const Schema<CalendarConfig>& calendarSchema() {
    static const Schema<CalendarConfig> s = {
        field(&CalendarConfig::enabled, "enabled"),
        field(&CalendarConfig::dedupeEvents, "dedupe_events"),
        field(&CalendarConfig::dedupeIgnorePatterns, "dedupe_ignore_patterns"),
        field(&CalendarConfig::refreshMinutes, "refresh_minutes", kRefreshMinutesRange),
        field(&CalendarConfig::eventDateFormat, "event_date_format"),
        field(&CalendarConfig::eventTimeFormat, "event_time_format"),
        subTable(&CalendarConfig::reminders, "reminders", calendarRemindersSchema()),
        namedMap<CalendarConfig, CalendarConfig::Account>(
            &CalendarConfig::accounts, "account", calendarAccountSchema(),
            [](CalendarConfig::Account& a, std::string_view id) { a.id = std::string(id); },
            [](const CalendarConfig::Account& a) { return a.id; }, true
        ),
    };
    return s;
  }

} // namespace noctalia::config::schema
