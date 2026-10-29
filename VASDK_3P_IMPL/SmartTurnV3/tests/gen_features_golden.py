# Writes the feature golden for features_test.cpp with the reference preprocessing of
# pipecat-ai/smart-turn (audio_utils.truncate_audio_to_last_n_seconds + WhisperFeatureExtractor),
# shortened from 8 s to MAX_LEN samples to keep the golden small.
#
#   uv run --python 3.12 --with transformers --with numpy gen_features_golden.py golden/features_golden.txt
import math
import sys

import numpy as np
from transformers import WhisperFeatureExtractor

N, MAX_LEN = 6000, 8000  # keep in sync with features_test.cpp
audio = np.array([
    (8000 * math.sin(2 * math.pi * 440 * i / 16000)
     + 3000 * math.sin(2 * math.pi * 1234.5 * i / 16000 + 0.3)
     + 200 * ((i * 7919) % 101 - 50)) / 32768
    for i in range(N)
], dtype=np.float32)

# truncate_audio_to_last_n_seconds: keep the tail, left-pad with zeros
audio = np.pad(audio[-MAX_LEN:], (MAX_LEN - min(N, MAX_LEN), 0))

fe = WhisperFeatureExtractor(chunk_length=8)
feats = fe(audio, sampling_rate=16000, return_tensors="np", padding="max_length",
           max_length=MAX_LEN, truncation=True, do_normalize=True).input_features[0]  # [80, frames]

with open(sys.argv[1], "w") as f:
    f.write(f"{feats.shape[1]} {feats.shape[0]}\n")
    for row in feats.T:  # time-major, like SmartTurnV3Impl::getFeatures()
        f.write(" ".join(f"{v:.6f}" for v in row) + "\n")
