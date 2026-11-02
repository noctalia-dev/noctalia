#include "shell/settings/font_family_catalog.h"
#include "test_check.h"

#include <string>
#include <vector>

int main() {
  TEST_CHECK(
      settings::parseFontFamilyList("JetBrainsMono Nerd Font\n") == std::vector<std::string>{"JetBrainsMono Nerd Font"}
  );
  TEST_CHECK((
      settings::parseFontFamilyList("FUNK\\-JIVE\n7\\-Segment\n") == std::vector<std::string>{"7-Segment", "FUNK-JIVE"}
  ));
  TEST_CHECK((settings::parseFontFamilyList("Foo\\, Bar,Baz\n") == std::vector<std::string>{"Baz", "Foo, Bar"}));
}
