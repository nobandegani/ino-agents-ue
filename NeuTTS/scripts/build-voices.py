#!/usr/bin/env python3
"""
Convert vendored NeuTTS sample voices (.pt + .txt) into .nvoice.json files
for runtime consumption by the InoNeuTtsNative UE module.

The .pt files are PyTorch tensors of int speech codes produced by the
NeuCodec encoder (50 Hz, single codebook). The .txt files are the
transcripts of the matching .wav files.

Phonemization is intentionally NOT done here — the runtime calls
InoSpeakNG (espeak-ng) to phonemize ref_text on first use. This keeps
the script's only dependency at PyTorch.

Usage:
    pip install torch
    python build-voices.py

Run from anywhere; paths are resolved relative to this script.
"""

import json
import sys
from pathlib import Path

try:
    import torch
except ImportError:
    sys.exit("This script requires PyTorch. Install with: pip install torch")


# Voice → eSpeak language code. Matches vendor/samples/.
VOICE_LANGUAGE = {
    "jo":       "en-us",
    "dave":     "en-us",
    "greta":    "de",
    "juliette": "fr-fr",
    "mateo":    "es",
}


def main():
    script_dir  = Path(__file__).resolve().parent
    samples_dir = (script_dir / ".." / "vendor" / "samples").resolve()
    voices_dir  = (script_dir / ".." / "voices").resolve()

    if not samples_dir.exists():
        sys.exit(f"Samples directory not found: {samples_dir}\n"
                 f"Did you initialize the NeuTTS submodule?")

    voices_dir.mkdir(parents=True, exist_ok=True)

    pt_files = sorted(samples_dir.glob("*.pt"))
    if not pt_files:
        sys.exit(f"No .pt files found in {samples_dir}")

    print(f"Samples:     {samples_dir}")
    print(f"Voices out:  {voices_dir}")
    print()

    for pt_file in pt_files:
        name     = pt_file.stem
        txt_file = samples_dir / f"{name}.txt"
        out_file = voices_dir  / f"{name}.nvoice.json"

        if not txt_file.exists():
            print(f"  [skip] {name}: missing {txt_file.name}")
            continue

        # Load codes (tensor of ints) and flatten to a Python list.
        codes_tensor = torch.load(pt_file, map_location="cpu", weights_only=True)
        codes = [int(c) for c in codes_tensor.flatten().tolist()]

        ref_text = txt_file.read_text(encoding="utf-8").strip()
        language = VOICE_LANGUAGE.get(name, "en-us")

        voice = {
            "name":      name,
            "language":  language,
            "ref_text":  ref_text,
            "ref_codes": codes,
        }

        out_file.write_text(
            json.dumps(voice, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

        print(f"  [ok]   {name:10s}  language={language:6s}  codes={len(codes):4d}  -> {out_file.name}")

    print()
    print("Done.")


if __name__ == "__main__":
    main()
