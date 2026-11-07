#!/usr/bin/env python3
"""Checks the agent skills against the repository, so they can't go stale silently.

  python3 skills/check_skills.py              check skills/ and AGENTS.md in this repository
  python3 skills/check_skills.py --root DIR   check another tree laid out the same way
  python3 skills/check_skills.py --self-test  run the checks on a broken fixture; every planted fault must be found

Checks:
  - every skills/<name>/SKILL.md starts with frontmatter whose `name` is its folder and whose
    `description` is set and at most 1024 characters
  - every skill is listed in AGENTS.md and skills/README.md
  - every relative link resolves, including its #anchor when it points into a markdown file
  - every `path` in backticks whose first part is a top-level entry of the repository exists
  - every provider id registered in ProviderCatalog.cpp is named in some skill
Placeholders (anything with <...>) and code blocks are not checked. Standard library only.
"""

import argparse
import os
import re
import sys

MAX_DESCRIPTION = 1024
CATALOG = os.path.join("VASDK_3P_IMPL", "ProviderCatalog", "ProviderCatalog.cpp")
FENCE = re.compile(r"^```.*?^```[^\n]*$", re.MULTILINE | re.DOTALL)
LINK = re.compile(r"\]\(([^)\s]+)\)")
INLINE_CODE = re.compile(r"`([^`\n]+)`")
PATH_TOKEN = re.compile(r"^[A-Za-z0-9_.\-/]+$")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$", re.MULTILINE)
REGISTERED_ID = re.compile(r'registry\.add<[^(]*>\(\s*"([^"]+)"')
FRONTMATTER = re.compile(r"\A---\n(.*?)\n---\n", re.DOTALL)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def without_code_blocks(text):
    return FENCE.sub("", text)


def anchor_slugs(markdown):
    """GitHub's heading anchors: lower case, punctuation dropped, spaces to hyphens, -1/-2 for repeats."""
    slugs = set()
    seen = {}
    for match in HEADING.finditer(without_code_blocks(markdown)):
        title = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", match.group(2))   # [text](url) -> text
        slug = re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        slugs.add(slug if count == 0 else f"{slug}-{count}")
    return slugs


def skill_folders(root):
    skills_dir = os.path.join(root, "skills")
    return sorted(name for name in os.listdir(skills_dir)
                  if os.path.isfile(os.path.join(skills_dir, name, "SKILL.md")))


def files_to_check(root):
    files = [os.path.join(root, "AGENTS.md"), os.path.join(root, "skills", "README.md")]
    for name in skill_folders(root):
        folder = os.path.join(root, "skills", name)
        files += sorted(os.path.join(folder, entry) for entry in os.listdir(folder) if entry.endswith(".md"))
    return [path for path in files if os.path.isfile(path)]


def check_frontmatter(root, name, errors):
    path = os.path.join(root, "skills", name, "SKILL.md")
    match = FRONTMATTER.match(read(path))
    if not match:
        errors.append(f"{path}: no frontmatter (--- name/description ---) at the top")
        return
    fields = dict(line.split(":", 1) for line in match.group(1).splitlines() if ":" in line)
    fields = {key.strip(): value.strip() for key, value in fields.items()}
    if fields.get("name") != name:
        errors.append(f"{path}: name '{fields.get('name')}' must be its folder name '{name}'")
    description = fields.get("description", "")
    if not description:
        errors.append(f"{path}: description is missing")
    elif len(description) > MAX_DESCRIPTION:
        errors.append(f"{path}: description is {len(description)} characters, at most {MAX_DESCRIPTION}")


def check_listed(root, names, errors):
    for index_file in ("AGENTS.md", os.path.join("skills", "README.md")):
        path = os.path.join(root, index_file)
        text = read(path) if os.path.isfile(path) else ""
        for name in names:
            if f"{name}/SKILL.md" not in text:
                errors.append(f"{path}: skill '{name}' is not listed")


def check_references(root, path, errors, slug_cache):
    text = without_code_blocks(read(path))
    top_level = set(os.listdir(root))
    for target in LINK.findall(text):
        if "<" in target or re.match(r"^[a-z]+:", target) or target.startswith("#"):
            continue
        file_part, _, anchor = target.partition("#")
        resolved = os.path.normpath(os.path.join(os.path.dirname(path), file_part))
        if not os.path.exists(resolved):
            errors.append(f"{path}: link to a missing file: {target}")
        elif anchor and resolved.endswith(".md"):
            if resolved not in slug_cache:
                slug_cache[resolved] = anchor_slugs(read(resolved))
            if anchor not in slug_cache[resolved]:
                errors.append(f"{path}: link to a missing heading: {target}")
    for token in INLINE_CODE.findall(text):
        if "/" not in token or not PATH_TOKEN.match(token) or token.split("/")[0] not in top_level:
            continue
        if not os.path.exists(os.path.join(root, token.rstrip("/"))):
            errors.append(f"{path}: path in backticks doesn't exist: {token}")


def check_provider_ids(root, names, errors):
    catalog = os.path.join(root, CATALOG)
    if not os.path.isfile(catalog):
        errors.append(f"{catalog}: not found")
        return
    skills_text = "".join(read(os.path.join(root, "skills", name, "SKILL.md")) for name in names)
    for provider_id in sorted(set(REGISTERED_ID.findall(read(catalog)))):
        if f"`{provider_id}`" not in skills_text:
            errors.append(f"{catalog}: provider id '{provider_id}' is in no skill; add it to its stage skill's table")


def check(root):
    errors = []
    names = skill_folders(root)
    if not names:
        errors.append(f"{os.path.join(root, 'skills')}: no skills found")
    for name in names:
        check_frontmatter(root, name, errors)
    check_listed(root, names, errors)
    slug_cache = {}
    for path in files_to_check(root):
        check_references(root, path, errors, slug_cache)
    check_provider_ids(root, names, errors)
    return errors


# The faults planted in tests/fixtures/broken; each must be reported, and nothing else.
EXPECTED_FAULTS = [
    "name 'other-name' must be its folder name 'wrong-name'",
    "description is missing",
    "skill 'no-description' is not listed",                 # missing from AGENTS.md
    "link to a missing file: ../../docs/missing.md",
    "link to a missing heading: ../../docs/page.md#no-such-heading",
    "path in backticks doesn't exist: docs/gone.md",
    "provider id 'ghost' is in no skill",
]


def self_test():
    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures", "broken")
    errors = check(fixture)
    missed = [fault for fault in EXPECTED_FAULTS if not any(fault in error for error in errors)]
    unexpected = [error for error in errors if not any(fault in error for fault in EXPECTED_FAULTS)]
    for fault in missed:
        print(f"MISSED: {fault}")
    for error in unexpected:
        print(f"UNEXPECTED: {error}")
    assert anchor_slugs("## Checking and testing: `vasdk_agentc`\n## Audio I/O\n## A\n## A\n") == {
        "checking-and-testing-vasdk_agentc", "audio-io", "a", "a-1"}
    if missed or unexpected:
        return 1
    print(f"self-test: all {len(EXPECTED_FAULTS)} planted faults found, nothing else")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    errors = check(os.path.abspath(arguments.root))
    for error in errors:
        print(error)
    if errors:
        print(f"{len(errors)} problem(s) in the skills")
        return 1
    print(f"skills ok: {len(skill_folders(arguments.root))} skills checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
