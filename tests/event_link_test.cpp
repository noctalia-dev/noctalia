#include "calendar/event_link.h"

#include <print>
#include <string>
#include <string_view>

namespace {

  bool expectLink(std::string_view location, std::string_view urlProperty, std::string_view expected) {
    const std::string actual = calendar::resolveEventLink(location, urlProperty);
    if (actual == expected) {
      return true;
    }
    std::println(
        stderr, R"(event_link_test: location="{}" url="{}" -> "{}", expected "{}")", location, urlProperty, actual,
        expected
    );
    return false;
  }

} // namespace

int main() {
  bool ok = true;

  // A bare link in LOCATION is the common Google/Outlook meeting form.
  ok = expectLink("https://meet.google.com/abc-defg-hij", "", "https://meet.google.com/abc-defg-hij") && ok;
  ok = expectLink("  https://meet.google.com/abc  ", "", "https://meet.google.com/abc") && ok;
  ok = expectLink("HTTPS://Meet.Example/x", "", "HTTPS://Meet.Example/x") && ok;
  ok = expectLink("http://intranet.local/room", "", "http://intranet.local/room") && ok;

  // Embedded in prose, with the punctuation that comes along with it.
  ok = expectLink("Room 3 / https://zoom.us/j/12345", "", "https://zoom.us/j/12345") && ok;
  ok = expectLink("Join: <https://meet.jit.si/x>.", "", "https://meet.jit.si/x") && ok;
  ok = expectLink("See (https://meet.example/x)", "", "https://meet.example/x") && ok;
  // Balanced parentheses belong to the link itself.
  ok = expectLink("https://wiki.example/Room_(main)", "", "https://wiki.example/Room_(main)") && ok;

  // Plain addresses yield nothing.
  ok = expectLink("Kyiv, Ukraine", "", "") && ok;
  ok = expectLink("", "", "") && ok;
  // Not a scheme match, and a scheme with no host.
  ok = expectLink("shttp://example.com/x", "", "") && ok;
  ok = expectLink("https:///no-host", "", "") && ok;

  // LOCATION wins over the URL property; the property is the fallback.
  ok = expectLink(
           "https://meet.google.com/abc", "https://calendar.google.com/event?eid=1", "https://meet.google.com/abc"
       )
      && ok;
  ok = expectLink("Room 3", "https://calendar.google.com/event?eid=1", "https://calendar.google.com/event?eid=1") && ok;
  ok = expectLink("Room 3", "  https://example.com/event  ", "https://example.com/event") && ok;

  // Only http(s) reaches xdg-open: everything else a hostile server could send is dropped.
  ok = expectLink("file:///home/user/.ssh/id_ed25519", "file:///home/user/.ssh/id_ed25519", "") && ok;
  ok = expectLink("data:text/html,<script>", "data:text/html,<script>", "") && ok;
  ok = expectLink("webcal://evil.example/x", "webcal://evil.example/x", "") && ok;
  ok = expectLink("mailto:someone@example.com", "mailto:someone@example.com", "") && ok;

  // Control characters and oversized values are rejected rather than truncated.
  ok = expectLink("", std::string("https://example.com/a\nb"), "") && ok;
  ok = expectLink("", std::string("https://example.com/a\tb"), "") && ok;
  ok = expectLink("", "https://example.com/" + std::string(2048, 'a'), "") && ok;

  // A rejected first candidate does not hide a valid one later in the field.
  ok = expectLink("https:// https://meet.example/x", "", "https://meet.example/x") && ok;

  return ok ? 0 : 1;
}
