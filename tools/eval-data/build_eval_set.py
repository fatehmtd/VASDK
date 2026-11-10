"""Builds VASDK's provider evaluation set (docs/evaluation.md): real read speech from public corpora,
with what was said, and clips composed from it with where the speech is.

    python -I tools/eval-data/build_eval_set.py --cache <download folder> --out <set folder>
    python -I tools/eval-data/build_eval_set.py ... --github-release fatehmtd/VASDK:eval-data-v1   # upload, pin the manifest
    python -I tools/eval-data/build_eval_set.py ... --upload <hf dataset repo>                     # or a HF dataset

Writes into --out:
  vasdk_eval_v1/clips/*.wav        16 kHz mono 16-bit
  vasdk_eval_v1/references.txtpb   a vasdk.eval.EvalSet (VASDK_3P_IMPL/Evaluation/proto/vasdk/eval/eval_set.proto)
  vasdk_eval_v1/ATTRIBUTION.md     the corpora and their licenses
  manifest.json                    a model-store manifest of those files (id eval_audio_v1), with
                                   file:// URLs, or where they were uploaded: a GitHub release's
                                   assets (flat names) or a Hugging Face dataset at its revision

Copy manifest.json to VASDK_3P_IMPL/Evaluation/data/manifest.json once it points at the upload.

Sources, all CC BY 4.0:
  LibriSpeech test-clean (openslr.org/12): English read speech with speakers and genders.
  FLEURS en_us and fr_fr test (huggingface.co/datasets/google/fleurs): read sentences, punctuated.

Clips:
  SPEECH  LibriSpeech: 10 speakers (5 female, 5 male), 4 utterances each, 3-15 s.
          FLEURS: 20 English and 20 French sentences without digits, one speaker each, 3-15 s.
  VAD     2 LibriSpeech utterances with silence or noise around and between them; speech is
          where each utterance's energy is, split at pauses of 300 ms or more.
  TURN    FLEURS English: 20 sentences whole and 20 cut at a pause inside them, each followed by
          1.5 s of silence; speech marks where it is.
  NOISE   6 s each: white, pink and brown noise, mains hum, and babble (6 LibriSpeech talkers who
          aren't among the speech clips' speakers).
SMOKE clips (what ctest runs) are a fixed subset; every clip is in FULL.

Needs ffmpeg on PATH, numpy and huggingface_hub. Deterministic: the same inputs give the same files.
"""

import argparse
import hashlib
import io
import json
import random
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import wave
from pathlib import Path

import numpy as np

RATE = 16000
SET_NAME = "vasdk_eval_v1"
MODEL_ID = "eval_audio_v1"
LIBRISPEECH_URL = "https://www.openslr.org/resources/12/test-clean.tar.gz"
LIBRISPEECH_SHA256 = "39fde525e59672dc6d1551919b1478f724438a95aa55f874b576be21967e6c23"
FLEURS_REPO = "google/fleurs"
FLEURS_REVISION = "70bb2e84b976b7e960aa89f1c648e09c59f894dd"
LICENSES = [
    "LibriSpeech ASR corpus (test-clean), Vassil Panayotov, Guoguo Chen, Daniel Povey, Sanjeev Khudanpur, "
    "CC BY 4.0, https://www.openslr.org/12",
    "FLEURS (en_us, fr_fr test), Conneau et al., Google, CC BY 4.0, https://huggingface.co/datasets/google/fleurs",
]
SEED = 2026


# ---- Downloads and decoding ----

def download(url, destination, sha256=None):
    destination = Path(destination)
    if destination.exists() and (sha256 is None or file_sha256(destination) == sha256):
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    part = destination.with_suffix(destination.suffix + ".part")
    print(f"downloading {url}", flush=True)
    with urllib.request.urlopen(url) as response, open(part, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    if sha256 and file_sha256(part) != sha256:
        part.unlink()
        sys.exit(f"{url}: SHA-256 doesn't match {sha256}")
    part.replace(destination)
    return destination


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def decode(data):
    """Any audio file's bytes as 16 kHz mono float32, through ffmpeg."""
    result = subprocess.run(["ffmpeg", "-v", "error", "-i", "pipe:0", "-f", "s16le", "-ac", "1", "-ar", str(RATE), "pipe:1"],
                            input=data, capture_output=True, check=True)
    return np.frombuffer(result.stdout, dtype="<i2").astype(np.float32) / 32768.0


def write_wav(path, samples):
    pcm = np.clip(np.round(samples * 32767.0), -32767, 32767).astype("<i2")
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(pcm.tobytes())


# ---- Speech analysis ----

def frame_energy_db(samples, frame=160):
    count = len(samples) // frame
    frames = samples[: count * frame].reshape(count, frame)
    return 10 * np.log10(np.mean(frames ** 2, axis=1) + 1e-12)


def speech_segments(samples, min_pause_ms=300, floor_db=-35.0):
    """Where the speech is, in ms: 10 ms frames within floor_db of the loudest, joined across pauses
    shorter than min_pause_ms, each padded by 20 ms."""
    energy = frame_energy_db(samples)
    voiced = energy > (np.percentile(energy, 99) + floor_db)
    segments = []
    start = None
    for index, on in enumerate(voiced):
        if on and start is None:
            start = index
        elif not on and start is not None:
            segments.append([start, index])
            start = None
    if start is not None:
        segments.append([start, len(voiced)])
    joined = []
    for segment in segments:
        if joined and (segment[0] - joined[-1][1]) * 10 < min_pause_ms:
            joined[-1][1] = segment[1]
        else:
            joined.append(segment)
    joined = [s for s in joined if (s[1] - s[0]) * 10 >= 60]   # clicks aren't speech
    total = len(samples) * 1000 // RATE
    return [(max(0, s[0] * 10 - 20), min(total, s[1] * 10 + 20)) for s in joined]


def word_gaps(samples, low=0.35, high=0.75):
    """Short dips between words inside running speech (20-100 ms, no pause), between low and high of
    the utterance, in ms: where a sentence can be stopped mid-phrase."""
    energy = frame_energy_db(samples)
    speech_level = np.percentile(energy, 90)
    quiet = energy < speech_level - 18
    gaps = []
    index = 0
    while index < len(quiet):
        if not quiet[index]:
            index += 1
            continue
        end = index
        while end < len(quiet) and quiet[end]:
            end += 1
        length = end - index
        before, after = energy[max(0, index - 10):index], energy[end:end + 10]
        if 2 <= length <= 10 and len(before) and len(after) and before.max() > speech_level - 8 and after.max() > speech_level - 8:
            gaps.append((index + end) * 5)   # the dip's middle, ms
        index = end
    total = len(energy) * 10
    return [gap for gap in gaps if low * total <= gap <= high * total]


def trimmed(samples):
    """The utterance from its first speech to its last, with 100 ms around."""
    segments = speech_segments(samples)
    if not segments:
        return samples
    start = max(0, (segments[0][0] - 100) * RATE // 1000)
    end = min(len(samples), (segments[-1][1] + 100) * RATE // 1000)
    return samples[start:end]


def rms(samples):
    return float(np.sqrt(np.mean(samples ** 2) + 1e-12))


# ---- Noise ----

def white(rng, length):
    return rng.standard_normal(length).astype(np.float32)


def coloured(rng, length, exponent):
    """1/f^exponent noise: pink is 1, brown is 2."""
    spectrum = np.fft.rfft(rng.standard_normal(length))
    frequencies = np.fft.rfftfreq(length, 1 / RATE)
    frequencies[0] = frequencies[1]
    spectrum /= frequencies ** (exponent / 2)
    noise = np.fft.irfft(spectrum, length)
    return (noise / np.max(np.abs(noise))).astype(np.float32)


def hum(length):
    t = np.arange(length) / RATE
    return sum(np.sin(2 * np.pi * 50 * k * t) / k for k in range(1, 6)).astype(np.float32)


def at_level(samples, dbfs):
    return samples * (10 ** (dbfs / 20) / rms(samples))


# ---- Sources ----

def librispeech(cache):
    archive = download(LIBRISPEECH_URL, Path(cache) / "test-clean.tar.gz", LIBRISPEECH_SHA256)
    root = Path(cache) / "LibriSpeech"
    if not (root / "test-clean").is_dir():
        print("extracting LibriSpeech test-clean", flush=True)
        with tarfile.open(archive, "r:gz") as tar:
            tar.extractall(cache, filter="data")
    genders = {}
    for line in (root / "SPEAKERS.TXT").read_text(encoding="utf-8").splitlines():
        if line.startswith(";"):
            continue
        parts = [part.strip() for part in line.split("|")]
        if len(parts) >= 3 and parts[2] == "test-clean":
            genders[parts[0]] = "female" if parts[1] == "F" else "male"
    utterances = {}
    for transcript in sorted((root / "test-clean").glob("*/*/*.trans.txt")):
        for line in transcript.read_text(encoding="utf-8").splitlines():
            identifier, text = line.split(" ", 1)
            speaker = identifier.split("-")[0]
            utterances.setdefault(speaker, []).append((identifier, text.strip(), transcript.parent / f"{identifier}.flac"))
    return genders, utterances


def fleurs(cache, language):
    from huggingface_hub import hf_hub_download
    folder = Path(cache) / "hf"
    table = hf_hub_download(FLEURS_REPO, f"data/{language}/test.tsv", repo_type="dataset", revision=FLEURS_REVISION, cache_dir=folder)
    rows = []
    for line in Path(table).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) >= 7:
            rows.append({"sentence": fields[0], "file": fields[1], "text": fields[2], "samples": int(fields[5]), "gender": fields[6].lower()})
    return rows


def fleurs_audio(cache, language, files):
    """The named files of a FLEURS test archive, decoded."""
    from huggingface_hub import hf_hub_download
    archive = hf_hub_download(FLEURS_REPO, f"data/{language}/audio/test.tar.gz", repo_type="dataset", revision=FLEURS_REVISION,
                              cache_dir=Path(cache) / "hf")
    wanted = set(files)
    found = {}
    with tarfile.open(archive, "r|gz") as tar:
        for member in tar:
            name = Path(member.name).name
            if member.isfile() and name in wanted:
                found[name] = decode(tar.extractfile(member).read())
                if len(found) == len(wanted):
                    break
    missing = wanted - set(found)
    if missing:
        sys.exit(f"FLEURS {language}: not in the archive: {sorted(missing)[:5]}")
    return found


def fleurs_sentences(rows, count, skip=(), seconds=(3.0, 15.0)):
    """count rows of distinct sentences, without digits, of the given length; one reading each."""
    seen = set(skip)
    chosen = []
    for row in sorted(rows, key=lambda r: (int(r["sentence"]), r["file"])):
        if row["sentence"] in seen or re.search(r"\d", row["text"]):
            continue
        if not seconds[0] <= row["samples"] / RATE <= seconds[1]:
            continue
        seen.add(row["sentence"])
        chosen.append(row)
    random.Random(SEED).shuffle(chosen)
    return chosen[:count]


# ---- The set ----

class EvalSet:
    def __init__(self, folder):
        self.folder = Path(folder)
        (self.folder / "clips").mkdir(parents=True, exist_ok=True)
        self.clips = []

    def add(self, identifier, kind, samples, smoke, **fields):
        path = f"clips/{identifier}.wav"
        write_wav(self.folder / path, samples)
        self.clips.append(dict(id=identifier, path=path, kind=kind, tier="SMOKE" if smoke else "FULL", **fields))

    def write(self):
        lines = ["# proto-file: VASDK_3P_IMPL/Evaluation/proto/vasdk/eval/eval_set.proto",
                 "# proto-message: vasdk.eval.EvalSet",
                 "# Generated by tools/eval-data/build_eval_set.py: rebuild rather than edit.",
                 f'name: "{SET_NAME}"',
                 'description: "Real read speech (LibriSpeech test-clean, FLEURS en_us and fr_fr) and clips composed from it"']
        lines += [f"licenses: {quoted(text)}" for text in LICENSES]
        for clip in self.clips:
            parts = [f"id: {quoted(clip['id'])}", f"path: {quoted(clip['path'])}", f"kind: {clip['kind']}", f"tier: {clip['tier']}"]
            for key in ("language", "text", "speaker", "gender", "source", "noise"):
                if clip.get(key):
                    parts.append(f"{key}: {quoted(clip[key])}")
            if clip.get("turn_complete"):
                parts.append("turn_complete: true")
            for start, end in clip.get("speech", []):
                parts.append(f"speech {{ start_ms: {start} end_ms: {end} }}")
            lines.append("clips { " + " ".join(parts) + " }")
        (self.folder / "references.txtpb").write_text("\n".join(lines) + "\n", encoding="utf-8")
        (self.folder / "ATTRIBUTION.md").write_text(
            "# VASDK evaluation set\n\nClips cut, resampled to 16 kHz mono and mixed from these corpora, "
            "each under CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/):\n\n"
            + "".join(f"- {text}\n" for text in LICENSES)
            + "\nSynthetic noises (white, pink, brown, hum) are generated. Built by tools/eval-data/build_eval_set.py "
              "in the VASDK repository, https://github.com/fatehmtd/VASDK.\n", encoding="utf-8")


def quoted(text):
    return json.dumps(text, ensure_ascii=False)


def build(cache, out):
    folder = Path(out) / SET_NAME
    if folder.exists():
        shutil.rmtree(folder)
    clips = EvalSet(folder)
    rng = np.random.default_rng(SEED)

    # LibriSpeech: 5 female and 5 male speakers, 4 utterances of 3-15 s each
    genders, utterances = librispeech(cache)
    speakers = sorted(utterances, key=int)
    random.Random(SEED).shuffle(speakers)
    chosen = [s for s in speakers if genders.get(s) == "female"][:5] + [s for s in speakers if genders.get(s) == "male"][:5]
    babble_speakers = [s for s in speakers if s not in chosen][:6]
    librispeech_audio = {}
    for speaker_index, speaker in enumerate(chosen):
        kept = 0
        for identifier, text, path in utterances[speaker]:
            samples = decode(path.read_bytes())
            if not 3.0 <= len(samples) / RATE <= 15.0:
                continue
            librispeech_audio[identifier] = samples
            clips.add(f"ls_{identifier}", "SPEECH", samples, smoke=kept == 0 or (kept == 1 and speaker_index % 2 == 0),
                      language="en-US", text=text.lower(), speaker=f"librispeech-{speaker}", gender=genders[speaker],
                      source=f"LibriSpeech test-clean {identifier}")
            kept += 1
            if kept == 4:
                break

    # FLEURS: English and French sentences for STT and TTS; English ones for turns, apart
    english = fleurs(cache, "en_us")
    french = fleurs(cache, "fr_fr")
    english_speech = fleurs_sentences(english, 20)
    turn_rows = fleurs_sentences(english, 80, skip={row["sentence"] for row in english_speech}, seconds=(3.0, 12.0))
    french_speech = fleurs_sentences(french, 20)
    english_audio = fleurs_audio(cache, "en_us", [row["file"] for row in english_speech + turn_rows])
    french_audio = fleurs_audio(cache, "fr_fr", [row["file"] for row in french_speech])
    for language, rows, audio, short in (("en-US", english_speech, english_audio, "en"), ("fr-FR", french_speech, french_audio, "fr")):
        for index, row in enumerate(rows):
            clips.add(f"fleurs_{short}_{row['sentence']}", "SPEECH", trimmed(audio[row["file"]]), smoke=index < 5, language=language,
                      text=row["text"], speaker=f"fleurs-{short}-{row['file']}", gender=row["gender"],
                      source=f"FLEURS {short} test {row['sentence']} {row['file']}")

    # TURN: sentences stopped mid-phrase (at a short dip between words in running speech, not at a
    # pause, faded out over 20 ms), and other sentences whole; each followed by 1.5 s of silence
    silence_after = np.zeros(int(1.5 * RATE), dtype=np.float32)
    fade = np.linspace(1.0, 0.0, int(0.02 * RATE), dtype=np.float32)
    complete = cut = 0
    for row in turn_rows:
        samples = trimmed(english_audio[row["file"]])
        dither = at_level(white(rng, len(silence_after)), -70)
        gaps = word_gaps(samples)
        if gaps and cut < 20:
            middle = len(samples) * 1000 // RATE * 0.55
            at = min(gaps, key=lambda gap: abs(gap - middle)) * RATE // 1000
            part = samples[:at].copy()
            part[-len(fade):] *= fade
            clips.add(f"turn_cut_{row['sentence']}", "TURN", np.concatenate([part, silence_after + dither]), smoke=cut < 5,
                      language="en-US", speech=[(speech_segments(samples)[0][0], at * 1000 // RATE)],
                      source=f"FLEURS en test {row['sentence']} {row['file']}, stopped at {at * 1000 // RATE} ms")
            cut += 1
        elif complete < 20:
            clips.add(f"turn_complete_{row['sentence']}", "TURN", np.concatenate([samples, silence_after + dither]), smoke=complete < 5,
                      language="en-US", text=row["text"], turn_complete=True, speech=speech_segments(samples),
                      source=f"FLEURS en test {row['sentence']} {row['file']}")
            complete += 1
    print(f"turn clips: {complete} complete, {cut} cut", flush=True)

    # NOISE: generated noises and babble, 6 s, at -30 dBFS
    length = 6 * RATE
    babble = np.zeros(length, dtype=np.float32)
    for speaker in babble_speakers:
        talk = np.concatenate([decode(path.read_bytes()) for _, _, path in utterances[speaker][:4]])
        offset = int(rng.integers(0, max(1, len(talk) - length)))
        part = talk[offset:offset + length]
        babble[: len(part)] += part / rms(part)
    noises = {"white": white(rng, length), "pink": coloured(rng, length, 1), "brown": coloured(rng, length, 2),
              "hum": hum(length), "babble": babble}
    for index, (name, samples) in enumerate(noises.items()):
        clips.add(f"noise_{name}", "NOISE", at_level(samples, -30), smoke=True, noise=name,
                  source="LibriSpeech test-clean speakers " + ", ".join(babble_speakers) if name == "babble" else "generated")

    # VAD: two utterances of one speaker with silence or noise before, between and after
    backgrounds = [("silence", None, None), ("white", "white", 30), ("pink", "pink", 20), ("babble", "babble", 15),
                   ("silence2", None, None), ("brown", "brown", 20), ("hum", "hum", 25), ("babble2", "babble", 10)]
    identifiers = sorted(librispeech_audio)
    for index, (name, noise, snr) in enumerate(backgrounds):
        first, second = (trimmed(librispeech_audio[identifiers[(2 * index + k) % len(identifiers)]]) for k in (0, 1))
        gaps = [int(rng.uniform(0.8, 2.0) * RATE), int(rng.uniform(0.8, 2.0) * RATE), int(rng.uniform(1.0, 2.0) * RATE)]
        pieces, segments, at = [], [], 0
        for gap, utterance in ((gaps[0], first), (gaps[1], second), (gaps[2], None)):
            pieces.append(np.zeros(gap, dtype=np.float32))
            at += gap
            if utterance is not None:
                segments += [(at * 1000 // RATE + s, at * 1000 // RATE + e) for s, e in speech_segments(utterance)]
                pieces.append(utterance)
                at += len(utterance)
        mix = np.concatenate(pieces)
        speech_level = rms(np.concatenate([first, second]))
        if noise:
            background = np.resize(noises[noise], len(mix))
            mix = mix + background / rms(background) * speech_level / (10 ** (snr / 20))
        else:
            mix = mix + at_level(white(rng, len(mix)), -70)   # a recording's floor, not digital zero
        clips.add(f"vad_{name}", "VAD", mix, smoke=index < 3, language="en-US", speech=segments,
                  source=f"LibriSpeech test-clean {identifiers[(2 * index) % len(identifiers)]} and "
                         f"{identifiers[(2 * index + 1) % len(identifiers)]}" + (f", {noise} at {snr} dB SNR" if noise else ""))

    clips.write()
    kinds = {}
    for clip in clips.clips:
        kinds[clip["kind"]] = kinds.get(clip["kind"], 0) + 1
    print(f"{len(clips.clips)} clips: {kinds}; smoke: {sum(c['tier'] == 'SMOKE' for c in clips.clips)}")
    return folder


def set_files(out):
    out = Path(out)
    return [path for path in sorted((out / SET_NAME).rglob("*")) if path.is_file()]


def manifest(out, base_url=None, flat=False):
    """A model-store manifest of the set's files: base_url/<path> (base_url/<file name> when flat, as
    GitHub release assets are), else file:// URLs."""
    out = Path(out)
    files = []
    for path in set_files(out):
        relative = path.relative_to(out).as_posix()
        name = path.name if flat else relative
        url = f"{base_url.rstrip('/')}/{name}" if base_url else path.resolve().as_uri()
        files.append({"path": relative, "url": url, "size": path.stat().st_size, "sha256": file_sha256(path)})
    document = {"schema": 1,
                "comment": "Generated by tools/eval-data/build_eval_set.py: the evaluation set (docs/evaluation.md).",
                "models": [{"id": MODEL_ID,
                            "description": "VASDK evaluation set v1: real read speech with references, and clips composed from it",
                            "providers": [], "license": "CC-BY-4.0",
                            "license_url": "https://creativecommons.org/licenses/by/4.0/", "files": files}]}
    (out / "manifest.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return document


def github_release(out, target):
    """Uploads the set's files as the assets of a GitHub release (created as a prerelease when it
    doesn't exist, so it is never the repository's latest release); the base URL of their downloads."""
    repository, tag = target.split(":", 1)
    files = set_files(out)
    names = [path.name for path in files]
    if len(set(names)) != len(names):
        sys.exit("two files of the set share a name: release assets are flat")
    exists = subprocess.run(["gh", "release", "view", tag, "--repo", repository], capture_output=True).returncode == 0
    if not exists:
        subprocess.run(["gh", "release", "create", tag, "--repo", repository, "--prerelease", "--title", f"Evaluation set ({SET_NAME})",
                        "--notes", "The clips VASDK's provider evaluation runs on (docs/evaluation.md): LibriSpeech test-clean and "
                                   "FLEURS (CC BY 4.0, see ATTRIBUTION.md). Downloaded through "
                                   "VASDK_3P_IMPL/Evaluation/data/manifest.json, which pins each file's SHA-256."],
                       check=True)
    for start in range(0, len(files), 40):
        subprocess.run(["gh", "release", "upload", tag, "--repo", repository, "--clobber", *map(str, files[start:start + 40])], check=True)
    print(f"uploaded {len(files)} files to {repository} release {tag}")
    return f"https://github.com/{repository}/releases/download/{tag}"


def upload(out, repository):
    from huggingface_hub import HfApi
    api = HfApi()
    api.create_repo(repository, repo_type="dataset", exist_ok=True)
    commit = api.upload_folder(folder_path=str(Path(out) / SET_NAME), path_in_repo=SET_NAME, repo_id=repository, repo_type="dataset",
                               commit_message="VASDK evaluation set (tools/eval-data/build_eval_set.py)")
    revision = commit.oid
    print(f"uploaded to {repository} at {revision}")
    return f"https://huggingface.co/datasets/{repository}/resolve/{revision}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", required=True, help="where the corpora are downloaded and extracted")
    parser.add_argument("--out", required=True, help="where the set and manifest.json are written")
    parser.add_argument("--github-release", metavar="OWNER/REPO:TAG", help="a GitHub release to upload the files to; the manifest then points at them")
    parser.add_argument("--upload", metavar="REPO", help="a Hugging Face dataset repo to upload to; the manifest then points at it")
    parser.add_argument("--base-url", help="the URL the files are served under, for a manifest without --upload")
    parser.add_argument("--manifest-only", action="store_true", help="rewrite manifest.json for an existing set")
    arguments = parser.parse_args()
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg isn't on PATH")
    if not arguments.manifest_only:
        build(arguments.cache, arguments.out)
    flat = bool(arguments.github_release)
    if arguments.github_release:
        base_url = github_release(arguments.out, arguments.github_release)
    elif arguments.upload:
        base_url = upload(arguments.out, arguments.upload)
    else:
        base_url = arguments.base_url
    manifest(arguments.out, base_url, flat)
    print(f"wrote {Path(arguments.out) / 'manifest.json'}")


if __name__ == "__main__":
    main()
