#!/usr/bin/env python3
"""
Issue backlog triage helper for noctalia-dev/noctalia. Requires an authenticated `gh`.

  candidates --out DIR
      Read-only. Fetches open issues and writes one TSV of candidates per list into DIR:
        stale-version-bugs.tsv    bugs filed against a release two or more minors behind the latest
        unknown-version-bugs.tsv  bugs with no parsable version
        unlabeled.tsv             issues that are neither bug nor feature
        fixed-by-commit.tsv       open issues referenced by a "Fixes #N" style commit on origin/main
        duplicates.tsv            clusters of issues with similar titles
        feature-review.tsv        every feature request, least supported first

  apply DECISIONS.tsv [--execute]
      Applies maintainer decisions, one row per issue: number<TAB>action<TAB>arg. Blank lines and
      lines starting with # are ignored. Dry run unless --execute is given.
      --allow-voted N [N ...] lets close actions on those issues skip the guard below.

        retest        label needs-retest and ask the reporter to retest on the latest release
        needs-info    label needs-info and ask the reporter for details
        fixed         arg: sha or sha,tag. Close as completed
        out-of-scope  arg: one-line reason. Close as not planned
        plugin        Close as not planned, pointing to the plugin API
        duplicate     arg: #N. Close as duplicate of N
        keep          no action

      Close actions skip issues with 3 or more reactions or the "PRs welcome" label.

Usage:
  python3 tools/issue-triage.py candidates --out /tmp/triage
  python3 tools/issue-triage.py apply decisions.tsv            # dry run
  python3 tools/issue-triage.py apply decisions.tsv --execute
  python3 tools/issue-triage.py apply decisions.tsv --execute --allow-voted 3591   # override the vote guard
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

REPO = "noctalia-dev/noctalia"
ROOT = Path(__file__).resolve().parents[1]

VOTE_GUARD = 3
PROTECTED_LABEL = "PRs welcome"
WRITE_DELAY_SECONDS = 2
DUPLICATE_SIMILARITY = 0.3
MAX_ISSUES = 2000

SCOPE_URL = f"https://github.com/{REPO}/blob/main/SCOPE.md"
DUPLICATE_SIMILARITY = 0.3
AUTOMATED_NOTE = "_This is an automated message from our backlog cleanup, posted from a maintainer's account._\n\n"
REPLY_RETEST = AUTOMATED_NOTE + (
    "Hi, and thanks for the report! We are going through older bug reports, and this one was filed "
    "against {version} while the latest release is {latest}. Even if you commented recently, a lot "
    "has changed since, so it may already be fixed. Could you give {latest} a try and let us know "
    "whether it still happens? If we do not hear back within 14 days, this issue will be closed "
    "automatically. No hard feelings, you can reopen it or comment at any time."
)
REPLY_NEEDS_INFO = AUTOMATED_NOTE + (
    "Hi, and thanks for the report! We are missing a few details to be able to act on it. Could you "
    "tell us which Noctalia version you run (the latest release is {latest}), your compositor, the "
    "steps to reproduce, and the logs from the terminal where Noctalia was started? If we do not "
    "hear back within 14 days, this issue will be closed automatically. You can reopen it or comment "
    "at any time."
)
REPLY_FIXED_TAGGED = AUTOMATED_NOTE + (
    "Good news: this was fixed in {sha} and shipped in {tag}, so we are closing it as completed. If "
    "it still happens on {tag} or later, comment here with details and we will reopen it."
)
REPLY_FIXED_UNRELEASED = AUTOMATED_NOTE + (
    "Good news: this was fixed on main in {sha} and will ship in the next release, so we are closing "
    "it as completed. If it still happens afterwards, comment here and we will reopen it."
)
REPLY_OUT_OF_SCOPE = AUTOMATED_NOTE + (
    "Thanks for the suggestion! We went through our open requests against [SCOPE.md](" + SCOPE_URL + ") "
    "and this one falls outside what Noctalia core takes on: {reason}. We are closing it to keep this "
    "tracker for bugs and accepted work. That is not a verdict on the idea, and you are welcome to "
    "talk it through with us on [Discord](https://discord.noctalia.dev)."
)
REPLY_PLUGIN = AUTOMATED_NOTE + (
    "Thanks for the suggestion! Per [SCOPE.md](" + SCOPE_URL + "), features that are useful to some "
    "users but not essential to the shell belong in plugins, so we will not track this one in core. "
    "If the [plugin API](https://docs.noctalia.dev) is missing something you need to build it, we are "
    "happy to add new endpoints for it, so please open an issue describing what you need. You are "
    "also welcome to talk it through with us on [Discord](https://discord.noctalia.dev)."
)
REPLY_DUPLICATE = AUTOMATED_NOTE + (
    "Thanks for the report! This is a duplicate of {dup}, so we are closing it in favor of that "
    "issue. Please add your use case or a reaction over there."
)

MANAGED_LABELS = {
    "needs-info": ("fbca04", "Waiting for the reporter"),
    "needs-retest": ("fbca04", "Reporter should retest on the latest release"),
    "out-of-scope": ("cfd3d7", "Declined per SCOPE.md"),
    "duplicate": ("cfd3d7", "Duplicate of another issue"),
    "stale": ("ededed", "No activity, closing soon"),
}

CLOSE_ACTIONS = {"fixed", "out-of-scope", "plugin", "duplicate"}
ACTIONS = {"retest", "needs-info", "fixed", "out-of-scope", "plugin", "duplicate", "keep"}

TITLE_PREFIX = re.compile(r"^\s*\[(?:bug|feature)\]\s*:?\s*", re.IGNORECASE)
VERSION_FIELD = re.compile(r"###\s*Noctalia version\s*\r?\n+([^\n\r]+)", re.IGNORECASE)
VERSION_NUMBER = re.compile(r"(\d+)\.(\d+)(?:\.(\d+))?")
FIX_REFERENCE = re.compile(r"(?i)\b(?:fix(?:e[sd])?|close[sd]?|resolve[sd]?)\s*:?\s*#(\d+)")
STOPWORDS = {
    "the", "and", "for", "with", "add", "allow", "option", "support", "ability", "when", "from",
    "that", "this", "make", "can", "not", "into",
}  # fmt: skip


@dataclass
class Issue:
    number: int
    title: str
    kind: str
    version: tuple[int, int] | None
    version_text: str
    reactions: int
    ncomments: int
    labels: set[str]


def run(args: list[str], check: bool = True) -> str:
    result = subprocess.run(args, capture_output=True, text=True, cwd=ROOT)
    if check and result.returncode != 0:
        sys.exit(f"command failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout


def gh_json(args: list[str]):
    return json.loads(run(["gh", *args]))


def parse_version(text: str) -> tuple[int, int] | None:
    match = VERSION_NUMBER.search(text)
    return (int(match.group(1)), int(match.group(2))) if match else None


def classify(title: str, labels: set[str]) -> str:
    lowered = title.lower()
    if "bug" in labels or lowered.startswith("[bug]"):
        return "bug"
    if "feature" in labels or lowered.startswith("[feature]"):
        return "feature"
    return "unlabeled"


def to_issue(raw: dict) -> Issue:
    labels = {label["name"] for label in raw["labels"]}
    field = VERSION_FIELD.search(raw.get("body") or "")
    version_text = field.group(1).strip() if field else ""
    return Issue(
        number=raw["number"],
        title=raw["title"],
        kind=classify(raw["title"], labels),
        version=parse_version(version_text) if version_text else None,
        version_text=version_text,
        reactions=sum(group["users"]["totalCount"] for group in raw["reactionGroups"]),
        ncomments=len(raw["comments"]),
        labels=labels,
    )


def fetch_open_issues() -> list[Issue]:
    fields = "number,title,labels,comments,reactionGroups,body"
    raw = gh_json(["issue", "list", "-R", REPO, "--state", "open", "-L", str(MAX_ISSUES), "--json", fields])
    return [to_issue(item) for item in raw]


def fetch_one(number: int) -> dict | None:
    fields = "number,title,state,labels,comments,reactionGroups,body"
    result = subprocess.run(
        ["gh", "issue", "view", str(number), "-R", REPO, "--json", fields],
        capture_output=True, text=True, cwd=ROOT,
    )  # fmt: skip
    if result.returncode != 0:
        return None
    return json.loads(result.stdout)


def latest_release() -> str:
    return run(["gh", "release", "view", "-R", REPO, "--json", "tagName", "--jq", ".tagName"]).strip()


def write_tsv(path: Path, header: list[str], rows: list[list]) -> None:
    lines = ["\t".join(header)]
    for row in rows:
        lines.append("\t".join(str(cell).replace("\t", " ").replace("\n", " ") for cell in row))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(rows):5d}  {path.name}")


def title_tokens(title: str) -> frozenset[str]:
    stripped = TITLE_PREFIX.sub("", title).lower()
    words = re.split(r"[^a-z0-9]+", stripped)
    return frozenset(word for word in words if len(word) >= 3 and word not in STOPWORDS)


def duplicate_clusters(issues: list[Issue]) -> list[list[Issue]]:
    clusters: list[list[Issue]] = []
    for kind in ("bug", "feature"):
        members = [issue for issue in issues if issue.kind == kind]
        tokens = {issue.number: title_tokens(issue.title) for issue in members}
        parent = {issue.number: issue.number for issue in members}

        def find(node: int) -> int:
            while parent[node] != node:
                parent[node] = parent[parent[node]]
                node = parent[node]
            return node

        for index, left in enumerate(members):
            left_tokens = tokens[left.number]
            if not left_tokens:
                continue
            for right in members[index + 1 :]:
                right_tokens = tokens[right.number]
                if not right_tokens:
                    continue
                union = len(left_tokens | right_tokens)
                if len(left_tokens & right_tokens) / union >= DUPLICATE_SIMILARITY:
                    parent[find(left.number)] = find(right.number)

        groups: dict[int, list[Issue]] = {}
        for issue in members:
            groups.setdefault(find(issue.number), []).append(issue)
        clusters.extend(group for group in groups.values() if len(group) >= 2)
    return sorted(clusters, key=lambda group: min(issue.number for issue in group))


def fixed_by_commit(open_numbers: set[int]) -> list[list]:
    run(["git", "fetch", "origin", "main"])
    log = run(["git", "log", "origin/main", "--format=%H%x1f%s%x1f%B%x1e"])
    rows: dict[int, list] = {}
    for record in log.split("\x1e"):
        parts = record.strip("\n").split("\x1f")
        if len(parts) < 3:
            continue
        sha, subject, body = parts[0].strip(), parts[1], parts[2]
        for match in FIX_REFERENCE.finditer(subject + "\n" + body):
            number = int(match.group(1))
            if number not in open_numbers or number in rows:
                continue
            tags = run(["git", "tag", "--contains", sha, "--sort=version:refname", "-l", "v*"]).split()
            rows[number] = [number, sha[:10], tags[0] if tags else "", subject]
    return [rows[number] for number in sorted(rows)]


def cmd_candidates(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    issues = fetch_open_issues()
    latest = latest_release()
    latest_version = parse_version(latest)
    if latest_version is None:
        sys.exit(f"cannot parse latest release tag: {latest}")
    cutoff = (latest_version[0], latest_version[1] - 2)
    print(f"{len(issues)} open issues, latest release {latest}, stale bug cutoff <= {cutoff[0]}.{cutoff[1]}")

    bugs = [issue for issue in issues if issue.kind == "bug"]
    stale = [issue for issue in bugs if issue.version is not None and issue.version <= cutoff]
    write_tsv(
        out / "stale-version-bugs.tsv",
        ["number", "version", "reactions", "title"],
        [[i.number, i.version_text, i.reactions, i.title] for i in sorted(stale, key=lambda i: i.number)],
    )
    unknown = [issue for issue in bugs if issue.version is None]
    write_tsv(
        out / "unknown-version-bugs.tsv",
        ["number", "reactions", "title"],
        [[i.number, i.reactions, i.title] for i in sorted(unknown, key=lambda i: i.number)],
    )
    unlabeled = [issue for issue in issues if issue.kind == "unlabeled"]
    write_tsv(
        out / "unlabeled.tsv",
        ["number", "reactions", "ncomments", "title"],
        [[i.number, i.reactions, i.ncomments, i.title] for i in sorted(unlabeled, key=lambda i: i.number)],
    )
    write_tsv(
        out / "fixed-by-commit.tsv",
        ["number", "sha", "release_tag", "subject"],
        fixed_by_commit({issue.number for issue in issues}),
    )

    rows = []
    for index, cluster in enumerate(duplicate_clusters(issues), start=1):
        canonical = max(cluster, key=lambda i: (i.reactions, i.ncomments, -i.number))
        for issue in sorted(cluster, key=lambda i: i.number):
            rows.append([index, issue.number, issue.reactions, issue.ncomments, "yes" if issue is canonical else "", issue.title])
    write_tsv(out / "duplicates.tsv", ["cluster", "number", "reactions", "ncomments", "canonical", "title"], rows)

    features = sorted((i for i in issues if i.kind == "feature"), key=lambda i: (i.reactions, i.ncomments, i.number))
    write_tsv(
        out / "feature-review.tsv",
        ["number", "reactions", "ncomments", "title"],
        [[i.number, i.reactions, i.ncomments, i.title] for i in features],
    )


@dataclass
class Decision:
    number: int
    action: str
    arg: str
    line: int


def parse_decisions(path: Path) -> list[Decision]:
    decisions: list[Decision] = []
    errors: list[str] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cells = line.split("\t")
        number_text, action = cells[0].strip(), (cells[1].strip() if len(cells) > 1 else "")
        arg = cells[2].strip() if len(cells) > 2 else ""
        if not number_text.isdigit():
            errors.append(f"line {line_number}: issue number is not an integer: {number_text!r}")
            continue
        if action not in ACTIONS:
            errors.append(f"line {line_number}: unknown action {action!r}")
            continue
        if action in {"out-of-scope", "duplicate"} and not arg:
            errors.append(f"line {line_number}: action {action} requires an argument")
            continue
        if action == "duplicate" and not re.fullmatch(r"#\d+", arg):
            errors.append(f"line {line_number}: duplicate argument must look like #N, got {arg!r}")
            continue
        if action == "fixed" and not arg:
            errors.append(f"line {line_number}: action fixed requires sha or sha,tag")
            continue
        decisions.append(Decision(int(number_text), action, arg, line_number))
    if errors:
        sys.exit("decisions file rejected, nothing was applied:\n" + "\n".join(errors))
    return decisions


def build_comment(decision: Decision, version_text: str, latest: str) -> str | None:
    action, arg = decision.action, decision.arg
    if action == "retest":
        return REPLY_RETEST.format(version=version_text or "an older release", latest=latest)
    if action == "needs-info":
        return REPLY_NEEDS_INFO.format(latest=latest)
    if action == "fixed":
        sha, _, tag = arg.partition(",")
        template = REPLY_FIXED_TAGGED if tag.strip() else REPLY_FIXED_UNRELEASED
        return template.format(sha=sha.strip(), tag=tag.strip())
    if action == "out-of-scope":
        return REPLY_OUT_OF_SCOPE.format(reason=arg.rstrip("."))
    if action == "plugin":
        return REPLY_PLUGIN
    if action == "duplicate":
        return REPLY_DUPLICATE.format(dup=arg)
    return None


ADD_LABEL = {
    "retest": "needs-retest",
    "needs-info": "needs-info",
    "out-of-scope": "out-of-scope",
    "plugin": "out-of-scope",
    "duplicate": "duplicate",
}
STATE_REASON = {
    "fixed": "completed",
    "out-of-scope": "not_planned",
    "plugin": "not_planned",
    "duplicate": "duplicate",
}


def ensure_labels() -> None:
    for name, (color, description) in MANAGED_LABELS.items():
        run(["gh", "label", "create", name, "-R", REPO, "--color", color, "--description", description, "--force"])


def cmd_apply(path: Path, execute: bool, allow_voted: set[int]) -> None:
    decisions = parse_decisions(path)
    actionable = [d for d in decisions if d.action != "keep"]
    latest = latest_release()
    mode = "EXECUTE" if execute else "DRY RUN"
    print(f"{mode}: {len(actionable)} actions from {len(decisions)} rows, latest release {latest}")
    if execute and actionable:
        ensure_labels()

    applied = 0
    for decision in actionable:
        number = decision.number
        raw = fetch_one(number)
        if raw is None:
            print(f"SKIP #{number} {decision.action}: cannot read issue")
            continue
        if raw["state"] != "OPEN":
            print(f"SKIP #{number} {decision.action}: not open")
            continue
        issue = to_issue(raw)
        guarded = issue.reactions >= VOTE_GUARD or PROTECTED_LABEL in issue.labels
        if decision.action in CLOSE_ACTIONS and guarded and number not in allow_voted:
            print(f"SKIP #{number} {decision.action}: voted ({issue.reactions} reactions, labels {sorted(issue.labels)})")
            continue

        comment = build_comment(decision, issue.version_text, latest)
        label = ADD_LABEL.get(decision.action)
        reason = STATE_REASON.get(decision.action)
        summary = f"#{number} {decision.action} {decision.arg}".rstrip()
        print(f"{'APPLY' if execute else 'PLAN '} {summary} | {issue.title[:70]}")
        if not execute:
            continue

        if label:
            run(["gh", "issue", "edit", str(number), "-R", REPO, "--add-label", label])
        run(["gh", "issue", "comment", str(number), "-R", REPO, "--body", comment or ""])
        if reason:
            run([
                "gh", "api", "-X", "PATCH", f"repos/{REPO}/issues/{number}",
                "-f", "state=closed", "-f", f"state_reason={reason}",
            ])  # fmt: skip
        applied += 1
        time.sleep(WRITE_DELAY_SECONDS)

    if execute:
        print(f"applied {applied} actions")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    candidates = sub.add_parser("candidates", help="write candidate lists for review")
    candidates.add_argument("--out", type=Path, required=True)
    apply = sub.add_parser("apply", help="apply maintainer decisions")
    apply.add_argument("decisions", type=Path)
    apply.add_argument("--execute", action="store_true", help="perform the writes; default is a dry run")
    apply.add_argument(
        "--allow-voted", type=int, nargs="+", default=[], metavar="N",
        help="issue numbers whose close actions may skip the vote and PRs welcome guard",
    )  # fmt: skip
    args = parser.parse_args()

    if args.command == "candidates":
        cmd_candidates(args.out)
    else:
        cmd_apply(args.decisions, args.execute, set(args.allow_voted))


if __name__ == "__main__":
    main()
