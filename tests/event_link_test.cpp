#include "calendar/event_link.h"
#include "tests/test_check.h"

#include <string>

int main() {
  // A bare link in LOCATION is the common Google/Outlook meeting form.
  TEST_CHECK(
      calendar::resolveEventLink("https://meet.google.com/abc-defg-hij", {}, "")
      == "https://meet.google.com/abc-defg-hij"
  );
  TEST_CHECK(calendar::resolveEventLink("  https://meet.google.com/abc  ", {}, "") == "https://meet.google.com/abc");
  TEST_CHECK(calendar::resolveEventLink("HTTPS://Meet.Example/x", {}, "") == "HTTPS://Meet.Example/x");
  TEST_CHECK(calendar::resolveEventLink("http://intranet.local/room", {}, "") == "http://intranet.local/room");

  // Embedded in prose, with the punctuation that comes along with it.
  TEST_CHECK(calendar::resolveEventLink("Room 3 / https://zoom.us/j/12345", {}, "") == "https://zoom.us/j/12345");
  TEST_CHECK(calendar::resolveEventLink("Join: <https://meet.jit.si/x>.", {}, "") == "https://meet.jit.si/x");
  TEST_CHECK(calendar::resolveEventLink("See (https://meet.example/x)", {}, "") == "https://meet.example/x");
  // Balanced parentheses belong to the link itself.
  TEST_CHECK(
      calendar::resolveEventLink("https://wiki.example/Room_(main)", {}, "") == "https://wiki.example/Room_(main)"
  );

  // Plain addresses yield nothing.
  TEST_CHECK(calendar::resolveEventLink("Kyiv, Ukraine", {}, "").empty());
  TEST_CHECK(calendar::resolveEventLink("", {}, "").empty());
  // Not a scheme match, and a scheme with no host.
  TEST_CHECK(calendar::resolveEventLink("shttp://example.com/x", {}, "").empty());
  TEST_CHECK(calendar::resolveEventLink("https:///no-host", {}, "").empty());

  // LOCATION wins over the URL property; the property is the fallback.
  TEST_CHECK(
      calendar::resolveEventLink("https://meet.google.com/abc", {}, "https://calendar.google.com/event?eid=1")
      == "https://meet.google.com/abc"
  );
  TEST_CHECK(
      calendar::resolveEventLink("Room 3", {}, "https://calendar.google.com/event?eid=1")
      == "https://calendar.google.com/event?eid=1"
  );
  TEST_CHECK(calendar::resolveEventLink("Room 3", {}, "  https://example.com/event  ") == "https://example.com/event");

  // Outlook writes Teams joins into DESCRIPTION as an HTML anchor, while URL commonly points to an event page.
  TEST_CHECK(
      calendar::resolveEventLink(
          "Microsoft Teams Meeting",
          R"(Join now: <a href="https://teams.microsoft.com/l/meetup-join/19%3ameeting_abc%40thread.v2/0?context=%7b%7d">Join</a>)",
          "https://outlook.office.com/calendar/item/3"
      )
      == "https://teams.microsoft.com/l/meetup-join/19%3ameeting_abc%40thread.v2/0?context=%7b%7d"
  );

  // Only http(s) reaches xdg-open: everything else a hostile server could send is dropped.
  TEST_CHECK(
      calendar::resolveEventLink("file:///home/user/.ssh/id_ed25519", {}, "file:///home/user/.ssh/id_ed25519").empty()
  );
  TEST_CHECK(calendar::resolveEventLink("data:text/html,<script>", {}, "data:text/html,<script>").empty());
  TEST_CHECK(calendar::resolveEventLink("webcal://evil.example/x", {}, "webcal://evil.example/x").empty());
  TEST_CHECK(calendar::resolveEventLink("mailto:someone@example.com", {}, "mailto:someone@example.com").empty());

  // Control characters and oversized values are rejected rather than truncated.
  TEST_CHECK(calendar::resolveEventLink("", {}, "https://example.com/a\nb").empty());
  TEST_CHECK(calendar::resolveEventLink("", {}, "https://example.com/a\tb").empty());
  TEST_CHECK(calendar::resolveEventLink("", {}, "https://example.com/" + std::string(2048, 'a')).empty());

  // A rejected first candidate does not hide a valid one later in the field.
  TEST_CHECK(calendar::resolveEventLink("https:// https://meet.example/x", {}, "") == "https://meet.example/x");

  return 0;
}
