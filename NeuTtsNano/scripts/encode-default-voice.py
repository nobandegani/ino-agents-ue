#!/usr/bin/env python3
"""
encode-default-voice.py

Offline helper that runs Neuphonic's PyTorch NeuCodec encoder on a WAV
file + transcript and emits the .nvoice.json file that
UInoNeuTtsNanoSubsystem's voice registry loads at runtime.

NeuTTS Nano's voice-cloning flow needs a pre-encoded sequence of FSQ
speech tokens derived from a reference audio sample (3-15 seconds of
clean, continuous monologue). The encoder is PyTorch-only — there is no
ONNX / GGUF export of the NeuCodec encoder. Running the encoder at
runtime inside our UE plugin is therefore not feasible. Instead, voices
are encoded offline (via this script) and the resulting int32 code
array + transcript are shipped as a small JSON asset the runtime reads.

------------------------------------------------------------------------
Required pip deps (one-time):

    pip install neucodec librosa soundfile torch phonemizer

(librosa's default audio backend is soundfile, which needs to be
installed alongside it. torch CPU-only is fine; this script doesn't
need CUDA.)

Additionally, the phonemizer library needs espeak-ng installed as a
separate system package (it's the backend phonemizer uses to produce
IPA). On Windows:

    winget install -e --id eSpeak-NG.eSpeak-NG

On macOS/Linux:

    brew install espeak-ng                      # macOS
    sudo apt install espeak-ng                  # Debian/Ubuntu

Why we phonemize here rather than at synth time:
  NeuTTS Nano was trained on IPA phonemes (produced by espeak-ng),
  not on raw English. The runtime UE plugin does not carry a
  phonemizer in v1 (espeak-ng is GPLv3 — doesn't mix with commercial
  games; an ONNX G2P model lands in a follow-up milestone). Running
  the phonemizer once offline here and storing the result in the
  voice JSON lets the plugin skip phonemization entirely until v2
  arrives.

------------------------------------------------------------------------
Typical usage (default output path is the plugin's baked-in voice):

    python encode-default-voice.py \\
        --input-wav samples/jo.wav \\
        --ref-text "My name is Andy. I just moved to London."

That overwrites ../Resources/default_voice.nvoice.json which is then
loaded by UInoNeuTtsNanoSubsystem at Initialize time as the "Default"
voice.

Custom voices (future) will live at
Plugins/InoAgents/NeuTtsNano/Resources/voices/<voice-name>.nvoice.json
— this script can output there too via --output + --display-name.

------------------------------------------------------------------------
Reference audio guidelines (from Neuphonic upstream):

  - Mono, 16-44 kHz (script downsamples to 16 kHz regardless)
  - 3 to 15 seconds in length
  - Clean audio — minimal background noise
  - Natural, continuous speech (monologue / conversation, not singing)
  - WAV format

The transcript (--ref-text) must match exactly what's spoken in the WAV.
Mismatch between audio and transcript degrades voice cloning quality.
"""

import argparse
import json
import pathlib
import sys


def die(msg: str, code: int = 1) -> None:
    sys.stderr.write(f"error: {msg}\n")
    sys.exit(code)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Encode a reference WAV to a NeuTTS Nano .nvoice.json.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--input-wav", required=True, type=pathlib.Path,
        help="Path to the reference audio WAV (mono, 16-44 kHz, 3-15 sec).",
    )
    parser.add_argument(
        "--ref-text", required=True,
        help="Verbatim transcript of the reference WAV.",
    )
    parser.add_argument(
        "--output", type=pathlib.Path, default=None,
        help="Output JSON path. Defaults to "
             "Plugins/InoAgents/NeuTtsNano/Resources/default_voice.nvoice.json "
             "relative to this script.",
    )
    parser.add_argument(
        "--codec-repo", default="neuphonic/neucodec",
        help="HuggingFace repo (or local path) for NeuCodec. Default: %(default)s.",
    )
    parser.add_argument(
        "--display-name", default="Default",
        help="Display name baked into the JSON (shown in editor UI). "
             "Default: %(default)s.",
    )
    parser.add_argument(
        "--language", default="en-us",
        help="espeak-ng language code for phonemizing --ref-text. "
             "Default: %(default)s. Set to 'none' to skip phonemization "
             "and store only ref_text (not recommended — the runtime "
             "plugin needs ref_phones to build the chat prompt).",
    )
    args = parser.parse_args()

    # Import heavy deps AFTER arg parsing so --help is snappy and so missing
    # pip packages produce a focused error message.
    try:
        import torch
        import librosa
        from neucodec import NeuCodec
    except ImportError as e:
        die(f"{e}\n\nInstall with:\n"
            f"    pip install neucodec librosa soundfile torch")

    # Phonemizer import is optional only if --language=none.
    phonemize_fn = None
    if args.language.lower() != "none":
        try:
            from phonemizer import phonemize
            phonemize_fn = phonemize
        except ImportError as e:
            die(f"{e}\n\nPhonemization is required unless --language=none. "
                f"Install with:\n"
                f"    pip install phonemizer\n\n"
                f"phonemizer also needs espeak-ng installed as a system "
                f"package — see the comment at the top of this script.")

    if not args.input_wav.is_file():
        die(f"--input-wav does not exist: {args.input_wav}")

    if args.output is None:
        args.output = (
            pathlib.Path(__file__).resolve().parent
            / ".." / "Resources" / "default_voice.nvoice.json"
        ).resolve()

    # Load audio at 16 kHz mono — matches the upstream Python pipeline
    # (neutts/neutts.py :: encode_reference).
    print(f"[1/3] Loading {args.input_wav} at 16 kHz mono...")
    wav, sr = librosa.load(str(args.input_wav), sr=16000, mono=True)
    duration_s = len(wav) / float(sr)
    print(f"      {len(wav)} samples ({duration_s:.2f} s at {sr} Hz)")
    if duration_s < 3.0 or duration_s > 15.0:
        sys.stderr.write(
            f"warning: duration {duration_s:.2f}s is outside the recommended "
            f"3-15 s range. Voice cloning quality may be degraded.\n"
        )

    # Load NeuCodec model (downloads weights on first run, cached afterwards).
    print(f"[2/3] Loading NeuCodec from {args.codec_repo}...")
    codec = NeuCodec.from_pretrained(args.codec_repo)
    codec.eval()

    # Encode: librosa gives us numpy, codec wants torch [1, 1, T].
    print(f"[3/3] Encoding...")
    wav_tensor = torch.from_numpy(wav).float().unsqueeze(0).unsqueeze(0)
    with torch.no_grad():
        ref_codes = codec.encode_code(audio_or_path=wav_tensor)
    # encode_code returns shape [1, 1, T_codes]; flatten to 1D int list.
    ref_codes_list = [int(c) for c in ref_codes.squeeze(0).squeeze(0).cpu().numpy().tolist()]
    print(f"      Encoded to {len(ref_codes_list)} FSQ codes "
          f"(token rate: {len(ref_codes_list) / duration_s:.1f} Hz)")

    # Phonemize the reference transcript (or skip if --language=none).
    ref_phones = ""
    if phonemize_fn is not None:
        print(f"      Phonemizing ref_text via espeak-ng "
              f"(language={args.language})...")
        # Match NeuTTS's _to_phones: phonemize one string, split on
        # whitespace, rejoin with a single space. This normalises
        # espeak-ng's variable whitespace to exactly one space between
        # phonemes — matches the spacing the model was trained with.
        phones_raw = phonemize_fn([args.ref_text], language=args.language,
                                   backend="espeak", strip=True)[0]
        ref_phones = " ".join(phones_raw.split())
        print(f"      ref_phones ({len(ref_phones)} chars): {ref_phones}")

    # Write JSON.
    output_data = {
        "display_name": args.display_name,
        "ref_text":     args.ref_text,
        "ref_phones":   ref_phones,
        "ref_codes":    ref_codes_list,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(output_data, f, indent=2, ensure_ascii=False)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
