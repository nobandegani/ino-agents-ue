# ElevenLabs cloud voice integration

A standalone HTTP client for ElevenLabs' cloud audio API, completely independent of the LiteRT-LM subsystem and the streaming audio component — you can use this without ever touching either of them. See the top-level [`README.md`](../README.md) for the plugin's overview.

## Table of contents

- [What you can do with it](#what-you-can-do-with-it)
- [Phase status](#phase-status)
- [Configure the API key](#configure-the-api-key)
- [API surface](#api-surface)
  - [`UElevenLabsSettings`](#uelevenlabssettings--project-settings)
  - [`UElevenLabsSubsystem`](#uelevenlabssubsystem--game-instance-subsystem)
  - [`UElevenLabsTextToDialogueStream`](#uelevenlabstexttodialoguestream--latent-async-action)
  - [Request types](#request-types)
- [Blueprint usage](#blueprint-usage)
- [C++ usage](#c-usage)
- [Smoke tests](#smoke-tests)
- [Playing the result](#playing-the-result)
- [Known limitations](#known-limitations)

---

## What you can do with it

- **Synthesise multi-speaker dialogue** via a single Blueprint latent node. Pass an array of `{voice_id, text}` pairs; receive audio bytes back as they stream in from ElevenLabs.
- **Stream audio chunks in real time** via an `OnAudioChunk` delegate that fires as each HTTP progress tick lands — useful for low-latency playback, progress bars, or piping into your own audio buffer.
- **Cancel in-flight requests** individually or all-at-once via a single subsystem call. PIE-end teardown is automatic.
- **Store the API key in Project Settings** with a `PasswordField`-masked developer setting. Per-call override parameter lets you fetch keys from your own secret store at runtime.

---

## Phase status

| Phase | Endpoint | Status |
|---|---|---|
| **1** | `POST /v1/text-to-dialogue/stream` — multi-speaker dialogue, streamed | **shipped** |
| 2 | `POST /v1/text-to-speech/{voice_id}/stream` — single-voice TTS, streamed | roadmap |
| 3 | `POST /v1/speech-to-text` — transcription | roadmap |

Phases 2 and 3 will reuse the same `UElevenLabsSettings` / `UElevenLabsSubsystem` scaffolding documented below. Blueprint category: **`InoAgents|ElevenLabs`**. Native classes live under `Source/InoAgents/{Public,Private}/ElevenLabs/`.

---

## Configure the API key

1. Get a key at **https://elevenlabs.io/app/settings/api-keys**.
2. In the Unreal Editor, open **Edit → Project Settings → Plugins → InoAgents ElevenLabs**.
3. Paste the key into the **API Key** field (it's masked as a password) and close the dialog. The value is persisted to `Config/DefaultGame.ini` under `[/Script/InoAgents.ElevenLabsSettings]`.
4. If you edit the key mid-PIE-session, either restart PIE or run `InoAgents.ElevenLabs.ReloadSettings` in the console to re-cache it into the subsystem.

**Security note.** The key is stored **plaintext** in the ini. Do not commit `DefaultGame.ini` to a public repo once a real key is pasted in — treat it like any other dev secret. For shipping builds, pass a runtime-fetched key to the async action's `ApiKeyOverride` parameter instead of baking it into the ini.

---

## API surface

### Settings — Project Settings → Plugins → InoAgents → ElevenLabs

ElevenLabs settings live on the unified `UInoAgentsSettings` class (shared with LiteRT-LM settings on the same page). Fields:

| Field | Type | Default | Purpose |
|---|---|---|---|
| `ElevenLabsApiKey` | `FString` (password-masked) | *(empty)* | xi-api-key for ElevenLabs. |
| `ElevenLabsBaseUrl` | `FString` | *(empty → `https://api.elevenlabs.io`)* | Override for regional routing. Trailing slashes are stripped automatically. |
| `ElevenLabsDefaultModelId` | `FString` | `eleven_v3` | Used when a per-call request leaves `ModelId` empty. |
| `ElevenLabsDefaultOutputFormat` | enum | `Mp3_44100_128` | Used when no explicit format is passed. |

### `UElevenLabsSubsystem` — game instance subsystem

Shared state and lifecycle anchor. One instance per game instance; access via `Get Game Instance Subsystem (UElevenLabsSubsystem)` in Blueprint or `GetGameInstance()->GetSubsystem<UElevenLabsSubsystem>()` in C++.

The subsystem caches settings at `Initialize()`, holds a UPROPERTY `TSet` of every in-flight async action (so GC can't eat them mid-request), and calls `CancelAll()` from `Deinitialize()` so HTTP responses arriving after PIE end can't land on freed UObjects. You rarely need to touch it directly — the async actions below wire themselves up automatically — but it's useful for global cancellation and for reloading settings.

| Function | Kind | Purpose |
|---|---|---|
| `ReloadSettings()` | `BlueprintCallable` | Re-reads `UInoAgentsSettings` into the cached fields without restarting PIE. Also available as the `InoAgents.ElevenLabs.ReloadSettings` console command. |
| `CancelAll()` | `BlueprintCallable` | Aborts every in-flight ElevenLabs request tracked by the subsystem. Each action receives `OnError("cancelled")` before being released. Called automatically at PIE end. |
| `GetApiKey()` / `GetBaseUrl()` / `GetDefaultModelId()` / `GetDefaultOutputFormat()` | `(C++ only)` | Cached accessors used internally by the async actions. You usually don't need to call these — bind them via the async action's per-call override parameter instead. |

### `UElevenLabsTextToDialogueStream` — latent async action

A `UBlueprintAsyncActionBase` subclass. This is the Blueprint-friendly front door for the dialogue endpoint: drag it as a single latent node with three output exec pins.

Static factory:

```cpp
UElevenLabsTextToDialogueStream::StreamTextToDialogue(
    UObject*                          WorldContextObject,
    const FElevenLabsDialogueRequest& Request,
    FString                           ApiKeyOverride);
```

Multicast delegates (all `BlueprintAssignable`, all fire on the game thread):

| Delegate | Params | Fires |
|---|---|---|
| `OnAudioChunk` | `(const TArray<uint8>& AudioBytes, int64 TotalBytesReceived)` | Zero or more times per request, as HTTP progress ticks land. `AudioBytes` is the **new bytes only** (not the full accumulated buffer), ready to append to your own playback buffer. |
| `OnComplete` | `(const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat OutputFormat)` | Exactly once on success (2xx). `FullAudioBytes` is the complete response, byte-equal to concatenating every `OnAudioChunk` payload. |
| `OnError` | `(FString ErrorMessage)` | Exactly once on any failure path: missing API key, validation error, HTTP non-2xx, network failure, or explicit `CancelStream()`. Message is human-readable and safe to surface in UI. |

Additional method:

| Function | Kind | Purpose |
|---|---|---|
| `CancelStream()` | `BlueprintCallable` | Aborts the in-flight request. Fires `OnError("cancelled")` then destroys the action. Safe to call from any handler. No-op if the action has already finished. |

### Request types

`FElevenLabsDialogueRequest`:

| Field | Type | Default | Notes |
|---|---|---|---|
| `Inputs` | `TArray<FElevenLabsDialogueInput>` | *(empty)* | 1–10 unique voice IDs. Each input is a `{Text, VoiceId}` pair. |
| `ModelId` | `FString` | *(empty → subsystem default)* | e.g. `"eleven_v3"`. |
| `OutputFormat` | `EElevenLabsOutputFormat` | `Mp3_44100_128` | MP3 / PCM / u-law codec + sample rate. |
| `LanguageCode` | `FString` | *(empty → auto-detect)* | ISO 639-1 code. |
| `Stability` | `float` | `0.5` | Voice settings stability, clamped 0..1. |
| `Seed` | `int64` | `-1` (omit) | Deterministic sampling seed, 0..4294967295. Negative values omit the field. |
| `ApplyTextNormalization` | enum | `Auto` | `Auto` / `On` / `Off`. |

---

## Blueprint usage

Typical flow (describing nodes, not screenshots):

1. On **BeginPlay** (or button click) build an `FElevenLabsDialogueRequest`: drag off a Make struct node, populate the `Inputs` array with two-or-more `{Text, VoiceId}` entries via Make `FElevenLabsDialogueInput` nodes.
2. Drag out the **`ElevenLabs Stream Text-to-Dialogue`** node (search for "eleven" in the context menu). Hook the `WorldContextObject` pin to `self`, feed your request struct into `Request`, leave `ApiKeyOverride` empty to use the Project Settings key.
3. The node has three output exec pins that fire as events occur:
   - **`On Audio Chunk`** — fires many times. Use `Append Bytes` into a local `TArray<byte>` variable if you want to buffer, or pipe the bytes directly into a `Streaming Audio` component (see [Playing the result](#playing-the-result)).
   - **`On Complete`** — fires once with the full buffer. Save to disk, pipe into a component, or hand to your own audio pipeline.
   - **`On Error`** — fires once on failure. Display the message in UI or log it.
4. Do NOT bind the output pins and then assume the node returns immediately — it IS a latent node, so execution flows out of the event pins as they fire, not the "finished" pin (there is no finished pin; each pin is terminal for its event type).

---

## C++ usage

```cpp
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "ElevenLabs/ElevenLabsTypes.h"

FElevenLabsDialogueRequest Req;
Req.Inputs.Add({ TEXT("Knock knock."),         TEXT("JBFqnCBsd6RMkjVDRZzb") });
Req.Inputs.Add({ TEXT("Who's there?"),         TEXT("Aw4FAjKCGjjNkVhN1Xmq") });
Req.Inputs.Add({ TEXT("A plugin, streaming."), TEXT("JBFqnCBsd6RMkjVDRZzb") });
Req.OutputFormat = EElevenLabsOutputFormat::Mp3_44100_128;

UElevenLabsTextToDialogueStream* Action =
    UElevenLabsTextToDialogueStream::StreamTextToDialogue(
        /*WorldContextObject=*/ this,
        /*Request=*/            Req,
        /*ApiKeyOverride=*/     FString());  // empty = use Project Settings key

Action->OnAudioChunk.AddDynamic(this, &UMyClass::HandleChunk);
Action->OnComplete  .AddDynamic(this, &UMyClass::HandleComplete);
Action->OnError     .AddDynamic(this, &UMyClass::HandleError);
Action->Activate();   // in Blueprint this fires automatically; from C++ we call it
```

Handler signatures must match the delegate's declared parameter-passing convention exactly. In this plugin:

- **`FString`, enums, and POD/primitive types** → pass **by value**. This matches the LiteRT-LM plugin convention.
- **`TArray<T>` and other containers** → pass **`const TArray<T>&`** (by const reference). Declaring a container-returning delegate with by-value `TArray<uint8>` compiles fine but fails at Blueprint-time with "function/event does not match the necessary signature" when a user drags the latent node into a graph, because the Blueprint event-handler generator emits `const&` for containers unconditionally.

```cpp
UFUNCTION() void HandleChunk   (const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);
UFUNCTION() void HandleComplete(const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat Format);
UFUNCTION() void HandleError   (FString ErrorMessage);
```

The `Action` object's lifetime is owned by `UElevenLabsSubsystem` — you do NOT need to `AddToRoot` it or store it in a `UPROPERTY` on the caller. The subsystem drops its reference once a terminal delegate fires, and GC collects the action on the next pass.

---

## Smoke tests

Two PIE-only console commands under the `InoAgents.ElevenLabs.*` namespace:

| Command | What it does |
|---|---|
| `InoAgents.ElevenLabs.DialogueStreamTest` | Dispatches a fixed 3-line dialogue using two ElevenLabs sample voices, logs each chunk's size as it arrives, and saves the resulting audio to `Saved/InoAgents/ElevenLabs/test.mp3` (or `.pcm` / `.ulaw` depending on the default output format). Logs **PASS** and the absolute output path on success. |
| `InoAgents.ElevenLabs.ReloadSettings` | Re-reads `UElevenLabsSettings` into the subsystem's cached fields. Run this after editing the API key in Project Settings if you want to pick up the change without restarting PIE. |

Expected log output for a successful run:

```
LogInoAgents: UElevenLabsTextToDialogueStream: POST https://api.elevenlabs.io/v1/text-to-dialogue/stream?output_format=mp3_44100_128 (3 inputs, N-byte body)
LogInoAgents: DialogueStreamTest: chunk   1 (+0.412 s) — 8192 bytes (total 8192)
LogInoAgents: DialogueStreamTest: chunk   2 (+0.503 s) — 16384 bytes (total 24576)
...
LogInoAgents: DialogueStreamTest: COMPLETE — N chunks, M bytes, X.XX s
LogInoAgents: DialogueStreamTest: PASS — saved to <abs path>\Saved\InoAgents\ElevenLabs\test.mp3
```

Open the saved file in VLC / Windows Media Player — you should hear the three-line dialogue spoken by two different voices.

Failure modes:

- **`API key is empty; set it in Project Settings -> Plugins -> InoAgents ElevenLabs`** → Either you haven't set the key, or you set it mid-PIE-session but didn't run `InoAgents.ElevenLabs.ReloadSettings`.
- **`HTTP 401: ...`** → Key is invalid or expired.
- **`HTTP 402: ...`** → Key is valid but your account doesn't have access to the requested voices. The smoke test uses the two sample voices from the ElevenLabs docs (`JBFqnCBsd6RMkjVDRZzb` and `Aw4FAjKCGjjNkVhN1Xmq`); swap them out for voices from your own library if you hit this.
- **`HTTP 422: ...`** → Validation error. The message usually identifies the field; check it against `FElevenLabsDialogueRequest`.
- **`HTTP 404: Not Found`** → Likely a malformed URL. Check for a stray trailing slash in the Base URL setting (the plugin strips trailing slashes automatically on reload, but an older plugin version may not have).

---

## Playing the result

The async action hands you **raw audio bytes**, not a `USoundWave`. To actually *hear* them inside the game, the plugin ships a companion `UInoAgentsStreamingAudioComponent` that accepts these bytes directly. See [`StreamingAudio.md → ElevenLabs → audio component wiring`](StreamingAudio.md#elevenlabs--audio-component-wiring) for the recipe (it's a three-node Blueprint: wire `OnAudioChunk` → `FeedAudioBytes`, `OnComplete` → `FinalizeStream`, done).

You can also buffer the bytes yourself and do whatever you want with them — save to disk, ship to a different audio backend, analyse them — the async action is agnostic.

---

## Known limitations

- **Phase 1 only.** Only `/v1/text-to-dialogue/stream` is wired up today. Single-voice Text-to-Speech (`/v1/text-to-speech/{voice_id}/stream`) and Speech-to-Text (`/v1/speech-to-text`) are planned for phases 2 and 3 respectively.
- **Raw bytes, not `USoundWave`.** The plugin hands you the full `TArray<uint8>` in `OnComplete` (and incrementally in `OnAudioChunk`). Decoding MP3 / PCM / u-law into something UE's audio engine can play is the caller's responsibility — use `UInoAgentsStreamingAudioComponent` for the easy path (see [Playing the result](#playing-the-result)).
- **API key plaintext in `DefaultGame.ini`.** Do not commit the ini with a real key. For shipping builds, pass a runtime-fetched key via the async action's `ApiKeyOverride` parameter instead.
- **No built-in rate-limit / retry handling.** 4xx / 5xx responses fire `OnError` with the server's message; the caller decides whether to retry and when. No exponential backoff, no queueing.
