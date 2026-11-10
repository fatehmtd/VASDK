#!/usr/bin/env python3
"""Writes manifest.json: every model file a VASDK provider loads, pinned by revision and SHA-256.

The manifest is the single list of models. The model store (VASDK/include/VASDK/ModelStore.h) and
the `vasdk models` command download from it at run time; the build downloads from it at configure
time (VASDK_3P_IMPL/cmake/VASDKModels.cmake). Run this after changing a model below; it asks the Hugging Face API for
each file's size and SHA-256 at the pinned revision (and downloads the few files the API has no
hash for), so the hashes are never typed by hand.

    python VASDK_3P_IMPL/models/generate_manifest.py [--refresh]   # HF_TOKEN for the gated Phonon repo
"""
import hashlib
import json
import os
import pathlib
import sys
import urllib.parse
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
HF = "https://huggingface.co"


def request(url, data=None):
    headers = {"User-Agent": "vasdk-manifest"}
    token = os.environ.get("HF_TOKEN")
    if token and url.startswith(HF):
        headers["Authorization"] = f"Bearer {token}"
    try:
        return urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers), timeout=120)
    except urllib.error.HTTPError as error:
        # A renamed repository answers a POST with 307: post again to where it moved
        if error.code in (307, 308) and error.headers.get("Location"):
            return request(urllib.parse.urljoin(url, error.headers["Location"]), data)
        raise


def hf_paths_info(kind, repo, revision, paths):
    """size and sha256 of each path (LFS files carry their sha256; small git files are hashed here)."""
    body = urllib.parse.urlencode([("paths", p) for p in paths]).encode()
    with request(f"{HF}/api/{kind}/{repo}/paths-info/{revision}", body) as response:
        entries = {entry["path"]: entry for entry in json.load(response)}
    result = {}
    for path in paths:
        entry = entries.get(path)
        if entry is None:
            sys.exit(f"{repo}@{revision}: no {path}")
        if entry.get("lfs"):
            result[path] = (entry["lfs"]["size"], entry["lfs"]["oid"])
        else:
            prefix = "spaces/" if kind == "spaces" else ""
            result[path] = hash_url(f"{HF}/{prefix}{repo}/resolve/{revision}/{path}")
    return result


def hf_revision(kind, repo, branch="main"):
    with request(f"{HF}/api/{kind}/{repo}/revision/{branch}") as response:
        return json.load(response)["sha"]


def hash_url(url):
    digest = hashlib.sha256()
    size = 0
    with request(url) as response:
        while chunk := response.read(1 << 20):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def hf_files(repo, revision, files, kind="models", arch=None):
    """files: [(remote path, local path)] -> manifest file entries."""
    info = hf_paths_info(kind, repo, revision, [remote for remote, _ in files])
    prefix = "spaces/" if kind == "spaces" else ""
    entries = []
    for remote, local in files:
        size, sha256 = info[remote]
        entry = {"path": local, "url": f"{HF}/{prefix}{repo}/resolve/{revision}/{remote}", "size": size, "sha256": sha256}
        if arch:
            entry["arch"] = arch
        entries.append(entry)
    return entries


def url_files(files):
    """files: [(url, local path)] for files outside the Hub, hashed by downloading them."""
    entries = []
    for url, local in files:
        size, sha256 = hash_url(url)
        entries.append({"path": local, "url": url, "size": size, "sha256": sha256})
    return entries


def phonon_voices(revision, names):
    with request(f"{HF}/gradium/phonon-voices/resolve/{revision}/voices.jsonl") as response:
        catalog = [json.loads(line) for line in response.read().decode().splitlines() if line.strip()]
    by_name = {voice["name"]: voice["filename"] for voice in catalog}
    return [(by_name[name], f"phonon_tts/voices/{name}.safetensors") for name in names]


def main():
    # Pinned revisions. To move one: change it here, run this script, commit the manifest.
    silero = "https://raw.githubusercontent.com/snakers4/silero-vad/v6.2/src/silero_vad/data/silero_vad.onnx"
    smart_turn = "f766f81d3cfdf7737ac64aad813d91bbfd56bf93"
    turn_1_mini = "41e98810a30e9bfc09ef752d7120fb41561006f1"
    # Repositories the build used to follow on main: pinned to the commit the manifest already
    # names, or main's current commit the first time (--refresh moves them all to main's).
    manifest_path = HERE / "manifest.json"
    previous = {}
    if manifest_path.exists() and "--refresh" not in sys.argv:
        previous = {model["id"]: model.get("revision") for model in json.loads(manifest_path.read_text())["models"]}

    def pinned(identifier, kind, repo):
        return previous.get(identifier) or hf_revision(kind, repo)

    minilm = pinned("minilm", "models", "sentence-transformers/all-MiniLM-L6-v2")
    parakeet = pinned("parakeet", "models", "istupakov/parakeet-tdt-0.6b-v3-onnx")
    pocket = pinned("pocket_tts", "spaces", "KevinAHM/pocket-tts-web")
    whisper = pinned("whispercpp", "models", "ggerganov/whisper.cpp")
    lfm = pinned("lfm2.5-1.2b-instruct", "models", "LiquidAI/LFM2.5-1.2B-Instruct-GGUF")
    phonon = "1537ce46c64f42903e0d608b132ccb073dac46d2"
    phonon_english = "92741c5b07d085ff840e26fea8ce43110b87852f"
    phonon_voice_revision = "0ee17205f254b5abc43b266c8a0f215565c352f7"
    moonshine = "da30bae714913eb0057e733bccb4700104afb1b0"
    supertonic = "aafc6e32416a594460b32413efc49d7fe4ce6d46"
    moss = "ec4afe55ae63d93fa15a1aed3730ccfd38e6bb86"
    moss_codec = "ceff0d0749bfb3fa2d61149794ec6feef0d1e1ae"
    whistle = "741118687e1f008f06d1351a402a731274789790"
    gliner_small = "8142fb00740ccea973e64b1272949ff48653df5e"
    gliner_multi = "6ddaeb9413b0e71ad8457da1aab378a165b24058"

    phonon_voice_names = ["Easton", "Zoey", "Augustin", "Anton", "Marcos", "Davi"]
    supertonic_voices = [f"voice_styles/{gender}{number}.json" for gender in "MF" for number in range(1, 6)]
    moonshine_graphs = ("frontend.ort", "encoder.ort", "adapter.ort", "cross_kv.ort", "decoder_kv.ort", "streaming_config.json")
    # whisper.cpp checkpoints the build offers (WHISPERCPP_MODEL); medium is the default one
    whisper_sizes = ["tiny.en", "tiny", "base.en", "base", "small.en", "small", "medium.en", "large-v3"]

    def moonshine_files(size):
        return (hf_files("UsefulSensors/moonshine-streaming", moonshine,
                         [(f"onnx/{size}/{name}", f"moonshine/{name}") for name in moonshine_graphs])
                + url_files([(f"https://download.moonshine.ai/model/{size}-streaming-en/quantized/tokenizer.bin",
                              "moonshine/tokenizer.bin")]))

    def model(identifier, description, providers, license, license_url, files, revision=None, **flags):
        entry = {"id": identifier, "description": description, "providers": providers,
                 "license": license, "license_url": license_url}
        if revision:
            entry["revision"] = revision
        entry.update(flags)
        entry["files"] = files
        return entry

    models = [
        model("silero_vad", "Silero VAD v6 (ONNX), voice activity detection", ["vad/silero"], "MIT",
              "https://github.com/snakers4/silero-vad/blob/master/LICENSE",
              url_files([(silero, "silero_vad.onnx")]), revision="v6.2"),
        model("smart_turn_v3", "Smart Turn v3.2 (ONNX, CPU), end-of-turn detection", ["turn/smartturn-v3"], "BSD-2-Clause",
              f"{HF}/pipecat-ai/smart-turn-v3",
              hf_files("pipecat-ai/smart-turn-v3", smart_turn, [("smart-turn-v3.2-cpu.onnx", "smart_turn_v3.onnx")]),
              revision=smart_turn),
        model("turn_1_mini", "turn-1-mini (ONNX, int8 dynamic), streaming end-of-turn detection", ["turn/turn-1-mini"],
              "Apache-2.0", f"{HF}/p99lab/turn-1-mini",
              hf_files("p99lab/turn-1-mini", turn_1_mini, [("onnx/turn-1-mini.step.int8dyn.onnx", "turn_1_mini.onnx")]),
              revision=turn_1_mini),
        model("pyannote_embedding", "WeSpeaker ResNet34-LM (ONNX), speaker embeddings", ["speaker_embedding/pyannote"],
              "CC-BY-4.0", f"{HF}/pyannote/wespeaker-voxceleb-resnet34-LM",
              url_files([("https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
                          "wespeaker_en_voxceleb_resnet34_LM.onnx", "pyannote_embedding.onnx")])),
        model("minilm", "all-MiniLM-L6-v2 (ONNX, int8), sentence encoder for intents", ["sentence_encoder/minilm"],
              "Apache-2.0", f"{HF}/sentence-transformers/all-MiniLM-L6-v2",
              hf_files("sentence-transformers/all-MiniLM-L6-v2", minilm, [("onnx/model_qint8_arm64.onnx", "minilm/model.onnx")], arch="arm64")
              + hf_files("sentence-transformers/all-MiniLM-L6-v2", minilm, [("onnx/model_quint8_avx2.onnx", "minilm/model.onnx")], arch="x86_64")
              + hf_files("sentence-transformers/all-MiniLM-L6-v2", minilm, [("vocab.txt", "minilm/vocab.txt")]),
              revision=minilm),
        model("gliner", "GLiNER small v2.1 (ONNX, int8), English entity finding", ["entity_model/gliner"], "Apache-2.0",
              f"{HF}/urchade/gliner_small-v2.1",
              hf_files("onnx-community/gliner_small-v2.1", gliner_small,
                       [("onnx/model_int8.onnx", "gliner/model.onnx"), ("spm.model", "gliner/spm.model"),
                        ("added_tokens.json", "gliner/added_tokens.json"), ("gliner_config.json", "gliner/gliner_config.json")]),
              revision=gliner_small),
        model("gliner_multi", "GLiNER multi v2.1 (ONNX, int8), multilingual entity finding (replaces gliner)",
              ["entity_model/gliner"], "Apache-2.0", f"{HF}/urchade/gliner_multi-v2.1",
              hf_files("onnx-community/gliner_multi-v2.1", gliner_multi,
                       [("onnx/model_int8.onnx", "gliner/model.onnx"), ("spm.model", "gliner/spm.model"),
                        ("added_tokens.json", "gliner/added_tokens.json"), ("gliner_config.json", "gliner/gliner_config.json")]),
              revision=gliner_multi),
        model("whistle", "Whistle (.cact), speech-to-text", ["stt/whistle"], "Apache-2.0", f"{HF}/Cactus-Compute/whistle",
              hf_files("Cactus-Compute/whistle", whistle, [("whistle.cact", "whistle/whistle.cact")]), revision=whistle),
        model("moonshine", "Moonshine Streaming small (English), speech-to-text", ["stt/moonshine"], "MIT",
              f"{HF}/UsefulSensors/moonshine-streaming",
              moonshine_files("small"), revision=moonshine),
        model("moonshine_tiny", "Moonshine Streaming tiny (English), speech-to-text (replaces moonshine)", [], "MIT",
              f"{HF}/UsefulSensors/moonshine-streaming", moonshine_files("tiny"), revision=moonshine),
        model("moonshine_medium", "Moonshine Streaming medium (English), speech-to-text (replaces moonshine)", [], "MIT",
              f"{HF}/UsefulSensors/moonshine-streaming", moonshine_files("medium"), revision=moonshine),
        model("parakeet", "Parakeet TDT 0.6B v3 (ONNX, int8), multilingual speech-to-text", ["stt/parakeet"], "CC-BY-4.0",
              f"{HF}/nvidia/parakeet-tdt-0.6b-v3",
              hf_files("istupakov/parakeet-tdt-0.6b-v3-onnx", parakeet,
                       [(name, f"parakeet/{name}") for name in
                        ("nemo128.onnx", "vocab.txt", "decoder_joint-model.int8.onnx", "encoder-model.int8.onnx")]),
              revision=parakeet),
        model("whispercpp", "Whisper medium (GGML), speech-to-text for whisper.cpp", [], "MIT", f"{HF}/ggerganov/whisper.cpp",
              hf_files("ggerganov/whisper.cpp", whisper, [("ggml-medium.bin", "whispercpp/ggml-medium.bin")]), revision=whisper),
        *[model(f"whispercpp_{size}", f"Whisper {size} (GGML), speech-to-text for whisper.cpp", [], "MIT",
                f"{HF}/ggerganov/whisper.cpp",
                hf_files("ggerganov/whisper.cpp", whisper, [(f"ggml-{size}.bin", f"whispercpp/ggml-{size}.bin")]),
                revision=whisper)
          for size in whisper_sizes],
        model("pocket_tts", "Pocket TTS (ONNX, int8), English text-to-speech", ["tts/pocket"], "CC-BY-4.0",
              f"{HF}/kyutai/pocket-tts",
              hf_files("KevinAHM/pocket-tts-web", pocket,
                       [(f"onnx/english_2026-04/{name}", f"pocket_tts/{name}") for name in
                        ("text_conditioner_int8.onnx", "flow_lm_main_int8.onnx", "flow_lm_flow_int8.onnx",
                         "mimi_decoder_int8.onnx", "tokenizer.model", "voices.bin")], kind="spaces"),
              revision=pocket),
        model("supertonic", "Supertonic 3 (ONNX), text-to-speech, ten voices (M1-M5, F1-F5)", ["tts/supertonic"], "OpenRAIL",
              f"{HF}/supertone-oss-archive/supertonic-3",
              hf_files("supertone-oss-archive/supertonic-3", supertonic,
                       [(name, f"supertonic3/{name}") for name in
                        ("onnx/tts.json", "onnx/unicode_indexer.json", "onnx/duration_predictor.onnx", "onnx/text_encoder.onnx",
                         "onnx/vector_estimator.onnx", "onnx/vocoder.onnx", *supertonic_voices)]),
              revision=supertonic, requires_acceptance=True),
        model("moss_tts", "MOSS-TTS-Nano 100M (ONNX) and its audio tokenizer, text-to-speech", ["tts/moss"], "Apache-2.0",
              f"{HF}/OpenMOSS-Team/MOSS-TTS-Nano",
              hf_files("Supbatomic/moss-tts-nano-100m-onnx-model", moss,
                       [(name, f"moss_tts/MOSS-TTS-Nano-100M-ONNX/{name}") for name in
                        ("browser_poc_manifest.json", "tts_browser_onnx_meta.json", "tokenizer.model", "moss_tts_prefill.onnx",
                         "moss_tts_decode_step.onnx", "moss_tts_global_shared.data", "moss_tts_local_fixed_sampled_frame.onnx",
                         "moss_tts_local_shared.data")])
              + hf_files("OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX", moss_codec,
                         [(name, f"moss_tts/MOSS-Audio-Tokenizer-Nano-ONNX/{name}") for name in
                          ("codec_browser_onnx_meta.json", "moss_audio_tokenizer_decode_step.onnx",
                           "moss_audio_tokenizer_decode_shared.data")]),
              revision=moss),
        model("phonon_tts", "Phonon 40m multilingual (GGUF) and six voices, text-to-speech", ["tts/phonon"], "CC-BY-NC-4.0",
              f"{HF}/gradium/phonon-40m-multilingual",
              hf_files("gradium/phonon-40m-multilingual", phonon,
                       [(name, f"phonon_tts/{name}") for name in
                        ("config.json", "tokenizer.model", "model.q8.gguf", "default-voice.safetensors")])
              + hf_files("gradium/phonon-voices", phonon_voice_revision,
                         phonon_voices(phonon_voice_revision, phonon_voice_names)),
              revision=phonon, gated=True, requires_acceptance=True),
        model("phonon_tts_en", "Phonon 31m English (GGUF) and six voices, text-to-speech (replaces phonon_tts)", [],
              "Apache-2.0", f"{HF}/gradium/phonon-31m-en",
              hf_files("gradium/phonon-31m-en", phonon_english,
                       [(name, f"phonon_tts/{name}") for name in
                        ("config.json", "tokenizer.model", "model.q8.gguf", "default-voice.safetensors")])
              + hf_files("gradium/phonon-voices", phonon_voice_revision,
                         phonon_voices(phonon_voice_revision, phonon_voice_names)),
              revision=phonon_english),
        model("lfm2.5-1.2b-instruct", "LFM2.5 1.2B Instruct (GGUF, Q4_K_M), local LLM", ["llm/llamacpp"], "LFM-Open-License-1.0",
              f"{HF}/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/blob/main/LICENSE",
              hf_files("LiquidAI/LFM2.5-1.2B-Instruct-GGUF", lfm,
                       [("LFM2.5-1.2B-Instruct-Q4_K_M.gguf", "LFM2.5-1.2B-Instruct-Q4_K_M.gguf")]),
              revision=lfm, requires_acceptance=True),
    ]
    manifest = {
        "schema": 1,
        "comment": "Generated by generate_manifest.py: every model file a provider loads, relative to models_dir.",
        "models": models,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", newline="\n")
    print(f"wrote {manifest_path}: {len(models)} models, {sum(len(m['files']) for m in models)} files")


if __name__ == "__main__":
    main()
