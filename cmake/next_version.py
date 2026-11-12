#!/usr/bin/env python3
"""Prints the version the next release gets, from the commits since the last release (docs/releasing.md).

  python3 cmake/next_version.py                  the next version, from the commits
  python3 cmake/next_version.py --bump minor     force the bump: patch, minor or major
  python3 cmake/next_version.py --self-test      check the rule on a throwaway git repository

The rule, over the Conventional Commits since the last tag v<MAJOR>.<MINOR>.<PATCH>:
  `type!:` or a `BREAKING CHANGE:` footer -> major; any `feat` -> minor; anything else -> patch.
Without a release tag yet, the first release is VERSION as it is (an explicit --bump raises it). A
VERSION raised by hand above the result wins. The version goes to stdout, the reason to stderr and,
in a GitHub Actions run, to the run's summary. Standard library only.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUMPS = ("auto", "patch", "minor", "major")
SUBJECT = re.compile(r"^(?P<type>[A-Za-z]+)(\([^)]*\))?(?P<breaking>!)?:\s")
BREAKING_FOOTER = re.compile(r"^BREAKING[ -]CHANGE:", re.MULTILINE)


class NothingToRelease(Exception):
    pass


def git(repository, *arguments):
    return subprocess.run(["git", "-C", str(repository), *arguments], check=True,
                          capture_output=True, text=True, encoding="utf-8").stdout


def parse(version):
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", version.strip())
    if not match:
        raise ValueError(f"'{version.strip()}' isn't MAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())


def raised(version, bump):
    major, minor, patch = version
    if bump == "major":
        return (major + 1, 0, 0)
    if bump == "minor":
        return (major, minor + 1, 0)
    return (major, minor, patch + 1)


def last_release_tag(repository):
    """The nearest release tag v1.2.3 behind HEAD, or None before the first release."""
    tags = git(repository, "tag", "--merged", "HEAD", "--list", "v*").split()
    releases = [tag for tag in tags if re.fullmatch(r"v\d+\.\d+\.\d+", tag)]
    return max(releases, key=parse) if releases else None


def commit_bump(message):
    """What one commit asks for: major, minor or patch."""
    subject = SUBJECT.match(message)
    if (subject and subject.group("breaking")) or BREAKING_FOOTER.search(message):
        return "major"
    if subject and subject.group("type").lower() == "feat":
        return "minor"
    return "patch"


def next_version(repository, bump="auto"):
    """(version, reason) of the next release from `repository`'s HEAD."""
    in_file = parse((Path(repository) / "VERSION").read_text(encoding="utf-8"))
    tag = last_release_tag(repository)
    if tag is None:
        version = in_file if bump == "auto" else raised(in_file, bump)
        reason = "the first release: VERSION" + ("" if bump == "auto" else f", {bump} asked")
        return ".".join(map(str, version)), reason

    messages = [message.strip() for message in git(repository, "log", "--format=%B%x00", f"{tag}..HEAD").split("\0")]
    messages = [message for message in messages if message]
    if not messages:
        raise NothingToRelease(f"nothing to release: no commit since {tag}")
    counts = {"major": 0, "minor": 0, "patch": 0}
    for message in messages:
        counts[commit_bump(message)] += 1
    if bump == "auto":
        bump = next(level for level in ("major", "minor", "patch") if counts[level])
        reason = f"{bump} from {len(messages)} commits since {tag}: {counts['major']} breaking, {counts['minor']} feat, {counts['patch']} other"
    else:
        reason = f"{bump} asked, {len(messages)} commits since {tag}"
    version = raised(parse(tag), bump)
    if in_file > version:
        version = in_file
        reason += "; VERSION was raised by hand above that"
    return ".".join(map(str, version)), reason


def self_test():
    def commit(repository, message):
        (repository / "change.txt").write_text(message, encoding="utf-8")
        git(repository, "add", "-A")
        git(repository, "commit", "-q", "-m", message)

    with tempfile.TemporaryDirectory() as directory:
        repository = Path(directory)
        git(repository, "init", "-q")
        for key, value in [("user.name", "test"), ("user.email", "test@example.com"),
                           ("commit.gpgsign", "false"), ("tag.gpgsign", "false"), ("core.hooksPath", os.devnull)]:
            git(repository, "config", key, value)
        (repository / "VERSION").write_text("1.0.0\n", encoding="utf-8")
        commit(repository, "feat: the first feature")
        git(repository, "tag", "eval-data-v1")   # not a release tag

        cases = []

        def expect(what, wanted, bump="auto"):
            got = next_version(repository, bump)[0]
            cases.append(what)
            if got != wanted:
                raise AssertionError(f"{what}: {got}, wanted {wanted}")

        expect("no release yet: VERSION as it is", "1.0.0")
        expect("no release yet, a bump asked", "1.1.0", "minor")
        git(repository, "tag", "v1.0.0")
        try:
            next_version(repository)
            raise AssertionError("no commit since the tag: no release")
        except NothingToRelease:
            cases.append("nothing since the tag")
        commit(repository, "Merge pull request #1 from someone/branch")
        expect("a plain commit is a patch", "1.0.1")
        commit(repository, "fix(tts): a fix")
        expect("a fix is a patch", "1.0.1")
        commit(repository, "feat(turn): a feature")
        expect("a feat is minor", "1.1.0")
        expect("a bump asked wins over the commits", "1.0.1", "patch")
        commit(repository, "feat!: a breaking feature")
        expect("type! is major", "2.0.0")
        git(repository, "tag", "v2.0.0")
        commit(repository, "refactor(c): rename\n\nBREAKING CHANGE: vasdk_old is gone")
        expect("a BREAKING CHANGE footer is major", "3.0.0")
        git(repository, "tag", "v3.0.0")
        commit(repository, "docs: words")
        (repository / "VERSION").write_text("5.0.0\n", encoding="utf-8")
        commit(repository, "chore: raise VERSION by hand")
        expect("VERSION raised by hand wins", "5.0.0")
    print(f"self-test: {len(cases)} cases ok")
    return 0


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bump", choices=BUMPS, default="auto")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args(arguments)
    if arguments.self_test:
        return self_test()
    try:
        version, reason = next_version(ROOT, arguments.bump)
    except NothingToRelease as nothing:
        print(nothing, file=sys.stderr)
        return 1
    print(f"{version}: {reason}", file=sys.stderr)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as file:
            file.write(f"**Version {version}**: {reason}\n")
    print(version)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
