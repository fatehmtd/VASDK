"""Extract the literal tables of the reference normalizer into impl/normalize_tables.inc.

    python extract_tables.py PTTS_SRC_DIR ../../impl/normalize_tables.inc

Copies preprocess.rs's FOLDS (letters with a diacritic -> ASCII letter) and
preprocess/rewrite/tlds.rs's TLDS (IANA top-level domains) mechanically, so the C++ port
cannot drift from them by a typo.
"""

import pathlib
import re
import sys


def main():
    src = pathlib.Path(sys.argv[1])
    tlds_rs = (src / "preprocess" / "rewrite" / "tlds.rs").read_text(encoding="utf-8")
    body = tlds_rs.split("const TLDS: &[&str] = &[", 1)[1].split("];", 1)[0]
    tlds = re.findall(r'"([^"]*)"', body)
    assert tlds == sorted(tlds) and len(set(tlds)) == len(tlds), "TLDS must be sorted and unique"
    pre = (src / "preprocess.rs").read_text(encoding="utf-8")
    folds = re.search(r'const FOLDS: &str = "([^"]*)";', pre).group(1)
    assert len(folds) % 2 == 0
    pairs = [(folds[i], folds[i + 1]) for i in range(0, len(folds), 2)]
    assert len({a for a, _ in pairs}) == len(pairs), "FOLDS keys must be unique"
    assert all(b.isascii() for _, b in pairs)

    out = [
        "// Extracted from gradium-ai/xn-ptts v0.4.1 (ptts/src/preprocess.rs FOLDS and",
        "// ptts/src/preprocess/rewrite/tlds.rs TLDS) by a script; do not edit by hand.",
        "",
        "// Letter with a diacritic -> the ASCII letter it folds to, in FOLDS order (first match wins).",
        "static const FoldPair kFolds[] = {",
    ]
    row = []
    for a, b in pairs:
        row.append("{0x%X, '%s'}" % (ord(a), b))
        if len(row) == 8:
            out.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        out.append("    " + ", ".join(row) + ",")
    out += ["};", "", "// IANA top-level domains, uppercase, sorted bytewise (binary-searched).",
            "static const char* const kTlds[] = {"]
    row = []
    for t in tlds:
        row.append('"%s"' % t)
        if len(row) == 10:
            out.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        out.append("    " + ", ".join(row) + ",")
    out.append("};")
    pathlib.Path(sys.argv[2]).write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(pairs)} fold pairs, {len(tlds)} TLDs")


if __name__ == "__main__":
    main()
