#!/usr/bin/env python3
"""Render the title tune (and the win/lose jingles) from src/main.c to a WAV.

main.c is the single source of truth: the note tables (NR23/NR24 period
pairs) and the frames-per-note constants are parsed straight out of it, so
tuning by ear is just editing the C, regenerating, and listening. A (0, 0)
pair is a rest.

The synth is intentionally simple: a 50%-duty square at the note's frequency,
with a linear decay over a fraction of a second to suggest the APU's
envelope. This is a sketch for the ear, not a simulator; the real sound is
mgba's business, as main.c says.

    python3 tools/render_titles.py out.wav       # default /tmp/titles.wav
"""
import math
import os
import re
import struct
import sys
import wave

RATE = 44100
DECAY_S = 0.22          # seconds a note takes to fade (envelope suggestion)


def parse_main(path):
    """Return the note tables and steps, straight from the C source."""
    src = open(path).read()

    def table(name):
        m = re.search(r"static const uint8_t %s\[\]\s*=\s*\{([^}]*)\}" % name, src, re.S)
        return [int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]

    def define(name):
        m = re.search(r"#define %s\s+\d+" % name, src)
        return int(m.group(0).split()[-1])

    return {
        "title_notes": (table("title_notes"), define("TITLE_STEP")),
        "win_notes":   (table("win_notes"), define("SFX_STEP")),
        "lose_notes":  (table("lose_notes"), define("SFX_STEP")),
    }


def synth(freq, seconds):
    """A 50% duty square, decayed linearly: the APU suggestion. 0 Hz = rest."""
    n = int(RATE * seconds)
    out = []
    vol = 0.5
    for i in range(n):
        if freq and i < n:
            sq = 1.0 if (int(freq * i / RATE) & 1) else -1.0
        else:
            sq = 0.0
        out.append(sq * vol)
        vol = max(0.0, 0.5 - i / RATE * (0.5 / DECAY_S))
    return out


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/gb-titles.wav"
    src = parse_main(os.path.join(os.path.dirname(__file__), "..", "src", "main.c"))
    pcm = []
    for name in ("title_notes", "win_notes", "lose_notes"):
        notes, step = src[name]
        pcm.extend(synth(0, 0.1))                       # a breath between tunes
        for i in range(0, len(notes), 2):
            lo, hi = notes[i], notes[i + 1]
            period = (hi & 7) * 256 + lo if hi else 0
            freq = 131072.0 / (2048 - period) if period and hi else 0.0
            pcm.extend(synth(freq, step / 60.0))        # 60 fps
        print("%s: %d notes at %d frames/note" % (name, len(notes) // 2, step))
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dB" % len(pcm),
                                  *[min(255, max(0, int(v * 96) + 128)) for v in pcm]))
    print("wrote %s (%.1fs)" % (out, len(pcm) / RATE))


if __name__ == "__main__":
    main()