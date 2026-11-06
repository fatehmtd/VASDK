"""The computer's microphone and speaker, as 16-bit mono PCM, through sounddevice (PortAudio):
pip install sounddevice. Only voice mode needs them; without it, the demo still runs, silently."""

import threading
import time

try:
    import sounddevice
except ImportError:   # optional
    sounddevice = None


def available():
    return sounddevice is not None


class Speaker:
    """Plays what it's given, as it comes. busy(): it's playing, or just stopped (the room's echo)."""

    def __init__(self, rate):
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._last_sound = 0.0
        self._stream = sounddevice.RawOutputStream(samplerate=rate, channels=1, dtype="int16",
                                                   blocksize=rate // 50, callback=self._play)
        self._stream.start()

    def play(self, pcm):
        with self._lock:
            self._buffer += pcm

    def clear(self):
        """The user talked over it: what hasn't played yet is dropped."""
        with self._lock:
            self._buffer.clear()

    def busy(self, tail=0.4):
        with self._lock:
            return bool(self._buffer) or time.monotonic() - self._last_sound < tail

    def close(self):
        self._stream.stop()
        self._stream.close()

    def _play(self, out, frames, _time, _status):
        wanted = frames * 2
        with self._lock:
            chunk = bytes(self._buffer[:wanted])
            del self._buffer[:wanted]
            if chunk:
                self._last_sound = time.monotonic()
        out[:len(chunk)] = chunk
        out[len(chunk):] = b"\0" * (wanted - len(chunk))


class Microphone:
    """Hands each 100 ms of the microphone to on_pcm. While muted() holds, it hands silence instead,
    so the speech detector's clock keeps running: half duplex, for a laptop's speakers."""

    def __init__(self, rate, on_pcm, muted=lambda: False):
        self._on_pcm = on_pcm
        self._muted = muted
        self._stream = sounddevice.RawInputStream(samplerate=rate, channels=1, dtype="int16",
                                                  blocksize=rate // 10, callback=self._heard)
        self._stream.start()

    def close(self):
        self._stream.stop()
        self._stream.close()

    def _heard(self, data, _frames, _time, _status):
        pcm = bytes(data)
        self._on_pcm(b"\0" * len(pcm) if self._muted() else pcm)
