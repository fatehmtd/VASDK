# Phonon normalizer reference data

`impl/normalize.cpp` and `impl/chunk.cpp` are C++ ports of the text front end of Gradium's
Phonon runtime, [gradium-ai/xn-ptts](https://github.com/gradium-ai/xn-ptts) v0.4.1: the
normalizer (`ptts::preprocess`, `ptts::tts_model::prepare_text_prompt`) and the splitting of long
text into prompts (`ptts::plan`, `ptts::tts_model::split_into_best_sentences`). This directory generates the data the port is built
from and tested against, by running the reference itself. You need Rust and Python only to
regenerate this data. Building VASDK doesn't need either, because the outputs are committed.

| Output | Made by | Used by |
| --- | --- | --- |
| `impl/unicode_props.inc` | `phonon-normgen unicode` | the port: Rust's `char` properties and uppercase mapping |
| `impl/normalize_tables.inc` | `extract_tables.py` | the port: the reference's FOLDS and TLDS tables, copied mechanically |
| `tests/golden/normalize_golden.jsonl` | `build_corpus.py` + `phonon-normgen golden` + `phonon-normgen chaos` | `tests/normalize_test.cpp` |
| `tests/golden/chunk_golden.jsonl` | `phonon-normgen chunks` | `tests/chunk_test.cpp` |

The golden file holds the reference's output for:

- every string literal of every unit test in the reference's normalizer sources, in all five
  languages and under several rule sets. These are run through `normalize_text`, through
  `rewrite_word` with each rule alone, and through `prepare_text_prompt`;
- realistic assistant replies in each language;
- a seeded token fuzz corpus and a seeded "chaos" corpus of random strings over every character
  the rules branch on, plus random code points.

`chunk_golden.jsonl` holds the reference's plan, made with the checkpoint's own
`tokenizer.json`, for paragraphs strung together from the corpus strings, long sentences with no
full stop, and random strings: each chunk's text, tokens, frames after end of speech and frame
budget, at several chunk sizes and cut limits. It also holds the token ids of every corpus string
and of random strings, which checks that SentencePiece on `tokenizer.model` (what the C++ engine
loads) tokenizes exactly as Hugging Face `tokenizers` on `tokenizer.json` (what the reference
loads).

## Run the parity tests

```bash
cmake -B build -S . -DVASDK_BUILD_PHONON_TTS=ON -DVASDK_BUILD_TESTS=ON
cmake --build build --config Release --target phonon_normalize_test phonon_chunk_test
ctest --test-dir build -C Release -R phonon_ --output-on-failure
```

`phonon_chunk_unit` runs the reference's unit tests of `plan.rs`, which use mock tokenizers.
`phonon_chunk_parity` needs `tokenizer.model` from the gated checkpoint, and reports "skipped"
when it has not been downloaded.

## Regenerate (after moving to a new xn-ptts release)

1. Bump the `tag` in `Cargo.toml` and the version mentioned in `impl/normalize.*`.
2. Point `PTTS_SRC` at `ptts/src` of a checkout at that tag. The copy cargo fetches works:
   `~/.cargo/git/checkouts/xn-ptts-*/<rev>/ptts/src`.
3. Run, with `TOKENIZER_JSON` pointing at `tokenizer.json` of the checkpoint (from
   gradium/phonon-40m-multilingual):

```bash
cargo build --release
./target/release/phonon-normgen unicode > ../../impl/unicode_props.inc
python extract_tables.py "$PTTS_SRC" ../../impl/normalize_tables.inc
python build_corpus.py "$PTTS_SRC" > corpus.jsonl
{ ./target/release/phonon-normgen golden corpus.jsonl 200 1; \
  ./target/release/phonon-normgen chaos 200 2; } > ../golden/normalize_golden.jsonl
./target/release/phonon-normgen chunks "$TOKENIZER_JSON" corpus.jsonl 40 3 > ../golden/chunk_golden.jsonl
```

4. Rebuild and run the parity tests. Every mismatch they print is something the new release
   changed; port that change into `impl/normalize.cpp` or `impl/chunk.cpp`.

For a deeper check that you don't commit, generate much larger corpora (for example
`golden corpus.jsonl 20000 777` and `chaos 20000 4242`, about 650,000 cases, or
`chunks "$TOKENIZER_JSON" corpus.jsonl 2000 9001`, about 50,000) and pass them to
`phonon_normalize_test`, or to `phonon_chunk_test` after the tokenizer path, as extra arguments.
