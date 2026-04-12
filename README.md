# InoAgents

Unreal Engine 5.7 runtime plugin with three integrations that compose into a complete AI agent pipeline:

1. **LiteRT-LM / Google Gemma 4** — on-device tool-calling LLM agents running directly inside the game process. No network, no cloud, no subscription, no Python runtime, no second binary to ship. Models auto-download on first use from Hugging Face.
2. **ElevenLabs cloud voice API** — standalone HTTP client for ElevenLabs' audio endpoints, exposed as native Blueprint latent nodes and C++ async actions. Phase 1 ships Text-to-Dialogue streaming with expressive `[emotion]` tag support; TTS and STT are on the roadmap.
3. **Streaming audio playback component** — a `UAudioComponent` subclass that plays raw audio bytes (PCM int16, PCM float32, or MP3) fed in at runtime. Inherits every standard UAudioComponent feature (volume, pitch, attenuation, spatialization, source effect chain, sound class, concurrency).

All three compose cleanly: the **LiteRT-LM Agent Component** (`UInoAgentsLiteRtLmAgentComponent`) wires them together as a single drop-on-actor scene component. Set a model config + voice ID in the details panel, call `SendMessage`, and the actor speaks with spatialised 3D audio — model loading, conversation management, per-sentence TTS dispatch, ordered audio playback, and emotion tags are all handled internally.

Each integration is also usable independently. The plugin is Blueprint-first: every surface is callable or bindable from Blueprint without writing C++.

**Status:** LiteRT-LM Milestone D + agent component shipped. ElevenLabs phase 1 shipped. Streaming audio component + ordered TTS queue shipped. Model auto-download shipped. All settings unified under one Project Settings page.

---

## Subsystem docs

Each integration has its own doc with full API surface, usage patterns, smoke tests, and known limitations:

- **[`docs/LiteRtLm.md`](docs/LiteRtLm.md)** — on-device Gemma 4 with tool calling. Covers the all-in-one agent component, `ULiteRtLmSubsystem`, `ULiteRtLmConversation`, `FLiteRtLmModelConfig` (struct, not a data asset), the `ILiteRtLmTool` interface, how to write tools in Blueprint or C++, model auto-download, and the smoke tests.
- **[`docs/ElevenLabs.md`](docs/ElevenLabs.md)** — ElevenLabs HTTP client. Covers API key configuration (Project Settings → Plugins → InoAgents → ElevenLabs), `UElevenLabsSubsystem`, `UElevenLabsTextToDialogueStream` latent node, request types, Blueprint + C++ usage, and the smoke tests.
- **[`docs/StreamingAudio.md`](docs/StreamingAudio.md)** — runtime audio byte playback via a `UAudioComponent` subclass. Covers formats (PCM int16 / PCM float32 / MP3), the format-vs-sample-rate-vs-channels distinction, the ordered TTS dialogue queue (`UInoAgentsLiteRtLmDialogueQueue`), Blueprint + C++ usage, and the smoke tests.

---

## Quick start — the easy path

### 1. Enable the plugin

Drop `Plugins/InoAgents/` into your project's `Plugins/` directory (or use `InoAgentDemo` as a starting point). Add `InoAgents` to your `.uproject`'s Plugins array, regenerate project files.

### 2. Build `LiteRtLm.dll` (one-time)

See [`docs/LiteRtLm.md → Requirements`](docs/LiteRtLm.md#requirements) for the Bazel build steps. ~15-40 minutes cold build.

### 3. Drop the agent component on an actor

- Add Component → **LiteRT-LM Agent**
- In the details panel: set **Model Config → Model File Name** (e.g. `gemma-4-E4B-it.litertlm`), set **Voice Id** to your ElevenLabs voice, set **Model Config → System Message** for the agent's personality.
- The model auto-downloads from Hugging Face on first Play if it isn't cached locally. Progress fires via `OnDownloadProgress`.
- Call `Send Message ("Hello")` from any trigger — the actor speaks.

### 4. Configure API keys + model URLs

**Project Settings → Plugins → InoAgents** has two sections:

| Section | What to configure |
|---|---|
| **ElevenLabs** | API Key (get one at https://elevenlabs.io/app/settings/api-keys), Base URL, default model/format |
| **LiteRT-LM → Models** | Array of `{DisplayName, ModelFileName, DownloadUrl}` — default entries point at Hugging Face for Gemma 4 E2B and E4B |

---

## Requirements

| Thing | Why |
|---|---|
| **Unreal Engine 5.7** | Minimum tested version. |
| **Windows (Win64, MSVC)** | Only platform currently supported. Android, iOS, Linux, macOS on the roadmap. |
| **A built `LiteRtLm.dll`** | See [`docs/LiteRtLm.md`](docs/LiteRtLm.md). Not needed if you only use ElevenLabs or the audio component. |

Model files (~2.5–5 GB) are auto-downloaded to `PersistentDownloadDir/InoAgents/Models/` on first use. No manual download step needed unless you want to pre-cache.

---

## Further reading

- **[`CLAUDE.md`](CLAUDE.md)** — architecture, threading model, Bazel build notes, Windows gotchas, tool-calling flow. Read this if you are modifying the plugin itself.
- **LiteRT-LM upstream** — https://github.com/google-ai-edge/LiteRT-LM
- **Gemma 4 edge models on Hugging Face** — https://huggingface.co/litert-community
- **ElevenLabs API reference** — https://elevenlabs.io/docs/api-reference
