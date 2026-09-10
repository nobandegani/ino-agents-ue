# InoAgents

**On-device AI agents for Unreal Engine 5.7.** Tool-calling LLM chat and voice-cloning
text-to-speech that run inside the game process — no Python runtime, no sidecar server, no
per-request cloud bill. Plus an ElevenLabs client for when you do want the cloud.

Everything is Blueprint-first: every capability is a Blueprint-callable node or an assignable
delegate. C++ is available but never required.

> **Status: beta.** The API surface is stable enough to build on, but names and defaults can
> still move between versions. See [Known limitations](#known-limitations).

---

## What's in the box

| | Capability | Runs | Backed by |
|---|---|---|---|
| 🧠 | **On-device LLM chat with tool calling** | Locally | Google [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM), Gemma models |
| 🗣️ | **On-device voice-cloning TTS** | Locally | Neuphonic [NeuTTS Nano](https://github.com/neuphonic/neutts) + NeuCodec |
| ☁️ | **Cloud TTS** | Network | [ElevenLabs](https://elevenlabs.io/docs/api-reference) Text-to-Dialogue streaming |
| 🎭 | **Character helpers** | Locally | Procedural blink/gaze, mono PCM utilities, gyro camera sway |

The name is deliberate: the goal isn't just text generation, it's **tool-use workflows** —
the model calls into your Blueprint functions, gets results back, and keeps going.

---

## Requirements

- **Unreal Engine 5.7**
- **Visual Studio 2022** (Win64) or the matching toolchain for your target platform
- A model file — downloaded automatically on first use, see [Models](#models)

### Plugin dependencies

InoAgents composes with four sibling plugins. Clone them into your project's `Plugins/`
directory alongside this one:

| Plugin | Provides | Required for |
|---|---|---|
| **InoLiteRT** | LiteRT + LiteRT-LM C APIs, runtime binary staging, per-platform packaging | LiteRT-LM chat, NeuTTS |
| **InoSpeakNG** | eSpeak NG phonemization (IPA) | NeuTTS |
| **InoNodes** | Resumable downloader with streaming SHA-256 verification | Model auto-download |
| **RuntimeAudioImporter** | `UStreamingSoundWave` — the audio sink for TTS output | Playing TTS audio |

`JsonBlueprintUtilities` ships with the engine — just enable it.

> InoAgents core (ElevenLabs, animation, audio, camera helpers) has **no** backend
> dependencies. If you only want cloud TTS and the character helpers, the on-device
> plugins are optional.

---

## Install

```bash
cd YourProject/Plugins
git clone https://github.com/nobandegani/ino-agents-ue.git InoAgents
```

Add `InoAgents` to your `.uproject` Plugins array, regenerate project files, and build.

---

## Quick start

### On-device chat with tool calling

```cpp
UInoLiteRtLmSubsystem* Subsys =
    GetGameInstance()->GetSubsystem<UInoLiteRtLmSubsystem>();

// 1. Load (downloads + SHA-verifies on first run, cached after)
FInoLiteRtLmModelConfig Config;
Config.ModelFileName  = TEXT("Gemma 4 E2B");   // DisplayName from Project Settings
Config.Backend        = EInoLiteRtLmBackend::Cpu;
Config.SystemMessage  = TEXT("You are a terse shopkeeper in a fantasy RPG.");

FOnInoLiteRtLmModelLoaded OnLoaded;
OnLoaded.BindDynamic(this, &AMyActor::HandleModelLoaded);
Subsys->LoadModelAsync(Config, /*OnDownloadProgress=*/{}, OnLoaded);

// 2. Register any tools BEFORE creating the conversation
Subsys->RegisterTool(NewObject<UInoLiteRtLmAddNumbersTool>(this));

// 3. Converse
UInoLiteRtLmConversation* Conv = Subsys->CreateConversation();
Conv->OnToken.AddDynamic(this, &AMyActor::HandleToken);
Conv->OnSentence.AddDynamic(this, &AMyActor::HandleSentence);
Conv->OnComplete.AddDynamic(this, &AMyActor::HandleComplete);
Conv->SendMessageAsync(TEXT("What do you have for sale?"));
```

**Streaming callbacks.** `OnToken` and `OnSentence` each deliver a `RawText` / `CleanText`
pair. `CleanText` has `[bracketed]` delivery tags stripped; `RawText` keeps them. That split
exists so you can subtitle with `CleanText` while feeding `RawText` straight to ElevenLabs,
which reads `[cheerfully]` as a delivery hint.

`OnSentenceBoundary` fires right after each `OnSentence` as a payload-less cue — handy for
driving animation or viseme triggers.

Terminal events are `OnComplete(FullText)` **or** `OnError(Message)`, never both.

**Writing a tool.** Subclass `UInoLiteRtLmToolBase` in C++ or Blueprint, set `ToolName`,
`Description` and `Parameters` (a JSON schema string), and implement `Execute`. See
`UInoLiteRtLmAddNumbersTool` for the canonical example.

Tools execute on the **game thread**, so you can touch UE state freely — but they stall token
streaming while they run. For anything slow, return immediately and call
`Conv->SubmitDeferredToolResult(ToolCallId, ResultJson)` once your async work lands.

---

### On-device voice cloning

NeuTTS clones a voice from a short reference sample. You prime one voice, then synthesize
many lines against it.

```cpp
UInoNeuTTSSubsystem* TTS = GetGameInstance()->GetSubsystem<UInoNeuTTSSubsystem>();

FInoNeuTTSConfig Cfg;                       // backbone + decoder names, backends, precision
TTS->LoadModelAsync(Cfg, OnLoaded, OnProgress);

// Prime a voice asset (imported from a .inv file — see below). Required before any synth.
TTS->SetActiveVoiceAsync(MyVoiceAsset, OnReady);

// Streaming synthesis — OnAudioChunk fires repeatedly, ~0.5 s of audio per chunk
TTS->SynthesizeStreamAsync(
    TEXT("Welcome, traveller."), Options, /*ChunkTokens=*/25,
    OnAudioChunk, OnComplete);
```

Output is **24 kHz mono int16 PCM (little-endian)** — feed it straight into
`UStreamingSoundWave::AppendAudioDataFromRAW`.

Streaming does real chunked decoding with an overlap-add crossfade at chunk boundaries, not a
one-big-buffer placeholder. Decode costs roughly 14% of real-time on CPU, so playback keeps
ahead of synthesis comfortably.

**Voice assets.** A Python encoder emits a `<voice>.inv` JSON file (transcript + FSQ codes).
Drag it into the Content Browser and `UInoNeuTTSVoiceFactory` turns it into a
`UInoNeuTTSVoiceAsset`. Right-click → Reimport picks up re-encodes.

> Only one voice prime and one synthesis may be in flight at a time. Wait for `OnReady`
> before synthesizing — the worker has no inline fallback and will refuse.

---

### Cloud TTS (ElevenLabs)

A latent Blueprint node, **ElevenLabs Stream Text-to-Dialogue**, or from C++:

```cpp
UInoElevenLabsTextToDialogueStream::StreamTextToDialogue(
    this, Request, /*ApiKeyOverride=*/TEXT(""));
```

`OnAudioChunk` delivers delta bytes as they arrive, `OnComplete` the full buffer, `OnError` on
failure. Output format is selectable via `EInoElevenLabsOutputFormat` (MP3 / PCM / µ-law).
Playback is yours to own — there's no built-in audio component.

---

## Configuration

Three Project Settings pages under **Plugins**, one per module:

| Page | Holds |
|---|---|
| **InoAgents** | ElevenLabs API key, base URL, default model id + output format |
| **InoLiteRtLm** | `Models[]` — the LLM model registry |
| **InoNeuTTS** | `BackboneModels[]` + `DecoderModels[]` |

### ⚠️ A note on your ElevenLabs API key

The key is a `Config` property, so **Unreal writes it in plaintext to your project's
`Config/DefaultGame.ini`** — a file most projects commit. The editor masks the field, but the
`.ini` is not encrypted.

If your project repo is public or shared, prefer one of:

- Pass `ApiKeyOverride` to `StreamTextToDialogue` and source the key from an environment
  variable or your own secret store at runtime.
- Add `Config/DefaultGame.ini` to `.gitignore`, or keep the key out of it and inject at launch.

Leave the Project Settings field empty in any committed configuration.

### Models

Model files are gigabytes and are **never** committed. `LoadModelAsync` downloads them on
first use — HEAD probe, `.partial` staging, atomic rename, streaming SHA-256, multi-connection
range requests, exponential-backoff retries, and cancellation are all handled by InoNodes.

Files land under `FPaths::ProjectPersistentDownloadDir()`:

```
<PersistentDownloadDir>/InoAgents/LiteRTLM/<name>.litertlm
<PersistentDownloadDir>/InoAgents/NeuTTS/<name>.litertlm   (backbone)
<PersistentDownloadDir>/InoAgents/NeuTTS/<name>.tflite     (decoder)
```

Each registry entry carries a `DownloadUrl`, `LocalFileName`, and an optional
`ExpectedSha256`. Set the hash — a corrupt cache is then detected, deleted and re-fetched
automatically instead of failing mysteriously at load.

`IsModelDownloaded()` is a cheap stat probe on both subsystems, so it's safe to poll from UMG
to decide whether to show a download-progress screen.

---

## Architecture

InoAgents uses a **core + backends** module split. Each backend is a self-contained deletion
unit — it owns its settings page and its smoke tests, and core never depends on it.

```
Source/
├── InoAgents/         core — log category, ElevenLabs, animation/audio/camera helpers
├── InoLiteRtLm/       backend — Gemma chat + tool calling
├── InoNeuTTS/         backend — NeuTTS Nano voice cloning
└── InoNeuTTSEditor/   editor-only — .inv voice import factory
```

To drop a backend: delete its folder and remove its entry from `InoAgents.uplugin`. Nothing
else breaks, because game code reaches each backend through
`GetGameInstance()->GetSubsystem<...>()` on demand rather than through a compile-time
dependency. `DepricatedModules/` holds previously retired backends (llama.cpp, Chatterbox,
Qwen3 ASR, and an earlier GGUF/ONNX NeuTTS implementation) for reference — they are not built
and not in the `.uplugin`. They still reference plugins this repo no longer depends on
(`InoOnnx`, `InoLlama`), so treat them as an archive, not as buildable code.

Threading follows one rule throughout: **heavy work on a thread-pool task, every delegate
marshalled back to the game thread**, with `TWeakObjectPtr` guards so a subsystem torn down
mid-flight cancels cleanly instead of dispatching into freed memory.

---

## Smoke tests

Console commands that exercise the real API surface end to end. Run them from the editor's
Output Log. They stage from lowest level to highest, so a regression is bisectable.

Raw C API — no PIE session needed, each isolates one layer:

```
Ino.LoadEngineTest [model]                    engine load only
Ino.GenerateTest [model] [prompt]             one-shot generate, logs tokens/sec
Ino.StreamTest [model] [prompt]               per-token arrival timing + TTFT
Ino.ConversationTest [model] [turns...]       multi-turn, verifies KV cache reuse
Ino.ToolCallTest [model]                      tool round-trip
```

Subsystem level — these need a running PIE session:

```
Ino.LiteRtLm.SubsystemLoadTest [model]        download + SHA-verify + engine create
Ino.LiteRtLm.ConversationSendTest [model]     CreateConversation + SendMessageAsync
Ino.LiteRtLm.ConversationStreamTest [model]   per-token / per-sentence timing
Ino.LiteRtLm.ConversationContextTest [model]  CreateConversationWithHistory
Ino.LiteRtLm.ConversationToolTest [model]     RegisterTool through OnToolCalled
Ino.LiteRtLm.ToolRegistryTest                 registry semantics (no PIE needed)

Ino.NeuTTS.DecoderProbeTest <abs .tflite>     bare LiteRT C API spike (no PIE)
Ino.NeuTTS.BackboneSpikeTest <abs .litertlm>  SessionConfig bisect probe (no PIE)
Ino.NeuTTS.LoadTest [backbone] [decoder]      batch download + runner construction

Ino.ElevenLabs.DialogueStreamTest             cloud round-trip (needs API key)
Ino.ElevenLabs.ReloadSettings                 re-read Project Settings without restart
```

---

## Platform support

The **UE-facing API is identical on every platform.** Native runtime availability is owned by
the sibling InoLiteRT plugin — see its docs for the current per-platform matrix.

Where LiteRT-LM binaries haven't been staged, link-time stubs keep the module loading cleanly
and `LoadModelAsync` fails with a clear error instead of crashing. ElevenLabs cloud TTS and
all the animation / audio / camera helpers work everywhere regardless.

On Android, `android.permission.INTERNET` is required for model download.

---

## Known limitations

- **Session config is detached by default.** `FInoLiteRtLmModelConfig::bAttachSessionConfig`
  defaults to `false` because LiteRT-LM `v0.11.0-rc.1` returned NULL from `Conversation::Create`
  when a user `SessionConfig` was attached. While false, `Sampler` and `MaxOutputTokens` are
  ignored and conversations run with engine defaults.
- **NeuTTS on GPU is unreliable.** The backbone `q8` bundle typically fails on the WebGPU
  delegate; use the `fp16` bundle for GPU, or `q8` on CPU. The NeuCodec decoder currently
  fails on GPU outright — keep it on CPU.
- **NeuTTS sampler parameters are placeholders.** `Temperature` / `TopK` / `RandomSeed` on
  `FInoNeuTTSOptions` are informational until an upstream LiteRT-LM regression around
  `set_sampler_params` is fixed.
- **One conversation per engine.** LiteRT-LM sessions on a shared engine share a single
  executor and KV cache, so creating a conversation shuts down any prior live one.
- **Async argument order differs between backends** — LiteRtLm takes
  `(Config, OnProgress, OnLoaded)` while NeuTTS takes `(Config, OnLoaded, OnProgress)`. Watch
  the order until this is unified.

---

## Documentation

- **[`CLAUDE.md`](CLAUDE.md)** — full architecture reference: module boundaries, threading
  model, prompt formats, streaming pipeline internals, and the reasoning behind the
  non-obvious defaults. Read this before modifying the plugin.
- **[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)** — per-path licensing, including the
  NeuTTS commercial-use threshold. Read this before shipping commercially.

This README and `CLAUDE.md` are the only reference material, and both track the shipped code.
Earlier per-subsystem notes under `docs/` were removed in favour of that — they documented a
pre-refactor API (including an audio component that no longer exists) and had become
misleading.

---

## License

Licensed under the [Apache License 2.0](LICENSE). Copyright 2026 Inoland.

### Third-party components

The Apache-2.0 grant above covers **`Source/`, `Content/` and `Resources/`, except where noted
below**. Third-party components keep their own licenses — see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the full per-path breakdown.

| Component | License | Notes |
|---|---|---|
| [LiteRT / LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM) | Apache-2.0 | Integrated, not vendored — supplied by the InoLiteRT plugin |
| [NeuTTS Nano / NeuCodec](https://github.com/neuphonic/neutts) | **NeuTTS Open License v1.0** | ⚠️ **Vendored** under `DepricatedModules/NeuTTS/vendor/`, and the shipped voice assets derive from it. **Not** an OSI-style license — see below |
| [eSpeak NG](https://github.com/espeak-ng/espeak-ng) | GPL-3.0 | Via InoSpeakNG. Dynamic on Win64/Android, **static on iOS and macOS** — see below |
| [RuntimeAudioImporter](https://github.com/gtreshchev/RuntimeAudioImporter) | MIT | Audio sink |

> **⚠️ NeuTTS is not open source.** The **NeuTTS Open License v1.0** permits redistribution,
> but conditions *all* commercial use on your legal entity earning **under $5,000,000 USD in
> annual revenue** (§5; the cap is waived for 501(c)(3)-equivalent non-profits doing
> non-commercial research). Above that threshold you need a paid license from Neuphonic —
> for the vendored code, for the model weights, **and for the voice assets in
> `Content/NeuTTS/Voices/`**, which are derived from Neuphonic's reference voices. The license
> also terminates automatically on any breach. Everything else in this plugin (LiteRT-LM chat,
> ElevenLabs, the character helpers) is unaffected — delete `Source/InoNeuTTS/`,
> `Source/InoNeuTTSEditor/`, `Content/NeuTTS/` and `DepricatedModules/NeuTTS/` and the
> restriction goes with it.

> **⚠️ eSpeak NG is GPL-3.0, and on iOS and macOS it is linked statically.** It reaches
> InoAgents through the sibling InoSpeakNG plugin, which links eSpeak NG dynamically on Win64
> (DLL) and Android (`.so`) but **statically on iOS and macOS** (`libespeak-ng.a` is linked
> directly into the shipped executable). InoSpeakNG is therefore itself GPL-3.0.
>
> So the "it's only dynamically linked" argument is **not available on iOS or macOS**, and it is
> contested even where it does apply. If you ship a build that includes eSpeak NG — especially a
> closed-source commercial one, or anything on a console or locked-down store where GPL-3.0's
> anti-tivoization terms bite — review your obligations properly before you do.
>
> **NeuTTS is the only capability that needs eSpeak NG.** LiteRT-LM chat, ElevenLabs cloud TTS,
> and every animation / audio / camera helper work without it, and InoAgents keeps its
> Apache-2.0 posture as long as you don't ship the NeuTTS backend. See
> [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the delete-list, and
> [ino-espeak-ng-ue](https://github.com/nobandegani/ino-espeak-ng-ue#licensing) for the full
> per-platform breakdown.

Model weights are **not** redistributed here. They download from their upstream hosts at
runtime and remain subject to their own licenses — including Google's Gemma Terms of Use.
