"""Regenerate tests/golden/robust_golden.jsonl from the upstream reference normalizer.

Usage:
    curl -LO https://raw.githubusercontent.com/OpenMOSS/MOSS-TTS-Nano/main/tts_robust_normalizer_single_script.py
    python build_robust_golden.py tts_robust_normalizer_single_script.py ../golden/robust_golden.jsonl

The reference ships its own TEST_CASES table of (name, input, expected) literals. This script
reads that table with ast.literal_eval - it parses the file without executing it - and writes
one {"name", "input", "expected"} record per case.
"""

import ast
import json
import sys


def main() -> None:
    reference_path, output_path = sys.argv[1], sys.argv[2]
    with open(reference_path, encoding="utf-8") as f:
        tree = ast.parse(f.read(), filename=reference_path)

    cases = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "TEST_CASES" for target in node.targets
        ):
            cases = ast.literal_eval(node.value)
    if cases is None:
        sys.exit("TEST_CASES not found in " + reference_path)

    with open(output_path, "w", encoding="utf-8", newline="\n") as f:
        for name, text, expected in cases:
            f.write(json.dumps({"name": name, "input": text, "expected": expected}, ensure_ascii=False) + "\n")
    print(f"wrote {len(cases)} cases to {output_path}")


if __name__ == "__main__":
    main()
