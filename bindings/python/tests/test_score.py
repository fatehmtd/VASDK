"""Transcript scores from Python, through the C API: word and character edits by kind, the texts as
scored, and a corpus rate from added counts.

    VASDK_LIBRARY=build/cmake/c_library/libvasdk_c.so python3 bindings/python/tests/test_score.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vasdk  # noqa: E402


def main():
    score = vasdk.score_transcript("The cat, sat on the mat.", "the cat sit on mat")
    words = score["words"]
    assert (words["substitutions"], words["deletions"], words["insertions"], words["reference_length"]) == (1, 1, 0, 6), words
    assert abs(words["error_rate"] - 2 / 6) < 1e-9, words
    assert score["reference"] == "the cat sat on the mat", score
    assert score["characters"]["reference_length"] == len("the cat sat on the mat"), score

    assert vasdk.normalize_transcript("L’HOMME ÉTÉ, déjà!") == "l'homme été déjà"
    assert vasdk.score_transcript("", "")["words"]["error_rate"] == 0.0
    assert vasdk.score_transcript(None, "a b")["words"]["insertions"] == 2

    clips = [("a", "b"), ("a b c d e f g h i", "a b c d e f g h i")]
    errors = length = 0
    for said, heard in clips:
        counted = vasdk.score_transcript(said, heard)["words"]
        errors += counted["substitutions"] + counted["deletions"] + counted["insertions"]
        length += counted["reference_length"]
    assert errors / length == 0.1, (errors, length)
    print("python score: all passed")


if __name__ == "__main__":
    main()
