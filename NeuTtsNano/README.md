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

## Status — v1 shipping ✓

Full pipeline verified end-to-end: `Ino.NeuTtsNano.SynthTest` loads the Q4
model in ~1.5 s and synthesises intelligible voice-cloned speech (Jo's
reference voice, encoded from Neuphonic's `jo.wav` sample). Output is 24
kHz mono int16 PCM LE bytes delivered via the `OnComplete` delegate,
directly consumable by RuntimeAudioImporter's `UStreamingSoundWave`.

## Scope of v1 (locked)

| Aspect | Value |
|---|---|
| Input text | **Pre-phonemized IPA string.** v1 does not include text-to-phoneme. Callers supply phonemes (e.g. `"hɛloʊ maɪ neɪm ɪz ændi."`). An ONNX G2P model lands in a follow-up milestone. |
| Voices | **One default voice baked in** — `Resources/default_voice.nvoice.json` ships with Neuphonic's `jo.wav` pre-encoded (653 FSQ codes, 251-char IPA phones). Custom voices deferred. |
| Backbone variant | **Q4 only** (`neutts-nano-Q4_0.gguf`, 195 MB). Q8 / FP16 deferred. |
| Output | **One-shot.** Full utterance delivered via `OnComplete` as 24 kHz mono int16 PCM bytes. Streaming deferred. |
| Platform | **Win64 verified.** Android packaging untouched (all dependencies have Android builds); device verification is a follow-up. |

## Observed numbers (CPU, Alder Lake — `ggml-cpu-alderlake.dll`)

- **Model load (warm cache):** ~1.5 s
- **LM generation:** ~120 tok/s at Q4 on CPU
- **NeuCodec decoder:** ~70 ms per second of audio
- **Short utterance (2.5 s audio output):** ~10 s wall-clock (0.25× real-time)

The dominant cost is **prompt prefill** — linear in reference-voice length. Shorter reference WAVs (3-5 s) cut prefill significantly. Vulkan offload via `FInoNeuTtsNanoModelConfig::NumGpuLayers > 0` is available but untested; the llama.cpp Vulkan backend registers at module startup on hosts with a working driver.

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
Schema:

```json
{
  "display_name": "Default (Jo)",
  "ref_text":   "Verbatim English transcript of the reference WAV.",
  "ref_phones": "vɝbeɪtɪm ˈɪŋɡlɪʃ tɹænskɹɪpt əv ðə ɹɛfɹəns wav",
  "ref_codes":  [18578, 55378, /* ~200-750 int32 FSQ codes */]
}
```

- **`ref_text`** — original transcript, preserved for diagnostic
  purposes (shown in logs). Not consumed by synthesis.
- **`ref_phones`** — IPA phonemization of `ref_text` via espeak-ng
  (produced by the Python encoder). **This is what the runtime prompt
  builder concatenates** with the caller's pre-phonemized target text.
- **`ref_codes`** — FSQ speech-token ids from NeuCodec's encoder
  (50 Hz token rate). Feeds into the Qwen2 prompt's assistant preamble
  as `<|speech_N|>` tokens for voice conditioning.

The committed file ships with Neuphonic's own `jo.wav` sample
(Apache 2.0) pre-encoded — 653 codes (13 s @ 50 Hz), 251-char IPA
phones for "So I just tried Neuphonic and I'm genuinely impressed...".
To replace with your own voice, see below.

### Why this is a separate file (and why encoding happens offline)

NeuTTS Nano's voice-cloning path conditions the LM on a short sequence
of FSQ speech tokens encoded from a reference audio sample. Neuphonic
ships the NeuCodec **decoder** as an ONNX file (783 MB, we use it at
runtime), but the **encoder** is PyTorch-only. There's no ONNX / GGUF
export of the encoder, and running PyTorch inside a UE game process is
not realistic. So we encode once offline (via `encode-default-voice.py`)
and ship the resulting small int32 array as a JSON asset.

Separately, NeuTTS Nano was trained on IPA phonemes from espeak-ng —
raw English won't align. We phonemize at encode time too (storing
`ref_phones` alongside `ref_codes`), keeping espeak-ng out of the
runtime plugin (GPLv3 concern for commercial games).

## Regenerating the default voice

### Prerequisites

Python packages (one-time):

```
pip install neucodec librosa soundfile torch phonemizer
```

CPU-only PyTorch is fine — this is a one-shot offline encode.

Plus **espeak-ng** as a system package (phonemizer's backend):

```
winget install -e --id eSpeak-NG.eSpeak-NG    # Windows
brew install espeak-ng                         # macOS
sudo apt install espeak-ng                     # Debian/Ubuntu
```

On Windows, the script expects `libespeak-ng.dll` at the default
install path (`C:/Program Files/eSpeak NG/`). Set `PHONEMIZER_ESPEAK_LIBRARY`
if it's installed elsewhere.

### Run (recommended — conda-based isolated env)

```powershell
conda create -n neuttstest python=3.11 -y
conda activate neuttstest
pip install --index-url https://download.pytorch.org/whl/cpu torch
pip install neucodec librosa soundfile phonemizer

# Set espeak-ng hints (Windows):
$env:PHONEMIZER_ESPEAK_LIBRARY = "C:/Program Files/eSpeak NG/libespeak-ng.dll"
$env:PHONEMIZER_ESPEAK_PATH    = "C:/Program Files/eSpeak NG/espeak-ng.exe"

cd Plugins/InoAgents/NeuTtsNano
python scripts/encode-default-voice.py `
    --input-wav <path-to-reference.wav> `
    --ref-text  "<verbatim transcript of the WAV>" `
    --display-name "My Voice"
```

This overwrites `Resources/default_voice.nvoice.json`. Commit the
resulting JSON — it's ~10-30 KB, tracked normally.

### The script does

1. Loads the WAV at 16 kHz mono via librosa.
2. Downloads `neuphonic/neucodec` from HuggingFace (first run only; cached).
3. Runs `NeuCodec.encode_code` on the WAV → int32 FSQ codes at 50 Hz.
4. Phonemizes the transcript via espeak-ng (unless `--language none`).
5. Writes all four fields (`display_name`, `ref_text`, `ref_phones`, `ref_codes`)
   to JSON.

Total time: ~30 s on first run (model download), ~5 s on subsequent runs.

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
| `FInoNeuTtsNanoModelEntry` USTRUCT + delegates | `Source/InoAgents/Public/NeuTtsNano/InoNeuTtsNanoTypes.h` |
| Subsystem public API | `Source/InoAgents/Public/NeuTtsNano/InoNeuTtsNanoSubsystem.h` |
| Subsystem impl (download + ThreadPool load + SynthesizeAsync) | `Source/InoAgents/Private/NeuTtsNano/InoNeuTtsNanoSubsystem.cpp` |
| Runner (llama_model + llama_context + FInoOnnxSession owner) | `Source/InoAgents/Private/NeuTtsNano/InoNeuTtsNanoRunner.{h,cpp}` |
| FRunnable worker + AR loop + full synthesis pipeline | `Source/InoAgents/Private/NeuTtsNano/InoNeuTtsNanoSynthesisWorker.{h,cpp}` |
| Prompt builder + voice registry | `Source/InoAgents/Private/NeuTtsNano/InoNeuTtsNanoPromptBuilder.{h,cpp}` + `InoNeuTtsNanoVoiceRegistry.{h,cpp}` |
| Smoke tests | `Source/InoAgents/Private/SmokeTests/InoNeuTtsNano*.{h,cpp}` |

The runtime subsystem is a **pure consumer** of our existing
`InoLlamaCppModule` vtable (for the backbone) and `FInoOnnxSession`
(for the decoder). No changes to those runtimes are needed for NeuTTS.

## Using it from Blueprint / C++

```cpp
UInoNeuTtsNanoSubsystem* Subsys =
    GetGameInstance()->GetSubsystem<UInoNeuTtsNanoSubsystem>();

FInoNeuTtsNanoModelConfig Cfg;
Cfg.Variant         = EInoNeuTtsNanoBackboneVariant::Q4;
Cfg.NumGpuLayers    = 0;       // 99 = all on Vulkan (Win64 only, untested)
Cfg.NumContextTokens = 2048;

FOnInoNeuTtsNanoModelLoaded OnLoaded;
OnLoaded.BindDynamic(this, &MyClass::HandleModelLoaded);
Subsys->LoadModelAsync(Cfg, OnLoaded);
```

Then after OnLoaded fires with bSuccess=true:

```cpp
FInoNeuTtsNanoSynthesisOptions Opts;    // defaults match NeuTTS upstream
                                         // (TopK=50, Temperature=1.0, Seed=-1)

FOnInoNeuTtsNanoSynthesisComplete OnDone;
OnDone.BindDynamic(this, &MyClass::HandlePcm);

Subsys->SynthesizeAsync(
    TEXT("hɛloʊ wɝːld"),     // pre-phonemized IPA
    FName(TEXT("Default")),   // voice name
    Opts,
    OnDone);
```

The PCM you get back is 24 kHz mono int16 LE — feed it directly into
`UStreamingSoundWave::AppendAudioDataFromRAW` (RuntimeAudioImporter)
or save to WAV via `UInoAudioFunctionLibrary::SaveInt16PcmAsWav`.

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
