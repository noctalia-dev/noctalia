#include "filemanager/permanent_delete_prompt.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
  using filemanager::permanentDeletePrompt;
  assert(
      permanentDeletePrompt({"/tmp/very-long-example-name.txt"}, 1)
      == "Permanently delete \"very-long-example-name.txt\"? This cannot be undone."
  );
  assert(
      permanentDeletePrompt({"/tmp/one.txt", "/tmp/two.txt"}, 2)
      == "Permanently delete 2 selected items? This cannot be undone."
  );
  assert(permanentDeletePrompt({"/tmp/one.txt"}, 2) == "Permanently delete 2 selected items? This cannot be undone.");
  assert(
      permanentDeletePrompt({"/tmp/one.txt", "/tmp/two.txt", "/tmp/three.txt"}, 3)
      == "Permanently delete 3 selected items? This cannot be undone."
  );
  // Display sanitization escapes newlines, carriage returns, and Unicode bidi overrides.
  const std::string spoofed = permanentDeletePrompt(
      {"/tmp/invoice\n\r\xE2\x80\xAE"
       "fdp.exe"},
      1
  );
  assert(spoofed.find('\n') == std::string::npos);
  assert(spoofed.find('\r') == std::string::npos);
  assert(spoofed.find("\\n\\r<U+202E>fdp.exe") != std::string::npos);
  // Normal Unicode and emoji filenames remain legible.
  assert(
      permanentDeletePrompt({"/tmp/résumé-日本語-🚀.txt"}, 1)
      == "Permanently delete \"résumé-日本語-🚀.txt\"? This cannot be undone."
  );
  return 0;
}
