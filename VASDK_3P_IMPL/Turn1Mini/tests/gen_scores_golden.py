# Writes the score golden for turn1mini_test.cpp with the reference streaming loop of p99lab/turn-1-mini
# (p99turn/onnx_stream.py: 2,560 samples a step, every new_state_<name> fed back as state_<name>), on
# the synthetic signal below (keep it in sync with signal() in turn1mini_test.cpp).
#
# onnxruntime is pinned to the version VASDK builds with (ONNXRUNTIME_VERSION in
# VASDK_3P_IMPL/cmake/FindONNXRuntime.cmake): the int8 graph's integer kernels change between versions
# (1.31 moves the scores by up to 0.0012), and with the same version the C++ stream gives the same scores.
#
#   uv run gen_scores_golden.py <turn_1_mini.onnx> golden/scores_golden.txt     (uv reads the packages below)
#   pip install onnxruntime==1.19.2 "numpy<2" && python gen_scores_golden.py <turn_1_mini.onnx> golden/scores_golden.txt
#
# /// script
# requires-python = "==3.12.*"
# dependencies = ["onnxruntime==1.19.2", "numpy<2"]
# ///
import math
import sys

import numpy as np
import onnxruntime as ort

RATE, STEP = 16000, 2560
STATES = ("audio", "conv1", "conv2", "k", "v", "k_valid", "h6")
SPEECH_SECONDS, TOTAL_STEPS = 3.0, 30  # 3 s of a voice, then silence: 4.8 s


def signal():
    """A voice-like signal: 10 harmonics at 4 syllables a second, pitch falling at the end, then quiet."""
    samples, phase, seed = [], 0.0, 1
    for i in range(TOTAL_STEPS * STEP):
        t = i / RATE
        seed = (1103515245 * seed + 12345) % 2147483648
        noise = (seed / 2147483648.0 - 0.5) * 0.002
        value = noise
        if t < SPEECH_SECONDS:
            f0 = 140.0 if t < 2.4 else 140.0 - 50.0 * (t - 2.4) / 0.6
            phase += 2.0 * math.pi * f0 / RATE
            envelope = 0.5 * (1.0 - math.cos(2.0 * math.pi * 4.0 * t))
            voice = sum(math.sin(k * phase) / k for k in range(1, 11))
            value += 0.3 * envelope * voice
        samples.append(value)
    return np.array(samples, dtype=np.float32)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_scores_golden.py <turn_1_mini.onnx> <out.txt>")
    options = ort.SessionOptions()
    options.intra_op_num_threads = options.inter_op_num_threads = 1
    session = ort.InferenceSession(sys.argv[1], options, providers=["CPUExecutionProvider"])
    state = {i.name: np.zeros(i.shape, np.float32) for i in session.get_inputs() if i.name != "audio"}
    outputs = ["probs"] + [f"new_state_{s}" for s in STATES]
    audio = signal()
    scores = []
    for start in range(0, len(audio), STEP):
        probs, *new = session.run(outputs, {"audio": audio[start:start + STEP].reshape(1, STEP), **state})
        state = {f"state_{s}": v for s, v in zip(STATES, new)}
        scores.extend(float(p) for p in probs)
    with open(sys.argv[2], "w", newline="\n") as out:
        out.write(f"# turn-1-mini scores, gen_scores_golden.py, onnxruntime {ort.__version__}\n")
        out.write(f"{len(scores)}\n")
        for score in scores:
            out.write(f"{score:.7f}\n")
    print(f"{len(scores)} scores, last step: {' '.join(f'{s:.3f}' for s in scores[-8:])}")


if __name__ == "__main__":
    main()
