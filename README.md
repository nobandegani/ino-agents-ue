# InoAgents

Unreal Engine 5.7 runtime plugin with three independent integrations:

1. **LiteRT-LM / Google Gemma 4** — on-device tool-calling LLM agents running directly inside the game process. No network, no cloud, no subscription, no Python runtime, no second binary to ship.
2. **ElevenLabs cloud voice API** — standalone HTTP client for ElevenLabs' audio endpoints, exposed as native Blueprint latent nodes and C++ async actions. Phase 1 ships Text-to-Dialogue streaming; TTS and STT are on the roadmap.
3. **Streaming audio playback component** — a `UAudioComponent` subclass that plays raw audio bytes (PCM int16, PCM float32, or MP3) fed in at runtime. Inherits every standard UAudioComponent feature (volume, pitch, attenuation, spatialization, source effect chain, sound class, concurrency).

All three integrations are **fully decoupled** — use any combination, or none. The plugin is Blueprint-first: every surface a gameplay programmer or designer needs is callable or bindable from Blueprint without writing C++.

**Status:** LiteRT-LM Milestone D shipped. ElevenLabs phase 1 shipped. Streaming audio component shipped.

---

## Subsystem docs

Each integration has its own doc with full API surface, usage patterns, smoke tests, and known limitations. Start here once the plugin is enabled in your project:

- **[`docs/LiteRtLm.md`](docs/LiteRtLm.md)** — on-device Gemma 4 with tool calling. Covers `ULiteRtLmSubsystem`, `ULiteRtLmConversation`, `ULiteRtLmModelConfig`, the `ILiteRtLmTool` interface, how to write tools in Blueprint or C++, and the five `InoAgents.LiteRtLm.*` smoke tests.
- **[`docs/ElevenLabs.md`](docs/ElevenLabs.md)** — ElevenLabs HTTP client. Covers API key configuration, `UElevenLabsSubsystem`, `UElevenLabsTextToDialogueStream` latent node, request types, Blueprint + C++ usage, and the two `InoAgents.ElevenLabs.*` smoke tests.
- **[`docs/StreamingAudio.md`](docs/StreamingAudio.md)** — runtime audio byte playback via a `UAudioComponent` subclass. Covers formats (PCM int16 / PCM float32 / MP3), the format-vs-sample-rate-vs-channels distinction, Blueprint + C++ usage, ElevenLabs-to-audio wiring recipe, and the three `InoAgents.Audio.*` smoke tests.

---

## Requirements

| Thing | Why |
|---|---|
| **Unreal Engine 5.7** | Minimum tested version. Earlier UE versions have incompatible subsystem / delegate APIs. |
| **Windows (Win64, MSVC)** | The only platform currently supported. Android, iOS, Linux, and macOS are on the roadmap. |
| **DirectX 12** | Only required for LiteRT-LM's (future) GPU path. CPU inference works everywhere. Ignore if you're only using ElevenLabs or the audio component. |

LiteRT-LM additionally needs a built `LiteRtLm.dll` and a downloaded Gemma 4 model file — see [`docs/LiteRtLm.md → Requirements`](docs/LiteRtLm.md#requirements). ElevenLabs needs an API key — see [`docs/ElevenLabs.md → Configure the API key`](docs/ElevenLabs.md#configure-the-api-key). The streaming audio component has no extra requirements beyond the plugin itself.

---

## Quick start

### 1. Enable the plugin

Drop `Plugins/InoAgents/` into your project's `Plugins/` directory (or copy this whole `InoAgentDemo` project and use it as a starting point — it is the plugin's demo host). Open `YourProject.uproject`, add `InoAgents` to the `Plugins` array with `"Enabled": true`, and regenerate Visual Studio project files.

### 2. Pick the integrations you need

| You want to... | Read |
|---|---|
| Run an on-device LLM and have it call your tools | [`docs/LiteRtLm.md`](docs/LiteRtLm.md) |
| Generate voice audio via ElevenLabs | [`docs/ElevenLabs.md`](docs/ElevenLabs.md) |
| Play runtime audio bytes (PCM or MP3) through UE's audio pipeline | [`docs/StreamingAudio.md`](docs/StreamingAudio.md) |
| Synthesise TTS and hear it in-game | [`docs/ElevenLabs.md`](docs/ElevenLabs.md) + [`docs/StreamingAudio.md`](docs/StreamingAudio.md) (the two compose cleanly via a three-node Blueprint wiring — see the audio doc's ElevenLabs recipe section) |

Each doc has its own **Quick start** section with a smoke test you can run from the Output Log command input to prove the integration works before writing any of your own Blueprint or C++.

---

## Further reading

- **[`CLAUDE.md`](CLAUDE.md)** — architecture, threading model, Bazel build notes, Windows gotchas, model file distribution, tool-calling flow. Read this if you are modifying the plugin itself.
- **[`docs/superpowers/specs/2026-04-11-milestone-d-litert-lm-ue-api-design.md`](docs/superpowers/specs/2026-04-11-milestone-d-litert-lm-ue-api-design.md)** — the Milestone D design record for the LiteRT-LM UE API.
- **[`docs/superpowers/plans/2026-04-11-milestone-d-litert-lm-ue-api.md`](docs/superpowers/plans/2026-04-11-milestone-d-litert-lm-ue-api.md)** — the Milestone D implementation plan.
- **LiteRT-LM upstream** — https://github.com/google-ai-edge/LiteRT-LM
- **Gemma 4 edge models on Hugging Face** — https://huggingface.co/litert-community
- **ElevenLabs API reference** — https://elevenlabs.io/docs/api-reference
