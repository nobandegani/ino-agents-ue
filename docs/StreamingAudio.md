# Streaming audio playback

A standalone `UAudioComponent` subclass for playing audio bytes fed in at runtime. Takes raw PCM (int16 or float32) or MP3, plays it through UE's normal audio pipeline with full volume / pitch / attenuation / spatialization / source-effect-chain / sound-class / concurrency support. **Not coupled to ElevenLabs or LiteRT-LM** — you can use it to play any audio bytes from any source (disk, HTTP, DSP pipeline, synthesis, ...). See the top-level [`README.md`](../README.md) for the plugin's overview.

## Table of contents

- [What you can do with it](#what-you-can-do-with-it)
- [`UInoAgentsStreamingAudioComponent` — `UAudioComponent` subclass](#uinoagentsstreamingaudiocomponent--uaudiocomponent-subclass)
- [Format, sample rate, and channels — the three axes](#format-sample-rate-and-channels--the-three-axes)
- [Blueprint usage](#blueprint-usage)
- [C++ usage](#c-usage)
- [ElevenLabs → audio component wiring](#elevenlabs--audio-component-wiring)
- [Smoke tests](#smoke-tests)
- [Known limitations](#known-limitations)

---

## What you can do with it

- **Play raw audio bytes at runtime** through a `UAudioComponent` subclass — drop it onto any actor via Add Component, hand it a byte buffer, hear sound.
- **Multiple formats:** PCM 16-bit signed (the universal default), PCM 32-bit float (for DSP pipeline outputs), and MP3 (decoded on the fly via a bundled minimp3 single-header library).
- **True chunked streaming** — `FeedAudioBytes` is callable many times as chunks arrive; playback starts as soon as the first frame decodes, not after the whole buffer is assembled.
- **Every UAudioComponent feature inherited for free:** volume multiplier, pitch multiplier, attenuation settings, 3D spatialization, source effect chain, sound class, concurrency, Play/Stop/Pause/FadeIn/FadeOut.
- **Zero coupling to ElevenLabs or LiteRT-LM.** A game that only uses this component to play downloaded MP3s from an HTTP server — no AI involved — is a fully supported use case.

Blueprint category: **`InoAgents|Audio`**. Native classes live under `Source/InoAgents/{Public,Private}/Audio/`. The MP3 decoder is the `minimp3` single-header library (CC0-licensed, ~1900 lines, vendored into `Private/Audio/ThirdParty/` and included from exactly one TU).

---

## `UInoAgentsStreamingAudioComponent` — `UAudioComponent` subclass

Drop it onto an actor via **Add Component → Streaming Audio**. Because it inherits from `UAudioComponent`, every standard audio UPROPERTY is already in the details panel:

- **Volume Multiplier** / **Pitch Multiplier**
- **Attenuation Settings** (3D falloff, distance-based volume)
- **Source Effect Chain** (low-pass filter, reverb send, custom source effects)
- **Sound Class** / **Concurrency Set**
- **bAllowSpatialization** (spatial 3D vs 2D)
- **bOverrideAttenuation** and friends

On top of that, the subclass adds byte-feeding methods and three multicast delegates:

| Method | Purpose |
|---|---|
| `SetPcmFormat(SampleRateHz, NumChannels)` | Configure the sample rate (8000–192000) and channel count (1 or 2) for the NEXT PCM stream. Must be called BEFORE the first `FeedAudioBytes`. Ignored for MP3 streams (auto-detected from frame header). Defaults: 44100 Hz mono. |
| `FeedAudioBytes(Bytes, Format)` | Append bytes to the in-flight stream. Callable many times as chunks arrive. The first call implicitly starts the stream, configures the procedural wave, and calls `Play` so audio starts playing as soon as enough bytes are queued. |
| `FinalizeStream()` | Mark the current stream as complete. After this call, the component fires `OnFinished` when the queued audio fully drains. Already-queued bytes keep playing to completion. |
| `PlayAudio(Bytes, Format)` | One-shot convenience: `FeedAudioBytes` + `FinalizeStream` in a single call. Use when you already have the entire buffer in hand. |
| `StopAndReset()` | Abort any in-flight stream, flush the queue, reset the decoder state. Does NOT fire `OnFinished` (that's reserved for the "drained cleanly after Finalize" path). |

| Delegate | Fires |
|---|---|
| `OnReadyToPlay` | Once per stream, as soon as the first bytes are queued (immediately for PCM, after the first MP3 frame decodes for MP3). Good place to trigger a "speaker speaking" UI indicator. |
| `OnFinished` | Once per stream, after `FinalizeStream` AND the queue fully drains. Triggers a tick-based polling path (cheap, one integer load per frame while active, auto-disables when idle). |
| `OnError` | Once per stream on MP3 decode failure or similar. The stream is then considered finished. |

---

## Format, sample rate, and channels — the three axes

This is where the API is most likely to confuse a first-time user, so read this once and it'll click.

Audio byte streams have **three independent properties** you need to tell the component about:

| Axis | Meaning | How to set |
|---|---|---|
| **Format / bit depth** | How each audio sample is encoded in the byte stream. Answers "how many bytes per sample, and what do they mean?" | The `Format` parameter on `FeedAudioBytes` / `PlayAudio` — a value from the `EInoAgentsAudioFormat` enum. |
| **Sample rate** | How many samples per second. Answers "how fast should playback be?" Typical values: 16000, 22050, 24000, 44100, 48000 Hz. | `SetPcmFormat(SampleRateHz, NumChannels)` — PCM only. MP3 auto-detects. |
| **Channel count** | Mono (1) or stereo (2). Answers "how are samples interleaved?" | `SetPcmFormat(SampleRateHz, NumChannels)` — PCM only. MP3 auto-detects. |

**Important:** `Pcm 16 kHz` and `Pcm 44 kHz` are NOT different formats — they're the same format (`PcmInt16`) played back at different sample rates. You pick the format once in the enum; you pick the sample rate separately via `SetPcmFormat`.

**The enum values:**

| Value | Meaning | Bytes per sample per channel | When to use |
|---|---|---|---|
| `PcmInt16` | Signed 16-bit integer PCM, little-endian. The universal standard for runtime audio bytes. Native input format for `USoundWaveProcedural` — no conversion happens, bytes queue straight through. | 2 | 99% of use cases. ElevenLabs PCM output, Whisper input, game audio bytes, ... |
| `PcmFloat32` | IEEE 754 single-precision float PCM. Samples are expected in the `[-1.0, +1.0]` range (out-of-range values are clamped). Converted to int16 internally before queueing. | 4 | DSP pipeline outputs that hand you float buffers, some ML/TTS libraries, VST-style audio plugins. |
| `Mp3` | MPEG-1 / 2 / 2.5 Layer III compressed audio. Sample rate and channel count are auto-detected from the first MP3 frame header — `SetPcmFormat` is ignored. Decoded on the fly by the bundled `minimp3` library. | variable | ElevenLabs default output, downloaded music files, any other MP3 source. |

**What about int8, int24, int32?**
- **int8** is obsolete (early 90s tech). If you somehow have it, upsample to int16 on your side before calling.
- **int24** is pro-audio-only (recording studios). Fiddly 3-byte packing. Not supported.
- **int32** is pro-audio-only. Not supported.
- **float64** is scientific computation only. Downcast to float32 and use `PcmFloat32`.

If you genuinely need one of these, convert to `PcmInt16` or `PcmFloat32` in your own code before calling `FeedAudioBytes`. The conversion is trivial (a few lines) and 99.9% of runtime audio sources don't need it.

**Sample rate guidance:** Match the source. If you're feeding ElevenLabs PCM 44.1 kHz bytes, call `SetPcmFormat(44100, 1)` or `(44100, 2)` depending on whether it's mono or stereo. If you're feeding 16 kHz STT audio (Whisper's native rate), call `SetPcmFormat(16000, 1)`. If you're feeding 48 kHz mixer output, call `SetPcmFormat(48000, 2)`. UE's audio mixer handles any-rate-to-any-rate conversion internally so you don't need to resample.

---

## Blueprint usage

Typical flow for playing a PCM byte array you already have in hand:

1. Drop a **Streaming Audio** component onto your actor (Add Component → Streaming Audio).
2. Adjust inherited UAudioComponent properties as desired (volume, pitch, attenuation...).
3. When ready to play: drag off the component, call **Set Pcm Format** with your sample rate and channel count, then **Play Audio** with your `TArray<byte>` buffer and `Format = PcmInt16`.
4. Optionally bind **On Ready To Play**, **On Finished**, and **On Error** events on the component to trigger UI updates.

For chunked / streaming input (e.g. from an HTTP download):

1. Drop the component onto your actor.
2. As each chunk of bytes arrives, call **Feed Audio Bytes** with `Format = PcmInt16` (or `Mp3`) — the first call starts playback automatically.
3. When the final chunk has been fed, call **Finalize Stream**.
4. `OnFinished` fires once the queue fully drains.

---

## C++ usage

```cpp
#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "Audio/InoAgentsAudioTypes.h"

// Assume MyActor is a spawned AActor; ensure it has a component.
UInoAgentsStreamingAudioComponent* Audio =
    MyActor->FindComponentByClass<UInoAgentsStreamingAudioComponent>();

// One-shot: you have a full buffer.
Audio->SetPcmFormat(44100, /*NumChannels=*/1);
Audio->PlayAudio(MyPcmBytes, EInoAgentsAudioFormat::PcmInt16);

// Chunked: you're receiving bytes as they stream in.
Audio->SetPcmFormat(44100, 1);
for (const TArray<uint8>& Chunk : IncomingChunks)
{
    Audio->FeedAudioBytes(Chunk, EInoAgentsAudioFormat::PcmInt16);
}
Audio->FinalizeStream();

// MP3 variant: no SetPcmFormat needed (auto-detected from frame header).
Audio->PlayAudio(MyMp3Bytes, EInoAgentsAudioFormat::Mp3);

// Float32 DSP output variant:
// Samples in [-1.0, +1.0], 4 bytes per sample, mono.
Audio->SetPcmFormat(48000, 1);
Audio->PlayAudio(MyFloat32Bytes, EInoAgentsAudioFormat::PcmFloat32);
```

Bind delegates the same way you would for any other `BlueprintAssignable`:

```cpp
Audio->OnReadyToPlay.AddDynamic(this, &UMyClass::HandleAudioReady);
Audio->OnFinished.AddDynamic(this, &UMyClass::HandleAudioFinished);
Audio->OnError.AddDynamic(this, &UMyClass::HandleAudioError);
```

Handler signatures:

```cpp
UFUNCTION() void HandleAudioReady();
UFUNCTION() void HandleAudioFinished();
UFUNCTION() void HandleAudioError(FString ErrorMessage);
```

---

## ElevenLabs → audio component wiring

This is the headline use case: TTS dialogue audible in-game, live, with zero disk intermediary.

**Blueprint setup:**

1. Drop a **Streaming Audio** component on any actor.
2. In that actor's BP, add the **ElevenLabs Stream Text-to-Dialogue** latent node.
3. Wire the node's **On Audio Chunk** event to **Feed Audio Bytes** on the component (set `Format = Mp3`).
4. Wire **On Complete** to **Finalize Stream**.
5. Wire **On Error** to **On Error** on the component (or your own error handler).
6. Press Play. Audio plays from the actor's 3D position as it streams in from ElevenLabs.

No sample-rate configuration is needed because MP3 carries that metadata in every frame. If you switch ElevenLabs' output format to PCM (via Project Settings → Plugins → InoAgents ElevenLabs → Default Output Format), update the Blueprint wiring to pass `Format = PcmInt16` and call `SetPcmFormat(44100, 1)` (or whichever rate you selected) before the first `OnAudioChunk`.

See [`ElevenLabs.md`](ElevenLabs.md) for the ElevenLabs half of this recipe.

---

## Smoke tests

Three PIE-only console commands under `InoAgents.Audio.*`:

| Command | What it does |
|---|---|
| `InoAgents.Audio.PlayPcmTest` | Generates a 1-second 440 Hz sine wave at 44100 Hz mono int16, plays it via `PlayAudio`, verifies `OnReadyToPlay` and `OnFinished` both fire. Exercises the PCM path end-to-end with no decoder involvement. |
| `InoAgents.Audio.PlayMp3Test [path]` | Loads an MP3 file from disk and plays it in one shot. Defaults to `Saved/InoAgents/ElevenLabs/test.mp3` so it plays whatever the ElevenLabs smoke test most recently generated. Optional path argument to override. |
| `InoAgents.Audio.PlayMp3ChunkedTest [path]` | Same MP3 file, but sliced into 4 KB chunks fed one per 50 ms via `FTSTicker` + `FeedAudioBytes`, then `FinalizeStream()`. Proves playback starts before all chunks are delivered. |

Expected flow for the full smoke test run:

```
# 1. Run the ElevenLabs smoke test to produce a test.mp3:
InoAgents.ElevenLabs.DialogueStreamTest

# 2. Sanity-check the PCM path (synthesised, no file needed):
InoAgents.Audio.PlayPcmTest
#    -> 1 second of 440 Hz tone audible from the editor listener

# 3. Play back what ElevenLabs just generated:
InoAgents.Audio.PlayMp3Test
#    -> multi-line dialogue audible

# 4. Prove chunked ingestion works:
InoAgents.Audio.PlayMp3ChunkedTest
#    -> same dialogue, but the log shows chunk-by-chunk feed while
#       audio is already playing
```

---

## Known limitations

- **Supports PCM int16, PCM float32, and MP3 only.** Opus, Vorbis, u-law, A-law, and other formats are not decoded. If you need them, convert in your own code first, or open an issue. Obsolete PCM variants (int8, int24, int32) are deliberately not exposed — use int16 or float32 and convert on your side if strictly necessary.
- **MP3 decoding runs on the game thread.** `minimp3` is very fast (one frame decodes well under a millisecond) so this is fine for TTS-sized buffers. Long music streams would benefit from a worker-thread decoder — not yet implemented.
- **Does NOT save `USoundWave` assets.** It plays bytes directly through a `USoundWaveProcedural` that lives only as long as the component. To persist audio, save the bytes to disk yourself from your `OnFinished` handler.
- **Inherited `Sound` UPROPERTY hidden.** The component owns its procedural wave internally. The `Sound` category is hidden via `HideCategories(Sound)` so designers don't accidentally swap in a different `USoundBase` and break the component. Do NOT call `SetSound()` yourself at runtime.
