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
  assert(
      permanentDeletePrompt({"/tmp/one.txt"}, 2)
      == "Permanently delete 2 selected items? This cannot be undone."
  );
  assert(
      permanentDeletePrompt({"/tmp/one.txt", "/tmp/two.txt", "/tmp/three.txt"}, 3)
      == "Permanently delete 3 selected items? This cannot be undone."
  );
  return 0;
}
