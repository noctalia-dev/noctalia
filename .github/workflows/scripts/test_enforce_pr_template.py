from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path
from unittest import mock


VALIDATOR_PATH = Path(__file__).with_name("enforce-pr-template.py")
TEMPLATE_PATH = Path(__file__).parents[2] / "PULL_REQUEST_TEMPLATE.md"
SPEC = importlib.util.spec_from_file_location("enforce_pr_template", VALIDATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
enforce_pr_template = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(enforce_pr_template)


def ready_template() -> str:
    """The template as a contributor leaves it before requesting review."""
    body = TEMPLATE_PATH.read_text().replace("- [ ]", "- [x]")
    for item in enforce_pr_template.TYPE_CHANGE_ITEMS[1:]:
        body = body.replace(f"- [x] {item}", f"- [ ] {item}")
    return body


def drop_section(body: str, heading: str) -> str:
    kept: list[str] = []
    skipping = False
    for line in body.splitlines(keepends=True):
        if line.strip() == heading:
            skipping = True
            continue
        if skipping and line.startswith("## "):
            skipping = False
        if not skipping:
            kept.append(line)
    return "".join(kept)


class TemplateValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.template = TEMPLATE_PATH.read_text()

    def test_accepts_canonical_template(self) -> None:
        self.assertEqual(enforce_pr_template.missing_requirements(self.template), [])

    def test_accepts_completed_ready_template(self) -> None:
        self.assertEqual(
            enforce_pr_template.missing_requirements(
                ready_template(),
                require_completed=True,
            ),
            [],
        )

    def test_accepts_ready_template_without_context_sections(self) -> None:
        """Sections that only offer context may be deleted."""
        body = ready_template()
        for heading in (
            "## Related Issue",
            "## Manual Coverage",
            "## Screenshots / Videos",
            "## Additional Notes",
        ):
            body = drop_section(body, heading)
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            [],
        )

    def test_accepts_ready_template_without_inapplicable_change_types(self) -> None:
        body = ready_template()
        for item in enforce_pr_template.TYPE_CHANGE_ITEMS[1:]:
            body = body.replace(f"- [ ] {item}\n", "")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            [],
        )

    def test_accepts_annotated_change_type(self) -> None:
        """A contributor may qualify a checked box."""
        body = ready_template().replace("- [x] Bug fix", "- [x] Bug fix (Non SystemD)")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            [],
        )

    def test_accepts_asterisk_bullets(self) -> None:
        body = ready_template().replace("- [", "* [")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            [],
        )

    def test_accepts_template_line_wrapping(self) -> None:
        wrapped = self.template.replace(
            "- [ ] I ran the relevant build or test commands, or explained why they were not run.",
            "- [ ] I ran the relevant build or test commands, or explained why they were\n      not run.",
        )
        self.assertEqual(enforce_pr_template.missing_requirements(wrapped), [])

    def test_accepts_body_stripped_of_guidance_comments(self) -> None:
        stripped = re.sub(r"<!--.*?-->", "", self.template, flags=re.DOTALL)
        self.assertEqual(enforce_pr_template.missing_requirements(stripped), [])

    def test_rejects_missing_testing_section(self) -> None:
        body = drop_section(ready_template(), "## Testing")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            ["the `## Testing` heading"],
        )

    def test_rejects_missing_motivation_section(self) -> None:
        body = drop_section(ready_template(), "## Motivation")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            ["the `## Motivation` heading"],
        )

    def test_rejects_renamed_section(self) -> None:
        body = self.template.replace("## Summary", "## Overview")
        self.assertIn(
            "the `## Summary` heading",
            enforce_pr_template.missing_requirements(body),
        )

    def test_rejects_ready_template_without_a_checked_change_type(self) -> None:
        body = ready_template().replace("- [x] Bug fix", "- [ ] Bug fix")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            ["at least one checked change type"],
        )

    def test_ready_template_requires_every_mandatory_item(self) -> None:
        item = enforce_pr_template.MANDATORY_CHECKLIST_ITEMS[3]
        body = ready_template().replace(f"- [x] {item}", f"- [ ] {item}")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            [f"the checked checklist entry: {item}"],
        )

    def test_draft_allows_unchecked_mandatory_items(self) -> None:
        self.assertEqual(
            enforce_pr_template.missing_requirements(
                self.template,
                require_completed=False,
            ),
            [],
        )

    def test_rejects_deleted_checklist_item(self) -> None:
        item = enforce_pr_template.MANDATORY_CHECKLIST_ITEMS[5]
        body = ready_template().replace(f"- [x] {item}\n", "")
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            [f"the checklist entry: {item}"],
        )

    def test_rejects_altered_checklist_item(self) -> None:
        body = ready_template().replace(
            "- [x] I self-reviewed the changes.",
            "- [x] I mostly self-reviewed the changes.",
        )
        self.assertEqual(
            enforce_pr_template.missing_requirements(body, require_completed=True),
            ["the checklist entry: I self-reviewed the changes."],
        )


class TemplateEnforcementTests(unittest.TestCase):
    ISSUE_URL = "https://api.github.test/repos/noctalia-dev/noctalia/issues/123"
    PULL_REQUEST_URL = "https://api.github.test/repos/noctalia-dev/noctalia/pulls/123"
    GRAPHQL_URL = "https://api.github.test/graphql"
    COMMENT_URL = "https://api.github.test/repos/noctalia-dev/noctalia/issues/comments/7"
    NODE_ID = "PR_node123"

    def event(self, body: str, *, draft: bool = False) -> dict[str, object]:
        return {
            "pull_request": {
                "body": body,
                "draft": draft,
                "issue_url": self.ISSUE_URL,
                "url": self.PULL_REQUEST_URL,
                "node_id": self.NODE_ID,
            }
        }

    def comments_call(self, page: int = 1) -> mock._Call:
        return mock.call(f"{self.ISSUE_URL}/comments?per_page=100&page={page}", "token")

    def draft_call(self) -> mock._Call:
        return mock.call(
            self.GRAPHQL_URL,
            "token",
            method="POST",
            payload={"query": mock.ANY, "variables": {"id": self.NODE_ID}},
        )

    def test_valid_template_writes_nothing_when_no_comment_exists(self) -> None:
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[[]],
        ) as request:
            self.assertEqual(
                enforce_pr_template.enforce(self.event(ready_template()), "token"),
                [],
            )
        self.assertEqual(request.call_args_list, [self.comments_call()])

    def test_valid_template_resolves_an_existing_enforcement_comment(self) -> None:
        stale = {
            "body": enforce_pr_template.build_enforcement_comment(
                ["the `## Testing` heading"],
                converted=False,
            ),
            "url": self.COMMENT_URL,
        }
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[[stale], {}],
        ) as request:
            self.assertEqual(
                enforce_pr_template.enforce(self.event(ready_template()), "token"),
                [],
            )
        self.assertEqual(
            request.call_args_list,
            [
                self.comments_call(),
                mock.call(
                    self.COMMENT_URL,
                    "token",
                    method="PATCH",
                    payload={"body": enforce_pr_template.RESOLVED_COMMENT},
                ),
            ],
        )

    def test_invalid_ready_pull_request_is_converted_then_gets_fresh_comment(self) -> None:
        body = drop_section(ready_template(), "## Testing")
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[
                {"data": {"convertPullRequestToDraft": {"pullRequest": {"isDraft": True}}}},
                {},
            ],
        ) as request:
            missing = enforce_pr_template.enforce(self.event(body), "token")

        self.assertEqual(missing, ["the `## Testing` heading"])
        comment = enforce_pr_template.build_enforcement_comment(
            missing,
            converted=True,
        )
        self.assertIn("converted to a draft", comment)
        self.assertEqual(
            request.call_args_list,
            [
                self.draft_call(),
                mock.call(
                    f"{self.ISSUE_URL}/comments",
                    "token",
                    method="POST",
                    payload={"body": comment},
                ),
            ],
        )

    def test_each_conversion_posts_a_fresh_comment_without_reading_old_comments(
        self,
    ) -> None:
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[
                {"data": {"convertPullRequestToDraft": {"pullRequest": {"isDraft": True}}}},
                {},
            ],
        ) as request:
            enforce_pr_template.enforce(self.event("AI-generated body"), "token")

        self.assertEqual(request.call_args_list[0], self.draft_call())
        self.assertEqual(
            request.call_args_list[1].args[0],
            f"{self.ISSUE_URL}/comments",
        )

    def test_invalid_draft_updates_the_latest_enforcement_comment(self) -> None:
        stale = {
            "body": enforce_pr_template.build_enforcement_comment(
                ["at least one checked change type"],
                converted=True,
            ),
            "url": self.COMMENT_URL,
        }
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[[stale], {}],
        ) as request:
            missing = enforce_pr_template.enforce(
                self.event("AI-generated body", draft=True),
                "token",
            )

        self.assertIn("the `## Summary` heading", missing)
        comment = enforce_pr_template.build_enforcement_comment(
            missing,
            converted=False,
        )
        self.assertEqual(
            request.call_args_list,
            [
                self.comments_call(),
                mock.call(
                    self.COMMENT_URL,
                    "token",
                    method="PATCH",
                    payload={"body": comment},
                ),
            ],
        )

    def test_identical_draft_comment_is_not_rewritten(self) -> None:
        missing = enforce_pr_template.missing_requirements(
            "AI-generated body",
            require_completed=False,
        )
        existing = {
            "body": enforce_pr_template.build_enforcement_comment(
                missing,
                converted=False,
            ),
            "url": self.COMMENT_URL,
        }
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[[existing]],
        ) as request:
            enforce_pr_template.enforce(
                self.event("AI-generated body", draft=True),
                "token",
            )

        self.assertEqual(request.call_args_list, [self.comments_call()])

    def test_graphql_error_stops_before_posting_conversion_comment(self) -> None:
        with mock.patch.object(
            enforce_pr_template,
            "github_request",
            side_effect=[{"errors": [{"message": "Resource not accessible"}]}],
        ) as request:
            with self.assertRaises(RuntimeError):
                enforce_pr_template.enforce(self.event("AI-generated body"), "token")

        self.assertEqual(request.call_args_list, [self.draft_call()])


if __name__ == "__main__":
    unittest.main()
