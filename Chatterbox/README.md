# Chatterbox/ — Chatterbox Turbo TTS integration

The TTS half of the plugin. Parallel to `LiteRtLm/` (LLM runtime) and
`OnnxRuntime/` (ORT binaries). Consumes the ORT runtime; adds
Chatterbox-specific model staging, an inference pipeline, and a
Blueprint subsystem.

**Phase A (this commit)** adds only the model-staging script and the
version pin. Later phases will add the tokenizer, the model runners,
the pipeline, the subsystem, and the authoring tools.

## Why Chatterbox Turbo?

Of the open-source TTS options we evaluated (see
`Plugins/InoAgents/CLAUDE.md` once it's updated in Phase F), Chatterbox
Turbo is the best fit for the plugin's constraints:

- MIT license on both **code and weights** — no commercial-tier friction.
- **Zero-shot voice cloning** from a short (5-10 s) reference clip.
- **Streaming** by design: ~470 ms first chunk on desktop GPU, sub-second
  target on mobile.
- **Paralinguistic tags** (`[laugh]`, `[sigh]`, `[cough]`, etc.) native
  in the prompt, which lets writers embed stage directions directly in
  NPC dialogue.
- **Official ONNX bundle** from `ResembleAI/chatterbox-turbo-ONNX` — no
  PyTorch runtime needed.
- **English-only**, **350 M params** (smaller than the 500 M base
  Chatterbox), **distilled 1-step decoder** — matches our fixed-cast,
  English-first, mobile-friendly requirements.

## Directory layout

```
Chatterbox/
├── CHATTERBOX_VERSION            ← pinned variant + revision ("fp16@main")
├── scripts/
│   ├── setup-chatterbox.ps1      ← dev-time model downloader
│   └── authoring/                ← (Phase E) voice-embedding extraction
└── README.md                     ← this file
```

At runtime the UInoChatterboxSubsystem downloads the same files on the
player's device into `PersistentDownloadDir/InoAgents/Models/Chatterbox/
<variant>/`. The script in this folder is the dev-time equivalent — it
populates the exact same path on the dev machine so editor smoke tests
work without waiting on a first-run download.

## Model components

The `ResembleAI/chatterbox-turbo-ONNX` repo exposes four logical
components, each in five quantization variants (fp32 / fp16 / q4 /
q4f16 / quantized). The runtime pipeline uses three; the fourth
(`speech_encoder`) runs at authoring time only.

| Component            | Role                                            | Runtime? |
|----------------------|-------------------------------------------------|----------|
| `language_model`     | T3 backbone — autoregressive text → speech tokens | ✅       |
| `embed_tokens`       | Token embedding lookup                          | ✅       |
| `conditional_decoder`| S3Gen mel decoder + HiFi-GAN vocoder (merged)   | ✅       |
| `speech_encoder`     | Reference audio → speaker embedding              | Dev-time only |

Weights larger than ~2 GB spill into a `<name>.onnx_data` companion
file. The setup script downloads both together automatically.

### Variant size trade-off

Approximate total on-disk size for the three runtime components:

| Variant    | Total  | Quality vs fp32         | Targeted at |
|------------|--------|-------------------------|-------------|
| fp32       | ~2.3 GB | reference              | local-only benchmark |
| **fp16**   | ~1.1 GB | essentially identical  | desktop default |
| q4         | ~470 MB | small quality drop     | intermediate |
| **q4f16**  | ~380 MB | similar drop, smaller   | mobile default |
| quantized  | ~760 MB | int8 — varies by model  | niche |

The plugin defaults to **fp16 on Windows** and **q4f16 on Android**.
The Phase D subsystem selects the variant at runtime based on
`PLATFORM_WINDOWS` / `PLATFORM_ANDROID`. The setup script here defaults
to fp16 (override with `-Variant q4f16` for mobile-parity testing).

## Usage

```powershell
# First time / after a version bump:
cd Plugins/InoAgents/Chatterbox/scripts
./setup-chatterbox.ps1

# Pull the mobile-parity variant too (downloads to a separate directory):
./setup-chatterbox.ps1 -Variant q4f16

# Include the authoring-only speech_encoder (needed for voice-embedding
# extraction, not for runtime synthesis):
./setup-chatterbox.ps1 -IncludeAuthoring

# Force re-download even if locally cached:
./setup-chatterbox.ps1 -Force
```

The script is idempotent — re-running with no flags downloads only what
is missing or stale (compares local size vs HuggingFace Content-Length).

## Bumping the version

1. Edit `CHATTERBOX_VERSION`. Set the revision to a specific HF commit
   hash (check the "history" tab on HF for the latest safe revision).
2. Re-run `./setup-chatterbox.ps1`. New files download, old ones stay
   cached (rename to a per-revision subdir later if cross-version testing
   becomes common).
3. Run the smoke tests from Phase B-D:
   - `Ino.Chatterbox.TokenizerTest`
   - `Ino.Chatterbox.LoadModelsTest`
   - `Ino.Chatterbox.SynthTest`
   - `Ino.Chatterbox.RoundTripTest`
4. Commit `CHATTERBOX_VERSION` + any code tweaks the new version needed.

## Shipping games

At ship time, the `UInoChatterboxSubsystem::LoadModelsAsync` call on
first boot downloads the same files to the same layout on the player's
device via `FHttpModule`. No pre-staging is required on the player's
machine. Download progress fires through `OnDownloadProgress` delegates
for loading-screen UI.

Developers who want to verify their packaged game's download path works
can delete the subsystem's target dir before launch:

```
rm -r <project>/Saved/PersistentDownloadDir/InoAgents/Models/Chatterbox/
```

...which forces a fresh download via the subsystem's HTTP path rather
than the dev-time script.

## See also

- `Plugins/InoAgents/OnnxRuntime/README.md` — the ORT runtime this
  pipeline depends on.
- `Plugins/InoAgents/CLAUDE.md` § *ONNX Runtime (the second runtime)* —
  broader rationale for the ORT + dynamic-loading isolation.
- `Source/InoAgents/Public/Chatterbox/` — Blueprint-facing API (Phase D).
- `Source/InoAgents/Private/Chatterbox/` — inference pipeline internals
  (Phases B-C).
