#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


COMMENT_MARKER = "<!-- noctalia-pr-template-enforcement -->"
TEMPLATE_URL = (
    "https://github.com/noctalia-dev/noctalia/blob/main/.github/PULL_REQUEST_TEMPLATE.md"
)
REQUIRED_HEADINGS = (
    "## Summary",
    "## Motivation",
    "## Type of Change",
    "## Testing",
    "## Checklist",
)
TYPE_CHANGE_ITEMS = (
    "Bug fix",
    "New feature",
    "Breaking change",
    "Refactoring",
    "Build / packaging",
)
MANDATORY_CHECKLIST_ITEMS = (
    "This PR is ready for review, or it is marked as Draft.",
    "I read and followed the relevant guidance in `CONTRIBUTING.md`.",
    "I ran `just format` with clang-format v22+ installed, or this PR has no code changes.",
    "I ran the relevant build or test commands, or explained why they were not run.",
    "I self-reviewed the changes.",
    "I checked for new warnings or errors.",
    "I updated user-facing documentation in [`docs/user/`](../docs/user/) when this PR changes documented behavior or configuration, or this PR does not require documentation changes.",
    "I added or updated `assets/translations/en.json`, or this PR adds no new user-facing strings.",
    "I did not edit non-English translation files unless this PR is explicitly for translation tooling, an import/export sync, or a maintainer-requested locale change.",
    "I used the existing canonical names for config keys, IPC names, paths, and identifiers.",
)
CONVERTED_INTRO = f"""{COMMENT_MARKER}
This pull request was converted to a draft because its description is missing required
parts of [the pull request template]({TEMPLATE_URL}).

Missing:
"""
DRAFT_INTRO = f"""{COMMENT_MARKER}
This draft pull request is missing required parts of
[the pull request template]({TEMPLATE_URL}).

Missing:
"""
OUTRO = """
Add the items above to the description, keeping their exact wording, then mark the pull
request ready for review. That re-runs this check. Draft pull requests may leave boxes
unchecked. Before a pull request is ready for review, select at least one change type and
check every item under Checklist.

Sections that only offer context may be deleted; nothing else about this pull request was
changed.
"""
RESOLVED_COMMENT = f"""{COMMENT_MARKER}
The description now contains the required template structure.
"""


def build_enforcement_comment(missing: list[str], *, converted: bool) -> str:
    bullets = "".join(f"- {item}\n" for item in missing)
    intro = CONVERTED_INTRO if converted else DRAFT_INTRO
    return f"{intro}{bullets}{OUTRO}"


def checklist_state(normalized_body: str, item: str) -> str | None:
    for bullet in ("-", "*"):
        for state in (" ", "x", "X"):
            if f"{bullet} [{state}] {item}" in normalized_body:
                return state
    return None


def missing_requirements(body: object, *, require_completed: bool = False) -> list[str]:
    if not isinstance(body, str):
        body = ""

    lines = {line.strip() for line in body.splitlines()}
    normalized_body = " ".join(body.split())
    missing: list[str] = []

    for heading in REQUIRED_HEADINGS:
        if heading not in lines:
            missing.append(f"the `{heading}` heading")

    states = {
        item: checklist_state(normalized_body, item)
        for item in MANDATORY_CHECKLIST_ITEMS
    }
    for item, state in states.items():
        if state is None:
            missing.append(f"the checklist entry: {item}")

    if not require_completed:
        return missing

    if not any(
        checklist_state(normalized_body, item) in ("x", "X")
        for item in TYPE_CHANGE_ITEMS
    ):
        missing.append("at least one checked change type")

    for item in MANDATORY_CHECKLIST_ITEMS:
        if states[item] == " ":
            missing.append(f"the checked checklist entry: {item}")

    return missing


def github_request(
    url: str,
    token: str,
    *,
    method: str = "GET",
    payload: dict[str, object] | None = None,
) -> Any:
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "User-Agent": "noctalia-pr-template-enforcement",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        response_body = response.read()
    return json.loads(response_body) if response_body else None


def convert_to_draft(pull_request_url: str, node_id: str, token: str) -> None:
    api_root, separator, _ = pull_request_url.partition("/repos/")
    if not separator:
        raise ValueError("pull request URL does not point at a GitHub API host")
    result = github_request(
        f"{api_root}/graphql",
        token,
        method="POST",
        payload={
            "query": (
                "mutation($id:ID!)"
                "{convertPullRequestToDraft(input:{pullRequestId:$id})"
                "{pullRequest{isDraft}}}"
            ),
            "variables": {"id": node_id},
        },
    )
    if not isinstance(result, dict):
        raise RuntimeError("GitHub returned an invalid draft conversion response")
    errors = result.get("errors")
    if errors:
        raise RuntimeError(f"GitHub refused to convert the pull request to a draft: {errors}")
    converted = result.get("data", {}).get("convertPullRequestToDraft", {})
    pull_request = converted.get("pullRequest") if isinstance(converted, dict) else None
    if not isinstance(pull_request, dict) or pull_request.get("isDraft") is not True:
        raise RuntimeError("GitHub did not confirm that the pull request became a draft")


def latest_enforcement_comment(issue_url: str, token: str) -> dict[str, Any] | None:
    page = 1
    latest: dict[str, Any] | None = None
    while True:
        comments = github_request(
            f"{issue_url}/comments?per_page=100&page={page}",
            token,
        )
        if not isinstance(comments, list):
            raise RuntimeError("GitHub returned an invalid pull request comment list")
        for comment in comments:
            if isinstance(comment, dict) and COMMENT_MARKER in str(comment.get("body", "")):
                latest = comment
        if len(comments) < 100:
            return latest
        page += 1


def sync_enforcement_comment(issue_url: str, token: str, comment: str) -> None:
    """Keep the latest bot comment current while a pull request remains a draft."""
    existing = latest_enforcement_comment(issue_url, token)
    if existing is None:
        if comment == RESOLVED_COMMENT:
            return
        github_request(
            f"{issue_url}/comments",
            token,
            method="POST",
            payload={"body": comment},
        )
        return
    if str(existing.get("body", "")) == comment:
        return
    comment_url = existing.get("url")
    if not isinstance(comment_url, str):
        raise RuntimeError("GitHub comment is missing its API URL")
    github_request(comment_url, token, method="PATCH", payload={"body": comment})


def enforce(event: dict[str, object], token: str) -> list[str]:
    pull_request = event.get("pull_request")
    if not isinstance(pull_request, dict):
        raise ValueError("event does not contain a pull_request object")

    is_draft = pull_request.get("draft") is True
    missing = missing_requirements(
        pull_request.get("body"),
        require_completed=not is_draft,
    )

    issue_url = pull_request.get("issue_url")
    pull_request_url = pull_request.get("url")
    node_id = pull_request.get("node_id")
    if not isinstance(issue_url, str) or not isinstance(pull_request_url, str):
        raise ValueError("pull request event is missing GitHub API URLs")
    if not token:
        raise ValueError("GITHUB_TOKEN is required to report on a pull request")

    if not missing:
        sync_enforcement_comment(issue_url, token, RESOLVED_COMMENT)
        return []

    comment = build_enforcement_comment(missing, converted=not is_draft)
    if is_draft:
        sync_enforcement_comment(issue_url, token, comment)
    else:
        if not isinstance(node_id, str):
            raise ValueError("pull request event is missing its node ID")
        convert_to_draft(pull_request_url, node_id, token)
        github_request(
            f"{issue_url}/comments",
            token,
            method="POST",
            payload={"body": comment},
        )
    return missing


def main(argv: list[str]) -> int:
    event_path = Path(argv[1] if len(argv) > 1 else os.environ["GITHUB_EVENT_PATH"])
    try:
        event = json.loads(event_path.read_text())
        if not isinstance(event, dict):
            raise ValueError("GitHub event payload must be a JSON object")
        missing = enforce(event, os.environ.get("GITHUB_TOKEN", ""))
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as error:
        print(f"::error title=PR template enforcement failed::{error}")
        return 1

    if missing:
        print(
            "::error title=Pull request description is missing required template content::"
            + "; ".join(missing)
        )
        return 1

    print("Pull request description retains the required template structure.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
