# Qwen3-ASR-0.6B via LiteRT

On-device automatic speech recognition for `InoAgents`, wrapping the
LiteRT (TFLite) export of [Qwen3-ASR-0.6B](https://huggingface.co/Qwen/Qwen3-ASR-0.6B).

This folder holds the **runtime assets** (the `.tflite` model, the
tokenizer, and any sample audio used for smoke tests). The C++ code that
loads and runs them lives at
`Plugins/InoAgents/Source/InoQwen3ASRLiteRT/`.

## What this model is

- **Architecture:** encoder-decoder transformer built on Qwen3-Omni.
  Audio → audio encoder → autoregressive language decoder → text tokens.
- **Parameters:** ~0.6 B (0.9 B after embedding etc. per the Qwen card).
- **Audio input:** 16 kHz mono PCM float32, 5-second window (= 80 000
  samples) for the `_5s_*` variants.
- **Output:** UTF-8 text (after detokenization with the Qwen3 BPE tokenizer).
- **Decoding:** autoregressive; the runner orchestrates one encoder call
  followed by an N-step decode loop with KV cache.
- **License:** Apache 2.0 (both the base model and the LiteRT export).

The streaming and forced-aligner features described on the Qwen page are
**not in scope** for v1 of this integration — we target offline
transcription of fixed-length 5-second windows.

## Files this directory needs

```
Qwen3ASR/LiteRT/
├── README.md                                 ← this file
├── models/
│   └── qwen3_asr_0.6b_5s_i8.tflite           ← ~794 MB int8-quantized model
└── tokenizer/
    └── tokenizer.json                        ← Qwen3 BPE tokenizer (from base repo)
```

The litert-community repo only ships the `.tflite` files. The tokenizer
lives in the base Qwen repo and must be downloaded separately.

### Downloading

Model (Win64 PowerShell):
```powershell
mkdir Plugins\InoAgents\Qwen3ASR\LiteRT\models
Invoke-WebRequest `
  -Uri "https://huggingface.co/litert-community/Qwen3-ASR-0.6B/resolve/main/qwen3_asr_0.6b_5s_i8.tflite" `
  -OutFile "Plugins\InoAgents\Qwen3ASR\LiteRT\models\qwen3_asr_0.6b_5s_i8.tflite"
```

Tokenizer:
```powershell
mkdir Plugins\InoAgents\Qwen3ASR\LiteRT\tokenizer
Invoke-WebRequest `
  -Uri "https://huggingface.co/Qwen/Qwen3-ASR-0.6B/resolve/main/tokenizer.json" `
  -OutFile "Plugins\InoAgents\Qwen3ASR\LiteRT\tokenizer\tokenizer.json"
```

(Tokenizer download can wait until Phase 3 — Phase 1's smoke test only
needs the `.tflite`.)

## Variants

The litert-community repo also publishes per-Qualcomm-SoC variants
(`_Qualcomm_SA8255`, `_SM8650`, etc., ~1.89 GB each). These are
ahead-of-time-compiled for specific NPU hardware and not portable to other
devices. We deliberately ignore them and ship only the
`qwen3_asr_0.6b_5s_i8.tflite` (~794 MB, runs on CPU/XNNPack everywhere).

The repo also has an `_f32` (full-precision, ~3.13 GB) variant. We don't
ship it because the i8 version is nearly indistinguishable in WER and
~4× smaller — but it's available as a drop-in replacement if quality
issues surface.

## Implementation phases

### Phase 1 — load + introspect *(in progress)*

Console command: `Ino.Qwen3ASRLiteRT.LoadTest`. Loads the `.tflite`, dumps
every signature with input/output names + dtypes + shapes, runs one
forward pass with zero-filled inputs as a runtime sanity check.

**Decides Phase 2's shape.** The signature layout tells us whether the
model is one-signature (audio + prev tokens → next logits), split into
encoder + decoder signatures, or multi-signature with externally-bound
KV (like the Gemma/Llama LiteRT exports).

### Phase 2 — inference orchestrator

`FInoQwen3ASRRunner` owns the env + compiled model, drives the audio
encoder pass and the autoregressive decode loop. Exact API depends on
Phase 1's signature dump — possibilities:
- One signature: caller maintains `prev_tokens` array and calls
  `Run(audio, prev_tokens)` → next token in a loop.
- Two signatures: cache encoder hidden states once, then loop
  `decode(hidden, prev_tokens) → next` until EOS.
- Multi-signature with external KV bindings: bind 24-layer KV buffers
  once via `LiteRtAddExternalTensorBinding`, run prefill then decode.

Sampling is greedy (`argmax`) — ASR has no creativity dimension to tune.

### Phase 3 — tokenizer

`FInoQwen3ASRTokenizer` parses Qwen3's `tokenizer.json` (HuggingFace
tokenizers format — BPE with byte-level pre-tokenization, similar in
shape to GPT-2 BPE). Decode-only is sufficient for v1 (we don't tokenize
text inputs — the model takes audio, the only direction is token-IDs →
text).

### Phase 4 — audio loader

`UInoAudioFunctionLibrary` (already in InoAgents) provides WAV I/O
helpers. We add a thin layer:
- Load WAV bytes → PCM int16 / float32
- Resample to 16 kHz mono if needed (UE has resampling built into the
  audio engine; for offline conversion a simple linear resampler suffices)
- Pad / truncate to exactly 80 000 samples per inference window

### Phase 5 — `UInoQwen3ASRLiteRTSubsystem`

`UGameInstanceSubsystem` exposing:
- `LoadModelAsync(OnLoaded)` — off-game-thread model + tokenizer load
- `TranscribeAsync(WavPath, OnComplete)` — full pipeline: load audio →
  encode → decode loop → detokenize → fire `OnComplete(FString Text)`
- `IsModelLoaded()`
- `CancelTranscription()`

Plus a `UInoQwen3ASRLiteRTTranscribe` Blueprint async-action wrapper.

### Phase 6 — end-to-end smoke test

Console command `Ino.Qwen3ASRLiteRT.TranscribeTest <wav-path>` that runs
the full pipeline and logs the resulting text. Verifies against a known
sample (e.g., LibriSpeech clip).

## Architecture decisions

- **CPU-only for v1.** GPU acceleration via WebGPU on Win64 / OpenCL on
  Android is gated behind an `EAcceleratorPreference` setting once Phase 5
  lands. The XNNPack CPU path is what every device gets out of the box.
- **No streaming.** The 5-second window export is fundamentally batch-
  oriented. Streaming ASR (continuous recognition) requires a different
  model export (chunked encoder, persistent KV state) — out of scope.
- **No speaker diarization, no timestamps, no punctuation control.**
  The base Qwen-ASR model supports these via its forced-aligner sibling
  (`Qwen3-ForcedAligner-0.6B`); we don't ship that.
- **English-first, but the underlying model is multilingual.** Language
  is auto-detected by the model — no per-call language hint in v1.
