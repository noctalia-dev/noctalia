"""Regression tests for the palette alpha source check."""

import importlib.util
import pathlib
import unittest

SPEC = importlib.util.spec_from_file_location(
    "role_alpha_check", pathlib.Path(__file__).with_name("role-alpha-check.py")
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class RoleAlphaCheckTests(unittest.TestCase):
    def test_literal_alpha_with_computed_or_qualified_roles(self):
        source = """colorSpecFromRole(activeRole, (0.7f));
ui::colorForRole(pickRole(flag, fallback), .45e0F);
colorForRole(active ? ColorRole::Error : ColorRole::Secondary, 0x1.p-1f);
colorForRole(role, +0.5'0F);
"""
        self.assertEqual(CHECK.scan(source), ([1, 2, 3, 4], [], []))

    def test_literal_in_either_branch_of_a_conditional(self):
        source = """colorForRole(role, config.enabled ? opacity : .5f);
colorForRole(role, enabled() ? (selected ? .8F : .4F) : opacity);
colorForRole(role, opt::enabled ? Style::hintAlpha : 0.60);
"""
        self.assertEqual(CHECK.scan(source), ([1, 2, 3], [], []))

    def test_named_opacity_and_transparent_or_unmodified_states(self):
        source = """colorForRole(role, Style::disabledForegroundAlpha);
colorSpecFromRole(role, enabled ? 1.0F : Style::disabledForegroundAlpha);
colorSpecFromRole(role, wrap ? 0.0F : config.backgroundOpacity);
colorSpecFromRole(role, visible ? 1.0F : 0.0F);
"""
        self.assertEqual(CHECK.scan(source), ([], [], []))

    def test_comments_and_strings_are_not_calls(self):
        source = """// colorSpecFromRole(role, 0.5F);
/* colorForRole(role, 0.5F); */
auto text = "colorSpecFromRole(role, 0.5F);";
auto example = R"cpp(colorSpecFromRole(role, 0.5F);)cpp";
colorForRole(role, 0.5F);
"""
        self.assertEqual(CHECK.scan(source), ([5], [], []))

    def test_a_reason_only_exempts_its_own_argument(self):
        source = """colorForRole(role, 0.5F /* palette-alpha-exception: Rim blends
  into the existing glass. */);
colorForRole(role, 0.5F);
"""
        self.assertEqual(
            CHECK.scan(source), ([3], [(1, "Rim blends into the existing glass.")], [])
        )

    def test_empty_detached_duplicate_or_stale_exceptions_are_rejected(self):
        source = """// palette-alpha-exception: Not attached to an argument.
colorForRole(role, 0.5F /* palette-alpha-exception:  */);
colorForRole(role, opacity /* palette-alpha-exception: Now unnecessary. */);
colorForRole(role, 0.5F /* palette-alpha-exception: First. */ /* palette-alpha-exception: Second. */);
colorForRole(role, /* palette-alpha-exception: Before the value. */ 0.5F);
"""
        self.assertEqual(CHECK.scan(source), ([2, 4, 5], [], [1, 2, 3, 4, 4, 5]))


if __name__ == "__main__":
    unittest.main()
