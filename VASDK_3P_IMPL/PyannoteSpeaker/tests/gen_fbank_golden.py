# Writes the fbank golden for fbank_test.cpp with the reference front end of pyannote's
# WeSpeaker ResNet34 (pyannote/audio models/embedding/wespeaker), before mean normalization.
#
#   uv run --python 3.12 --with torch --with torchaudio gen_fbank_golden.py golden/fbank_golden.txt
import math
import sys

import torch
import torchaudio.compliance.kaldi as kaldi

N = 4000  # keep in sync with fbank_test.cpp
signal = [
    8000 * math.sin(2 * math.pi * 440 * i / 16000)
    + 3000 * math.sin(2 * math.pi * 1234.5 * i / 16000 + 0.3)
    + 200 * ((i * 7919) % 101 - 50)
    for i in range(N)
]
feats = kaldi.fbank(
    torch.tensor(signal, dtype=torch.float32).unsqueeze(0),
    num_mel_bins=80,
    frame_length=25,
    frame_shift=10,
    dither=0.0,
    sample_frequency=16000,
    window_type="hamming",
    use_energy=False,
)
with open(sys.argv[1], "w") as f:
    f.write(f"{feats.shape[0]} {feats.shape[1]}\n")
    for row in feats.tolist():
        f.write(" ".join(f"{v:.6f}" for v in row) + "\n")
