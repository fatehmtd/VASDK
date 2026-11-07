"""Kitchen timers, each on a thread of its own; when one ends, on_done(label) is called."""

import math
import re
import threading
import time


def parse_duration(value):
    """Seconds from a number of minutes (9, "9") or an ISO 8601 duration ("PT9M", "PT1H30M", as
    @sys.duration gives it)."""
    if isinstance(value, (int, float)):
        return float(value) * 60
    text = str(value).strip().upper()
    if re.fullmatch(r"\d+(\.\d+)?", text):
        return float(text) * 60
    match = re.fullmatch(r"P(?:(\d+)D)?(?:T(?:(\d+(?:\.\d+)?)H)?(?:(\d+(?:\.\d+)?)M)?(?:(\d+(?:\.\d+)?)S)?)?", text)
    if not match or not any(match.groups()):
        raise ValueError(f"not a duration: {value!r}")
    days, hours, minutes, seconds = (float(part) if part else 0.0 for part in match.groups())
    return ((days * 24 + hours) * 60 + minutes) * 60 + seconds


def duration_text(seconds):
    """"9 minutes", "1 hour and 30 minutes", "45 seconds": as said."""
    seconds = int(round(seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    parts = []
    for amount, unit in ((hours, "hour"), (minutes, "minute"), (secs, "second")):
        if amount:
            parts.append(f"{amount} {unit}" + ("s" if amount != 1 else ""))
    if not parts:
        return "0 seconds"
    return parts[0] if len(parts) == 1 else ", ".join(parts[:-1]) + " and " + parts[-1]


class Timers:
    def __init__(self, on_done, clock=time.monotonic):
        self._on_done = on_done
        self._clock = clock
        self._lock = threading.Lock()
        self._running = []   # {"label", "seconds", "ends", "thread"}

    def start(self, seconds, label="kitchen"):
        if seconds <= 0:
            raise ValueError("a timer needs a length")
        label = label or "kitchen"
        timer = {"label": label, "seconds": seconds, "ends": self._clock() + seconds}
        thread = threading.Timer(seconds, self._ring, args=(timer,))
        thread.daemon = True
        timer["thread"] = thread
        with self._lock:
            self._running.append(timer)
        thread.start()
        return {"label": label, "duration_text": duration_text(seconds), "count": self.count(), "summary": self.summary()}

    def cancel(self, label=None):
        with self._lock:
            cancelled = [t for t in self._running if label in (None, "", t["label"])]
            self._running = [t for t in self._running if t not in cancelled]
        for timer in cancelled:
            timer["thread"].cancel()
        if not cancelled:
            return {"count": self.count(), "summary": "There's no timer running."}
        what = "the timer" if len(cancelled) == 1 else f"{len(cancelled)} timers"
        return {"count": self.count(), "summary": f"I cancelled {what}."}

    def count(self):
        with self._lock:
            return len(self._running)

    def status(self):
        return {"count": self.count(), "summary": self.summary(), "timers": self.running()}

    def running(self):
        """Each timer and the seconds it has left, soonest first: for the LLM's context."""
        now = self._clock()
        with self._lock:
            # Rounded up: just set, a 10 minute timer has "10 minutes" left, not "9 minutes and 59 seconds"
            return sorted(({"label": t["label"], "seconds_left": max(0, math.ceil(t["ends"] - now - 0.05))} for t in self._running),
                          key=lambda t: t["seconds_left"])

    def summary(self):
        timers = self.running()
        if not timers:
            return "There's no timer running."
        said = [f"{duration_text(t['seconds_left'])} left on the {t['label']} timer" for t in timers]
        return "There's " + (said[0] if len(said) == 1 else ", ".join(said[:-1]) + " and " + said[-1]) + "."

    def _ring(self, timer):
        with self._lock:
            if timer not in self._running:
                return   # cancelled
            self._running.remove(timer)
        self._on_done(timer["label"])
