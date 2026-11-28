#include "shell/tray/tray_identifier.h"
#include "shell/tray/tray_tooltip_markup.h"
#include "tests/test_check.h"

#include <glib.h>
#include <pango/pango.h>
#include <string>
#include <string_view>

namespace {

  void checkValidPangoMarkup(std::string_view markup, std::string_view expectedPlain) {
    PangoAttrList* attrs = nullptr;
    char* plain = nullptr;
    GError* err = nullptr;
    const gboolean ok =
        pango_parse_markup(markup.data(), static_cast<int>(markup.size()), 0, &attrs, &plain, nullptr, &err);
    if (err != nullptr) {
      g_error_free(err);
    }
    TEST_CHECK(ok != FALSE);
    TEST_CHECK(plain != nullptr);
    TEST_CHECK(std::string_view(plain) == expectedPlain);
    if (attrs != nullptr) {
      pango_attr_list_unref(attrs);
    }
    g_free(plain);
  }

} // namespace

int main() {
  TEST_CHECK(tray::escapePangoMarkup("A & B < C > D") == "A &amp; B &lt; C &gt; D");

  {
    const auto kclock = tray::htmlToPangoMarkup(
        "<html>Alarm: <b>Wednesday 8:40\xE2\x80\xAF"
        "AM</b></html>"
    );
    TEST_CHECK(kclock.hasFormatting);
    TEST_CHECK(
        kclock.markup
        == "Alarm: <b>Wednesday 8:40\xE2\x80\xAF"
           "AM</b>"
    );
    TEST_CHECK(
        kclock.plainText
        == "Alarm: Wednesday 8:40\xE2\x80\xAF"
           "AM"
    );
    checkValidPangoMarkup(kclock.markup, kclock.plainText);
  }
  {
    const auto common = tray::htmlToPangoMarkup(
        R"(<strong>B</strong> <em>I</em> <u>U</u> <del>S</del> <code>C</code> <sup>1</sup><sub>2</sub> )"
        R"(<small>sm</small> <big>lg</big> <a href="https://example.com">link</a>)"
    );
    TEST_CHECK(common.hasFormatting);
    TEST_CHECK(
        common.markup
        == "<b>B</b> <i>I</i> <u>U</u> <s>S</s> <tt>C</tt> <sup>1</sup><sub>2</sub> <small>sm</small> <big>lg</big> "
           "<u>link</u>"
    );
    TEST_CHECK(common.plainText == "B I U S C 12 sm lg link");
    checkValidPangoMarkup(common.markup, common.plainText);
  }
  {
    // Uncommon tags stripped while preserving inner text; head/style/script and comments dropped.
    const auto stripped = tray::htmlToPangoMarkup(
        R"(<!DOCTYPE html><html><head><meta name="qrichtext" content="1" /><style type="text/css">)"
        R"(p { white-space: pre-wrap; }</style><script>alert(1)</script></head><body><!-- comment -->)"
        R"(<qt><center><font color="red"><span class="x">Status</span></font></center></qt>)"
        R"(<img src="/tmp/icon.png" alt="" /><img src="/tmp/icon2.png" /></body></html>)"
    );
    TEST_CHECK(!stripped.hasFormatting);
    TEST_CHECK(stripped.markup == "Status");
    TEST_CHECK(stripped.plainText == "Status");
    checkValidPangoMarkup(stripped.markup, stripped.plainText);

    // <img> emits its decoded alt text as fallback when non-empty.
    const auto imgAlt = tray::htmlToPangoMarkup(R"(<img src="/tmp/alarm.png" alt="Alarm &amp; Timer" /> <b>8:40</b>)");
    TEST_CHECK(imgAlt.hasFormatting);
    TEST_CHECK(imgAlt.plainText == "Alarm & Timer 8:40");
    TEST_CHECK(imgAlt.markup == "Alarm &amp; Timer <b>8:40</b>");
    checkValidPangoMarkup(imgAlt.markup, imgAlt.plainText);

    // Unclosed hidden block drops payload through end-of-input rather than leaking CSS/JS.
    const auto unclosedStyle = tray::htmlToPangoMarkup("Before <style>p { color: red; }");
    TEST_CHECK(!unclosedStyle.hasFormatting);
    TEST_CHECK(unclosedStyle.plainText == "Before");
    checkValidPangoMarkup(unclosedStyle.markup, unclosedStyle.plainText);
  }
  {
    // Adversarial repeated unclosed delimiters stay linear and valid.
    std::string adversarial;
    adversarial.reserve(120000);
    for (int k = 0; k < 5000; ++k) {
      adversarial += "<!--<?<!a <a x=\"";
    }
    const auto res = tray::htmlToPangoMarkup(adversarial);
    TEST_CHECK(!res.hasFormatting);
    TEST_CHECK(res.plainText == adversarial);
    checkValidPangoMarkup(res.markup, res.plainText);
  }
  {
    // Misnested and unclosed tags always yield balanced Pango markup.
    const auto misnested = tray::htmlToPangoMarkup("<b>bold <i>both</b> italic</i><b>open");
    TEST_CHECK(misnested.hasFormatting);
    TEST_CHECK(misnested.markup == "<b>bold <i>both</i></b><i> italic</i><b>open</b>");
    TEST_CHECK(misnested.plainText == "bold both italicopen");
    checkValidPangoMarkup(misnested.markup, misnested.plainText);
  }
  {
    // Duplicate nested tags, stray closing tags, and empty tags do not emit empty or broken markup.
    const auto clean = tray::htmlToPangoMarkup("<b>a<b>b</b>c</b></b><i></i>");
    TEST_CHECK(clean.hasFormatting);
    TEST_CHECK(clean.markup == "<b>abc</b>");
    TEST_CHECK(clean.plainText == "abc");
    checkValidPangoMarkup(clean.markup, clean.plainText);

    const auto onlyEmpty = tray::htmlToPangoMarkup("<html><b>  </b></html>");
    TEST_CHECK(!onlyEmpty.hasFormatting);
    TEST_CHECK(onlyEmpty.markup.empty());
    TEST_CHECK(onlyEmpty.plainText.empty());
  }
  {
    // Non-tag angle brackets and HTML entities are preserved and Pango-escaped.
    const auto literals = tray::htmlToPangoMarkup(
        "Upload < 1 MB/s & Download > 5 MB/s <.< wawd :> <user@example.com> <10ms> "
        "<b && c> &lt;b&gt;R&amp;D&#39;s &#x2013; 10&deg;&lt;/b&gt;"
    );
    TEST_CHECK(!literals.hasFormatting);
    TEST_CHECK(
        literals.plainText
        == "Upload < 1 MB/s & Download > 5 MB/s <.< wawd :> <user@example.com> <10ms> "
           "<b && c> <b>R&D's \xE2\x80\x93 10\xC2\xB0</b>"
    );
    TEST_CHECK(
        literals.markup
        == "Upload &lt; 1 MB/s &amp; Download &gt; 5 MB/s &lt;.&lt; wawd :&gt; &lt;user@example.com&gt; &lt;10ms&gt; "
           "&lt;b &amp;&amp; c&gt; &lt;b&gt;R&amp;D's \xE2\x80\x93 10\xC2\xB0&lt;/b&gt;"
    );
    checkValidPangoMarkup(literals.markup, literals.plainText);
  }
  {
    // Block tags, headings, line breaks, lists, and tables format cleanly without leading/trailing blank lines.
    const auto blocks = tray::htmlToPangoMarkup("<html>\n  <p>Line 1</p>\n  <p>Line 2</p>\n</html>");
    TEST_CHECK(blocks.plainText == "Line 1\nLine 2");
    TEST_CHECK(blocks.markup == "Line 1\nLine 2");
    checkValidPangoMarkup(blocks.markup, blocks.plainText);

    const auto heading = tray::htmlToPangoMarkup("<h3>Title</h3><p>Body</p>");
    TEST_CHECK(heading.hasFormatting);
    TEST_CHECK(heading.markup == "<b>Title</b>\nBody");
    TEST_CHECK(heading.plainText == "Title\nBody");
    checkValidPangoMarkup(heading.markup, heading.plainText);

    const auto br = tray::htmlToPangoMarkup("<b>Line 1</b><br>\n  Line 2<br><br>Line 3");
    TEST_CHECK(br.markup == "<b>Line 1</b>\nLine 2\n\nLine 3");
    checkValidPangoMarkup(br.markup, br.plainText);

    const auto list = tray::htmlToPangoMarkup("<ul><li><b>One</b></li><li></li><li>Two</li></ul>");
    TEST_CHECK(list.markup == "\xE2\x80\xA2 <b>One</b>\n\xE2\x80\xA2 Two");
    TEST_CHECK(list.plainText == "\xE2\x80\xA2 One\n\xE2\x80\xA2 Two");
    checkValidPangoMarkup(list.markup, list.plainText);

    const auto table =
        tray::htmlToPangoMarkup("<table><tr><th>Key</th><td><b>Val</b></td></tr><tr><td>A</td><td>B</td></tr></table>");
    TEST_CHECK(table.markup == "<b>Key</b> <b>Val</b>\nA B");
    TEST_CHECK(table.plainText == "Key Val\nA B");
    checkValidPangoMarkup(table.markup, table.plainText);
  }
  {
    TrayItemInfo kclock;
    kclock.title = "KClock daemon";
    kclock.statusNotifierTitle = "KClock";
    kclock.statusNotifierDescription = "<html>Alarm: <b>Wednesday 8:40\xE2\x80\xAF"
                                       "AM</b></html>";

    const auto rich = tray::formatTrayItemTooltipRich(kclock);
    TEST_CHECK(rich.isMarkup);
    TEST_CHECK(
        rich.text
        == "KClock daemon - Alarm: <b>Wednesday 8:40\xE2\x80\xAF"
           "AM</b>"
    );
    TEST_CHECK(
        rich.plainText
        == "KClock daemon - Alarm: Wednesday 8:40\xE2\x80\xAF"
           "AM"
    );
    TEST_CHECK(tray::formatTrayItemTooltip(kclock) == rich.plainText);
    checkValidPangoMarkup(rich.text, rich.plainText);
  }
  {
    // Prefix title containing '&' is Pango-escaped when paired with formatted description.
    TrayItemInfo amp;
    amp.title = "Q&A Helper";
    amp.statusNotifierDescription = "<i>Idle</i>";

    const auto rich = tray::formatTrayItemTooltipRich(amp);
    TEST_CHECK(rich.isMarkup);
    TEST_CHECK(rich.text == "Q&amp;A Helper - <i>Idle</i>");
    TEST_CHECK(rich.plainText == "Q&A Helper - Idle");
    checkValidPangoMarkup(rich.text, rich.plainText);
  }
  {
    // Empty HTML description falls back to statusNotifierTitle as literal plain text.
    TrayItemInfo fallback;
    fallback.title = "App";
    fallback.statusNotifierTitle = "Research <b>";
    fallback.statusNotifierDescription = "<html><br></html>";

    const auto rich = tray::formatTrayItemTooltipRich(fallback);
    TEST_CHECK(!rich.isMarkup);
    TEST_CHECK(rich.text == "App - Research <b>");
    TEST_CHECK(rich.plainText == "App - Research <b>");
  }
  {
    // Deduplication compares plain text so markup tags do not duplicate the app title.
    TrayItemInfo dedup;
    dedup.title = "Spotify";
    dedup.statusNotifierDescription = "<b>Spotify</b> - Playing";

    const auto rich = tray::formatTrayItemTooltipRich(dedup);
    TEST_CHECK(rich.isMarkup);
    TEST_CHECK(rich.text == "<b>Spotify</b> - Playing");
    TEST_CHECK(rich.plainText == "Spotify - Playing");
    checkValidPangoMarkup(rich.text, rich.plainText);
  }

  return 0;
}
