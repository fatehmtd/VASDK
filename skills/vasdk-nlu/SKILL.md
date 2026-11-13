---
name: vasdk-nlu
description: Understanding in VASDK - how an agent finds intents (rules, a sentence encoder such as MiniLM, the LLM) and entities (GLiNER, the trained tagger, the LLM), the understanding config, measuring accuracy with eval sets, and implementing a new SentenceEncoder or EntityExtractor. Use when intents are missed, entities are wrong, or when adding an NLU model.
---

# Understanding (intents and entities)

**What it does:** turns what the user said into the agent's intent and entity values. An agent tries its rules first, then the on-device sentence encoder for paraphrases, then (if allowed) the LLM. Entities come from the agent's entity types and an entity model.

| | |
| --- | --- |
| Sentence encoder | `VASDK/include/VASDK/nlu/EmbeddingNluProvider.h`, [contract](../../docs/reference/providers.md#sentenceencoder) |
| Entity model | `VASDK/include/VASDK/nlu/EntityExtractor.h`, [contract](../../docs/reference/providers.md#entityextractor), [entity models](../../docs/reference/flows.md#entity-models) |
| Rules and LLM | `VASDK/include/VASDK/nlu/RuleNluProvider.h`, `VASDK/include/VASDK/nlu/LlmNluProvider.h`, `VASDK/include/VASDK/nlu/LlmEntityExtractor.h`, `VASDK/include/VASDK/nlu/TaggerEntityExtractor.h` |
| Config | `providers { sentence_encoder { id } entity_model { id } sentence_encoder_cache }` and `understanding { ... }`, [engine config](../../docs/reference/engine-config.md#understanding) |
| Agent side | [understanding](../../docs/reference/flows.md#understanding), [entities](../../docs/reference/flows.md#entities) |

## Built in

| Id | Where | Kind | Options |
| --- | --- | --- | --- |
| `minilm` | `VASDK_3P_IMPL/MiniLmNlu/` | sentence encoder, all-MiniLM-L6-v2 ONNX, on by default, `va::thirdparty::nlu` | `MiniLmOptions`: `model_dir` |
| `gliner` | `VASDK_3P_IMPL/GlinerNer/` | entity model, ONNX, opt-in, `va::thirdparty::nlu` | `GlinerOptions`: `model_dir`, `threshold` (0.5) |
| `tagger` | core (`TaggerEntityExtractor`) | entity model trained on the encoder's word vectors | built by `Session` |
| `llm` | core (`LlmEntityExtractor`) | entity model asked of the agent's LLM | built by `Session` |

## Using it

```textproto
providers { sentence_encoder { id: "minilm" } entity_model { id: "gliner" } sentence_encoder_cache: "agent.vectors" }
```

Measure before you tune: `garage_agentc eval agent.txtpb nlu_eval.txtpb --sweep` reports intent accuracy and entity F1 across thresholds; `--engine-config` compares entity models. The spoken variant (`nlu_eval.spoken.txtpb`, made by `vasdk_spoken_eval`) measures what the agent hears through STT ([checking and testing](../../docs/reference/flows.md#checking-and-testing-vasdk_agentc)).

## Contract pitfalls

- Shared by every session: `encode()` and `extract()` must be thread-safe and `const`.
- `encode()` returns a unit-length vector, empty on failure. `encodeWords()` is optional, but the `tagger` needs it.
- `extract()` returns non-overlapping spans indexing `words` (use `nlu::nonOverlapping`), each with a score from 0 to 1.
- **`sensitive`**: the words may be a secret answer; an extractor that sends text off the device returns nothing.
- `tagger` and `llm` are taken ids. Don't confuse an entity model with `FlowRegistry::addEntityExtractor`, which finds the values of one `code` entity, such as a database lookup.

## Adding one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md). Interfaces: `va::nlu::SentenceEncoder` or `va::nlu::EntityExtractor`. Options: a new `<Name>Options` message.
2. Templates: `VASDK_3P_IMPL/MiniLmNlu/` (encoder) and `VASDK_3P_IMPL/GlinerNer/` (entity model); both are `va::thirdparty::nlu` ONNX models with download scripts.
3. Register it with `registry.add<va::nlu::EntityExtractor, providers::<Name>Options>("<id>", ...)` (or `SentenceEncoder`); `nullptr` from the factory fails the runtime with a clear message.

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- Unit: `VASDK_3P_IMPL/<Name>/tests/` or a test next to it, as `vasdk_minilm_nlu` and `vasdk_gliner_ner`: known sentences give similar vectors, known spans are found, a missing model skips (`VA_REQUIRE_FILE`).
- Integration: `vasdk_entity_extractor`, `vasdk_entity_model`, `vasdk_embedding_cache` (`VASDK/tests/`), and the garage eval sets (`vasdk_agent_garage_menu_nlu`, `vasdk_agent_garage_menu_nlu_spoken`) run with the new id through `--engine-config`; accuracy must not drop below their `--min-accuracy`.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold, and its page reports the eval numbers against the models it is meant to replace.
