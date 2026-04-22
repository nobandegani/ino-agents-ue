# NeuTtsNano/ — NeuTTS Nano integration for the InoAgents plugin

This directory holds the **version-pinned defaults, offline helper
scripts, and baked-in default voice** for the plugin's NeuTTS Nano
subsystem — an on-device TTS that combines:

- **Backbone (Qwen2-derived ~117M GGUF)** via our existing llama.cpp
  runtime (see `../LlamaCpp/`) to generate FSQ speech tokens.
- **NeuCodec ONNX decoder** via our existing ONNX Runtime (see
  `../OnnxRuntime/`) to turn those tokens into 24 kHz mono audio.

Parallel to `Plugins/InoAgents/Chatterbox/` — a second on-device TTS
with a different voice character, smaller footprint, and different
licensing / runtime model.

## Scope of v1 (locked)

| Aspect | Value |
|---|---|
| Input text | **Pre-phonemized IPA string.** v1 does not include text-to-phoneme. Callers supply phonemes (e.g. `"h ə l oʊ m aɪ n eɪ m ɪ z ˈæ n d i"`). An ONNX G2P model lands in a follow-up milestone. |
| Voices | **One default voice baked in** via `Resources/default_voice.nvoice.json`. Custom voices deferred. |
| Backbone variant | **Q4 only** (`neutts-nano-Q4_0.gguf`, 195 MB). Q8 / FP16 deferred. |
| Output | **One-shot.** Full utterance delivered via `OnComplete` as 24 kHz mono int16 PCM bytes. Streaming deferred. |
| Platform | **Win64 verified.** Android packaging untouched (all dependencies have Android builds); device verification is a follow-up. |

## Directory layout

```
NeuTtsNano/
├── Resources/
│   └── default_voice.nvoice.json   ← the baked-in default voice
│                                     (ref_text + pre-encoded int32 ref_codes)
├── scripts/
│   ├── encode-default-voice.py     ← OFFLINE PyTorch encoder —
│   │                                 regenerates the JSON from a WAV
│   ├── setup-neutts-nano.ps1       ← OPTIONAL dev pre-stage (warms
│   │                                 PersistentDownloadDir cache)
│   └── clean.ps1                   ← wipes staged + cache dirs
├── .gitignore
└── README.md                       ← this file
```

## The default voice JSON

`Resources/default_voice.nvoice.json` is read at subsystem init by
`FInoNeuTtsNanoVoiceRegistry` and registered under the name `"Default"`.
Shape:

```json
{
  "display_name": "Default",
  "ref_text": "Verbatim transcript of the reference WAV.",
  "ref_codes": [4299, 6561, 123, 456, /* ~200-750 int32 FSQ codes */]
}
```

The committed file is a **placeholder** — it has empty `ref_text` and
`ref_codes`. Synthesis will fail with a clear error until the JSON is
regenerated from a real reference WAV. See below.

### Why this is a separate file (and why encoding happens offline)

NeuTTS Nano's voice-cloning path conditions the LM on a short sequence
of FSQ speech tokens encoded from a reference audio sample. Neuphonic
ships the NeuCodec **decoder** as an ONNX file (783 MB, we use it at
runtime), but the **encoder** is PyTorch-only. There's no ONNX / GGUF
export of the encoder, and running PyTorch inside a UE game process is
not realistic. So we encode once offline (via `encode-default-voice.py`)
and ship the resulting small int32 array as a JSON asset.

## Regenerating the default voice

### Prerequisites

A one-time Python install:

```
pip install neucodec librosa soundfile torch
```

CPU-only PyTorch is fine; this script is a one-shot encode, not a
hot path.

### Run

From this folder:

```
python scripts/encode-default-voice.py \
    --input-wav <path-to-reference.wav> \
    --ref-text "<verbatim transcript of the WAV>"
```

This overwrites `Resources/default_voice.nvoice.json`. Commit the
resulting JSON — it's ~5-20 KB, tracked normally.

### Reference audio guidelines (from Neuphonic upstream)

- Mono WAV, 16–44 kHz (script downsamples to 16 kHz regardless)
- 3 to 15 seconds in length
- Clean audio — minimal background noise
- Natural continuous speech — a monologue or conversational utterance.
  Not singing, not clipped syllables.

The transcript (`--ref-text`) must match exactly what's spoken in the
WAV. Mismatch degrades cloning quality.

### License note on the WAV source

Pick a WAV whose license allows redistribution — the encoded tokens
are arguably a derivative work. Neuphonic's own `samples/jo.wav` +
`samples/jo.txt` in their `neuphonic/neutts` GitHub repo (Apache 2.0)
is a safe source. Recording your own with a clean signing statement
is also fine.

## Pre-staging the model files (optional)

On first `LoadModelAsync` call, `UInoNeuTtsNanoSubsystem` downloads two
files from HuggingFace into UE's PersistentDownloadDir:

- `neutts-nano-Q4_0.gguf` (195 MB — GGUF backbone)
- `model.onnx` (783 MB — NeuCodec ONNX decoder)

That's ~1 GB and takes a couple of minutes on a typical connection.
For dev iteration, run this script **once**:

```powershell
cd Plugins/InoAgents/NeuTtsNano/scripts
./setup-neutts-nano.ps1
```

It downloads the two files into PersistentDownloadDir under the correct
subdirectory layout. Subsequent `LoadModelAsync` calls find them and
skip the runtime download.

Source URLs are in the Project Settings (`Edit → Project Settings →
Plugins → InoAgents → NeuTTS Nano`). The script reads its URLs from
command-line args defaulting to the same HF repos; pass `-BackboneUrl`
/ `-CodecUrl` to override.

## How this relates to the runtime subsystem

| Concern | Where it lives |
|---|---|
| Default voice JSON, offline encoder, dev pre-stage | here — `NeuTtsNano/` |
| Settings UI (backbone URL, codec URL, revision) | `Source/InoAgents/Public/InoAgentsSettings.h` |
| `FInoNeuTtsNanoModelEntry` USTRUCT | `Source/InoAgents/Public/NeuTtsNano/InoNeuTtsNanoTypes.h` |
| Subsystem + download + runner + worker | `Source/InoAgents/{Public,Private}/NeuTtsNano/` |
| Smoke tests | `Source/InoAgents/Private/SmokeTests/InoNeuTtsNano*.{h,cpp}` |

The runtime subsystem is a **pure consumer** of our existing
`InoLlamaCppModule` vtable (for the backbone) and `FInoOnnxSession`
(for the decoder). No changes to those runtimes are needed for NeuTTS.

## Upstream references

- **Official Python pipeline**: https://github.com/neuphonic/neutts
  (`neutts/neutts.py` — the reference inference loop we're porting)
- **Backbone model card**: https://huggingface.co/neuphonic/neutts-nano-q4-gguf
- **Codec decoder model card**: https://huggingface.co/neuphonic/neucodec-onnx-decoder
- **NeuCodec paper / method**: https://huggingface.co/neuphonic/neucodec
  (0.8 kbps FSQ, single codebook, 50 Hz token rate, 24 kHz upsampling decoder)
- **License**: the NeuTTS Nano backbone ships under a custom "NeuTTS Open
  License 1.0" — NOT Apache 2.0. Read and check compliance before any
  commercial deployment.
