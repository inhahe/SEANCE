#!/usr/bin/env python3
"""
pack_waveforms.py — build the factory single-cycle waveform asset for SEANCE.

SEANCE ships a built-in library of pre-made single-cycle oscillator shapes,
sourced from the public-domain Adventure Kid Waveforms (AKWF) set. Rather than
committing ~4000 loose .wav files (or embedding them in the binary), we pack the
whole library into ONE small file, cpp/resources/waveforms.bin, which the app
loads at runtime (see cpp/src/waveform_bank.{h,cpp}). This script regenerates
that asset from a local AKWF checkout.

Usage:
    # 1. clone the public-domain AKWF set somewhere:
    git clone --depth 1 https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE.git
    # 2. point this at its canonical `AKWF/` folder (65 themed sub-folders):
    python pack_waveforms.py --src /path/to/AKWF-FREE/AKWF \
                             --out ../resources/waveforms.bin

Each source wav is mono / 16-bit / 600 samples / one cycle. We:
  * read the 600 samples,
  * subtract the DC offset (a single-cycle oscillator should be centred — any
    offset just adds an inaudible-but-clipping bias),
  * resample to a fixed 512 samples (linear) — the size of a Drawn/Freehand
    WaveLayer's drawnSamples, so importing is a straight copy,
  * peak-normalise to 0.99,
  * store as int16 (×32767) to keep the asset ~4 MB rather than ~8 MB float.

Categories come from the AKWF sub-folder name via CATEGORY_NAMES (falls back to a
title-cased version). A curated "best of" subset is flagged per the CURATED rules
so the app can ★-star those and sort them to the top of each category.

Binary format — see cpp/src/waveform_bank.h for the authoritative spec.
"""

import argparse
import os
import struct
import sys
import wave

SAMPLE_COUNT = 512  # must match WaveformBank::kSampleCount

# --- folder name -> human display category ----------------------------------
CATEGORY_NAMES = {
    "aguitar": "Acoustic Guitar",
    "altosax": "Alto Sax",
    "birds": "Birds",
    "bitreduced": "Bit Reduced",
    "bw_blended": "Basic \u2014 Blended",
    "bw_perfectwaves": "Basic \u2014 Perfect Waves",
    "bw_saw": "Basic \u2014 Saw",
    "bw_sawbright": "Basic \u2014 Saw Bright",
    "bw_sawgap": "Basic \u2014 Saw Gap",
    "bw_sawrounded": "Basic \u2014 Saw Rounded",
    "bw_sin": "Basic \u2014 Sine",
    "bw_squ": "Basic \u2014 Square",
    "bw_squrounded": "Basic \u2014 Square Rounded",
    "bw_tri": "Basic \u2014 Triangle",
    "c604": "C604",
    "cello": "Cello",
    "clarinett": "Clarinet",
    "clavinet": "Clavinet",
    "dbass": "Double Bass",
    "distorted": "Distorted",
    "ebass": "Electric Bass",
    "eguitar": "Electric Guitar",
    "eorgan": "Electric Organ",
    "epiano": "Electric Piano",
    "flute": "Flute",
    "fmsynth": "FM Synth",
    "granular": "Granular",
    "hdrawn": "Hand Drawn",
    "hvoice": "Voice (Harmonic)",
    "linear": "Linear",
    "oboe": "Oboe",
    "oscchip": "Chip Osc",
    "overtone": "Overtone",
    "piano": "Piano",
    "pluckalgo": "Plucked",
    "raw": "Raw",
    "sinharm": "Sine Harmonics",
    "snippets": "Snippets",
    "stereo": "Stereo",
    "stringbox": "String Box",
    "symetric": "Symmetric",
    "theremin": "Theremin",
    "vgame": "Video Game",
    "vgamebasic": "Video Game Basic",
    "violin": "Violin",
}

# Emit categories in this order; anything not listed is appended afterward in
# folder order. Basic shapes first (the most-reached-for), then tuned
# instruments, then character/algorithmic, then the big numbered assorted banks.
CATEGORY_ORDER = [
    "bw_perfectwaves", "bw_sin", "bw_tri", "bw_saw", "bw_sawbright",
    "bw_sawgap", "bw_sawrounded", "bw_squ", "bw_squrounded", "bw_blended",
    "piano", "epiano", "eorgan", "clavinet",
    "aguitar", "eguitar", "ebass", "dbass", "stringbox",
    "violin", "cello", "altosax", "clarinett", "flute", "oboe", "theremin",
    "hvoice", "fmsynth", "overtone", "sinharm", "pluckalgo",
    "distorted", "bitreduced", "oscchip", "vgame", "vgamebasic", "c604",
    "granular", "hdrawn", "linear", "symetric", "snippets", "raw", "birds",
]


def category_for(folder):
    key = folder[5:] if folder.lower().startswith("akwf_") else folder
    if key in CATEGORY_NAMES:
        return CATEGORY_NAMES[key]
    # Numbered banks AKWF_0001..0020 -> "Assorted 01".."Assorted 20".
    if key.isdigit():
        return "Assorted " + str(int(key)).zfill(2)
    return key.replace("_", " ").title()


def is_curated(folder, index_in_folder, folder_count):
    """Heuristic curation (~150-220 waveforms) without auditioning each one.

    * Clean classic shapes (perfect waves + the canonical sine/saw/squ/tri sets):
      mark a generous-but-not-flooding slice.
    * Tuned instruments: the first couple per family — a representative each.
    * Character/algorithmic banks: a sparse every-Nth sampling for variety.
    Returns True if this wav should be flagged curated.
    """
    key = folder[5:] if folder.lower().startswith("akwf_") else folder

    # The pristine fundamental shapes — surface them strongly.
    if key == "bw_perfectwaves":
        return True
    if key in ("bw_sin", "bw_tri"):
        return index_in_folder < 6
    if key in ("bw_squ", "bw_squrounded"):
        return index_in_folder < 4
    if key in ("bw_saw", "bw_sawbright", "bw_sawrounded", "bw_sawgap"):
        return index_in_folder % 8 == 0  # every 8th across the morph
    if key == "bw_blended":
        return index_in_folder % 6 == 0

    # One or two representatives per tuned-instrument family.
    instruments = {
        "piano", "epiano", "eorgan", "clavinet", "aguitar", "eguitar",
        "ebass", "dbass", "stringbox", "violin", "cello", "altosax",
        "clarinett", "flute", "oboe", "theremin", "hvoice",
    }
    if key in instruments:
        return index_in_folder < 2

    # Character / algorithmic — sparse sampling for variety.
    character = {
        "fmsynth", "overtone", "sinharm", "pluckalgo", "distorted",
        "bitreduced", "oscchip", "vgame", "c604",
    }
    if key in character:
        return index_in_folder % 12 == 0

    return False


def read_cycle(path):
    """Return a list of 512 floats in [-1,1]: DC-removed, resampled, normalised."""
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2:
        return None  # only 16-bit sources expected
    total = len(raw) // 2
    ints = struct.unpack("<%dh" % total, raw)
    # Down-mix to mono if stereo.
    if ch > 1:
        mono = [sum(ints[i:i + ch]) / ch for i in range(0, total, ch)]
    else:
        mono = list(ints)
    if not mono:
        return None
    # DC removal.
    mean = sum(mono) / len(mono)
    mono = [s - mean for s in mono]
    # Linear resample to SAMPLE_COUNT.
    src = len(mono)
    out = []
    for i in range(SAMPLE_COUNT):
        pos = i * src / SAMPLE_COUNT
        i0 = int(pos)
        frac = pos - i0
        a = mono[i0 % src]
        b = mono[(i0 + 1) % src]
        out.append(a + (b - a) * frac)
    # Peak normalise to 0.99.
    peak = max(abs(s) for s in out)
    if peak > 1e-9:
        scale = 0.99 / peak
        out = [s * scale for s in out]
    return out


def main():
    ap = argparse.ArgumentParser(description="Pack AKWF wavs into waveforms.bin")
    ap.add_argument("--src", required=True,
                    help="path to AKWF-FREE/AKWF (folder of themed sub-folders)")
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(__file__), "..", "resources", "waveforms.bin"))
    ap.add_argument("--limit", type=int, default=0,
                    help="debug: cap waveforms per folder")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        print("error: --src is not a directory:", src, file=sys.stderr)
        return 2

    folders = sorted(d for d in os.listdir(src)
                     if os.path.isdir(os.path.join(src, d)))
    # Apply CATEGORY_ORDER (by folder key suffix), unknowns appended in folder order.
    def order_key(folder):
        key = folder[5:] if folder.lower().startswith("akwf_") else folder
        try:
            return (0, CATEGORY_ORDER.index(key))
        except ValueError:
            # Numbered assorted banks sort after named ones, by number.
            if key.isdigit():
                return (2, int(key))
            return (1, folder)
    folders.sort(key=order_key)

    entries = []   # (name, category, curated)
    blobs = []     # list of 512-float lists
    for folder in folders:
        fdir = os.path.join(src, folder)
        wavs = sorted(f for f in os.listdir(fdir) if f.lower().endswith(".wav"))
        if args.limit:
            wavs = wavs[:args.limit]
        category = category_for(folder)
        for idx, fname in enumerate(wavs):
            cyc = read_cycle(os.path.join(fdir, fname))
            if cyc is None:
                continue
            name = os.path.splitext(fname)[0]
            curated = is_curated(folder, idx, len(wavs))
            entries.append((name, category, curated))
            blobs.append(cyc)

    if not entries:
        print("error: no waveforms found under", src, file=sys.stderr)
        return 1

    # Stable sort entries (and parallel blobs) so each category is contiguous,
    # curated-first, then by name — exactly the browser's display order.
    cat_seq = {}
    for _, cat, _ in entries:
        if cat not in cat_seq:
            cat_seq[cat] = len(cat_seq)
    order = sorted(range(len(entries)), key=lambda i: (
        cat_seq[entries[i][1]],          # category emission order
        0 if entries[i][2] else 1,       # curated first
        entries[i][0],                   # then by name
    ))
    entries = [entries[i] for i in order]
    blobs = [blobs[i] for i in order]

    curated_n = sum(1 for _, _, c in entries if c)
    out_path = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(b"SSWB")
        f.write(struct.pack("<III", 1, SAMPLE_COUNT, len(entries)))
        for name, category, curated in entries:
            nb = name.encode("utf-8")
            cb = category.encode("utf-8")
            f.write(struct.pack("<B", 1 if curated else 0))
            f.write(struct.pack("<H", len(nb)))
            f.write(nb)
            f.write(struct.pack("<H", len(cb)))
            f.write(cb)
        for cyc in blobs:
            ints = [max(-32767, min(32767, int(round(s * 32767)))) for s in cyc]
            f.write(struct.pack("<%dh" % SAMPLE_COUNT, *ints))

    size = os.path.getsize(out_path)
    print("Wrote %s" % out_path)
    print("  %d waveforms, %d categories, %d curated (%.1f%%)" % (
        len(entries), len(cat_seq), curated_n, 100.0 * curated_n / len(entries)))
    print("  %.2f MB" % (size / (1024 * 1024)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
