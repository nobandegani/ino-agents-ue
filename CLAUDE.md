# CLAUDE.md — InoAgents plugin

This file provides guidance to Claude Code (claude.ai/code) when working inside `Plugins/InoAgents/`. The hosting demo project is documented in `E:/Projects/InoAgentDemo/CLAUDE.md`.

## Purpose

`InoAgents` is an Unreal Engine 5.7 runtime plugin that embeds **Google Gemma 4** on-device, so UE games and tools can run LLM-powered agents inside the game process with no external server and no cloud dependency.

The plugin is named for "agents" deliberately: the goal is not just text generation but **tool-use / function-calling workflows** running natively in UE, driven from Blueprint.

## Runtimes: LiteRT-LM + ONNX Runtime + llama.cpp

The plugin carries **three** on-device ML runtimes, each doing what it's best at:

- **LiteRT-LM** — Google's TFLite-based LLM runtime. Handles Gemma 4 inference (chat, tool calling, streaming). Built and staged by the sibling **`InoLiteRT`** plugin (see `Plugins/InoLiteRT/CLAUDE.md`); InoAgents links against the `LiteRtLm.dll` / `libLiteRtLm.so` it produces and consumes the same C API. See the sections below for how InoAgents uses the runtime.
- **ONNX Runtime** — Microsoft's ONNX inference runtime. Reserved for everything non-LLM: TTS models (Chatterbox Turbo + NeuCodec decoder for NeuTTS Nano), audio codec decoders, future vision / classifier / embedding models. Built and staged by the sibling **`InoOnnx`** plugin (see `Plugins/InoOnnx/CLAUDE.md`); InoAgents links against the `InoOnnxRuntime.dll` / `libInoOnnxRuntime.so` it produces and consumes the `OrtApi` vtable via `InoAgents::Onnx::GetApi()`.
- **llama.cpp** — The canonical on-device runtime for GGUF-format LLMs. Built and staged by the sibling **`InoLlama`** plugin (see `Plugins/InoLlama/CLAUDE.md`); InoAgents links against the `llama.dll` / `libllama.so` graph it produces and consumes the `FLlamaCppApi` vtable via `InoAgents::LlamaCpp::GetApi()`. First and currently only consumer: **NeuTTS Nano TTS**.

Why three runtimes instead of one: each is purpose-built for a specific ecosystem of models. LiteRT-LM is purpose-built for Gemma 4 (KV-cache, chat-template-aware streaming, quantized weights) and has no credible story for running arbitrary ONNX or GGUF models. ONNX Runtime is the industry-standard runtime for ONNX graphs (the only format Chatterbox and NeuCodec ship in) with prebuilts on every platform. llama.cpp is the only production-grade runtime for GGUF-format LLMs (which covers Qwen, Phi, Llama, SmolLM, NeuTTS's Qwen2 backbone, and essentially every non-Gemma open LLM worth running on-device). Each runtime is small enough (~14 MB LiteRT-LM + ~14 MB ORT + ~2-80 MB llama.cpp depending on backends shipped) that carrying all three costs less than forcing one runtime to do all three jobs. All three use the same integration shape (prebuilt binaries, dynamic loading, platform-aware execution providers) so the mental model stays consistent.

### LiteRT-LM

LiteRT-LM is **owned by the sibling `InoLiteRT` plugin** — see
`Plugins/InoLiteRT/CLAUDE.md` for the runtime itself: how it's built,
which SHA is pinned, supported platforms, backend selection, build
scripts, the C API header, and packaging. InoAgents is purely a
**consumer** — we link against `InoLiteRT` via `PublicDependencyModuleNames`
and call the C API directly from `Source/InoAgents/Private/LiteRtLm/`.

Why LiteRT-LM as the LLM backend (vs llama.cpp / ONNX-GenAI / MLX /
MediaPipe): single runtime that covers all our target platforms,
first-class support for Gemma 4 with public Hugging Face model bundles,
**native tool-use / function-calling exposed through the public C API**
(matches the plugin's "agents" mandate, not prompt-engineered on top of
a raw completion API), and a Windows GPU path that's pure D3D12 with no
extra CUDA / Vulkan / OpenCL runtime to ship alongside the game.

LiteRT-LM is **pre-1.0** — InoLiteRT pins a specific submodule SHA and
never tracks `main`. When that pin bumps, re-test the items in the next
section.

### Known LiteRT-LM Gemma 4 runtime limitations

These three Gemma-4-specific behaviours were originally discovered against v0.10.1. The pinned SHA bumped to post-v0.10.2 as part of the InoLiteRT extraction; items 1 and 2 are now re-enabled and pending smoke-test verification on the new SHA.

1. **Session config (sampler params + max output tokens).** Passing a non-null `LiteRtLmSessionConfig*` previously caused `litert_lm_conversation_create` to return NULL for Gemma 4 models, with a knock-on effect that subsequent conversation creations on the same engine produced error 13 on `send_message_stream`. With the move to the post-v0.10.2 SHA the session-config path has been **re-enabled** in `InoLiteRtLmConversation.cpp` — `litert_lm_conversation_config_set_session_config` now passes `FInoLiteRtLmSamplerConfig` and `MaxOutputTokens` through to the engine. **Status: pending smoke-test verification on the new SHA — flip back to `nullptr` if `conversation_create` still returns NULL for Gemma 4.**

2. **Activation data type (F16/I16/I8).** `litert_lm_engine_settings_set_activation_data_type` with non-F32 values previously loaded the engine successfully but `send_message_stream` returned error 13 (`absl::StatusCode::kInternal`) at runtime — corroborated by upstream `engine.cc:332-338` force-overriding activation to F32 for GPU backends and TODO bug `b/433590109` acknowledging FP16 GPU incompatibilities. Default `FInoLiteRtLmModelConfig::ActivationType` is now **F16** (was forced F32). **Status: pending smoke-test verification on the new SHA — change the default back to F32 if the first message returns error 13.** If a user previously loaded a model with one activation setting and gets error 13 after a config change, deleting the XNNPACK cache (next to the model file, or in the custom CacheDir) forces regeneration with the new format.

3. **`extra_context` parameter is ignored by Gemma 4.** The C API's `litert_lm_conversation_send_message_stream` accepts an `extra_context` JSON string. The Rust minijinja runtime injects its top-level keys as Jinja2 template variables. However, the Gemma 4 chat template (embedded in the `.litertlm` model file) does **not** reference any custom template variables — it only uses `bos_token`, `messages`, `tools`, `add_generation_prompt`, and `enable_thinking`. Any `extra_context` values are silently dropped by the template engine. Verified by extracting the template from the model binary and by runtime testing with flat key-value JSON. **Workaround:** per-turn dynamic context (game state, player state) is prepended as plain text in the user message, wrapped in `[Context]`/`[/Context]` tags, via `BuildMergedContext()`. The `SetSystemContext`/`SetUserContext` API on `UInoLiteRtLmConversation` feeds into this path.

When the InoLiteRT pinned SHA bumps again, re-test all three of these — they are the most impactful unlocks (lower RAM via F16, creative control via temperature, native context injection).

## Integration approach: link, not subprocess

We considered and rejected a subprocess-based integration (spawning `litert_lm_main --multi_turns` and piping stdin/stdout). Reasons:

1. **Tool calling is only available through the C++ / C API, not the CLI.** The CLI's `--multi_turns` mode does plain text chat only. A subprocess backend could never expose the plugin's headline feature.
2. **Subprocess is impossible on iOS** (code signing prohibits `exec` of bundled binaries) **and forbidden by Google Play Protect on Android.** Since phases 2–3 must link LiteRT-LM as a library anyway, doing subprocess for phase 1 would just mean writing the same backend twice.
3. **Crash isolation is real but restart cost is huge.** Reloading a 3.2 GB Gemma 4 model after a subprocess crash takes seconds. Not a win in practice.

The plugin uses **one integration strategy across all five platform phases: linked library via the LiteRT-LM public C API**.

## DLL boundary: pure C API (`litert/lm/engine.h`)

InoAgents calls the LiteRT-LM C API directly from
`Source/InoAgents/Private/LiteRtLm/InoLiteRtLmConversation.cpp` and
`InoLiteRtLmConversationWorker.cpp` — `#include "litert/lm/engine.h"`,
which resolves through InoLiteRT's public include path. The header is
pure C (`extern "C"`) with opaque pointers + primitive types + C
callbacks, so the UE integration skips entire classes of pain:

- **No C++ ABI matching** — no libstdc++-vs-MSVC-STL concerns, no
  iterator ABI, no exception propagation across the DLL boundary.
- **No STL types in the boundary** — `LiteRtLmEngine*`,
  `LiteRtLmSession*`, `LiteRtLmConversation*`, etc. + primitive types
  + `LiteRtLmStreamCallback`. Nothing more.
- **No hand-written wrapper layer** — UE-side work is marshalling
  between C types and UE types, not bridging ABIs.

The subset of the API InoAgents actively consumes:

- **Engine / conversation lifecycle.** `litert_lm_engine_settings_create`
  (with backend `"cpu"` / `"gpu"` / `"npu"`, vision/audio backends
  currently `nullptr`), `litert_lm_engine_create`, no-arg
  `litert_lm_conversation_config_create()` + per-field setters
  (`set_session_config`, `set_system_message`, `set_tools`,
  `set_messages`, `set_enable_constrained_decoding`),
  `litert_lm_conversation_create`.
- **Streaming.** `litert_lm_conversation_send_message_stream` with our
  `LiteRtLmStreamCallback` trampoline; `litert_lm_conversation_cancel_process`
  for cooperative abort.
- **Sampler params + activation precision** via `LiteRtLmSessionConfig`
  + `litert_lm_engine_settings_set_activation_data_type` (see Gemma 4
  runtime limitations above for current status).
- **Logging.** `litert_lm_set_min_log_level` (called from the InoLiteRT
  smoke test, not from InoAgents itself).

What's available in the API but **not yet wired** in InoAgents:
multimodal `LiteRtLmInputData` (image/audio types), tokenization helpers
(`litert_lm_engine_tokenize` / `_detokenize`,
`_get_start_token` / `_get_stop_tokens`), low-level prefill/decode split
(`litert_lm_session_run_prefill` / `_run_decode`),
`litert_lm_session_run_text_scoring`, benchmark info getters, speculative
decoding. See `Plugins/InoLiteRT/Source/ThirdParty/Public/litert/lm/engine.h`
for the full surface.

## Model scope

Only the two "E" (edge / on-device) Gemma 4 variants are in scope for this plugin:

| Variant | Effective params | Context | Modalities | File size |
|---|---|---|---|---|
| **E2B** | ~2B | 128K | Text + Image + Audio | ~2.6 GB |
| **E4B** | ~4B | 128K | Text + Image + Audio | ~3.7 GB |

**Both variants are fully multimodal** — verified by inspecting the `.litertlm` containers directly. Both files carry `tf_lite_vision_encoder` + `vision_adapter_280` and `tf_lite_audio_encoder_hw` + `audio_adapter_features/mask` sections, plus `<|image|>` and `<|audio|>` special tokens and template branches for both modalities. E4B differs from E2B only in LLM backbone size (4B vs 2B effective params); the vision and audio encoders are identical. Earlier versions of this file claimed E2B was text+image-only — that was wrong.

Note: **our UE-side wrapper currently passes `nullptr` for `vision_backend_str` and `audio_backend_str`** in `InoLiteRtLmSubsystem.cpp`'s `litert_lm_engine_settings_create` call, so even though the models support images/audio, the plugin's current code path only consumes text. Enabling multimodal input is tracked as future work — requires wiring image/audio payloads into our `UInoLiteRtLmConversation` Blueprint API and building `LiteRtLmInputData` arrays for the C API.

Gemma 4's **31B dense** and **26B A4B MoE** server-class variants are **intentionally out of scope** — they are not realistic to run inside a consumer UE game process alongside a renderer (17+ GB VRAM just for weights), and LiteRT-LM is an edge runtime, not a server runtime.

## Repository layout

```
Plugins/InoAgents/
├── InoAgents.uplugin                              ← declares "InoLiteRT", "InoOnnx", "InoLlama"
│                                                    in its Plugins array so UE refuses to load
│                                                    InoAgents without all three
│
├── Chatterbox/                                    ← Chatterbox Turbo TTS — model staging
│   ├── CHATTERBOX_VERSION                         ← HuggingFace repo revision pin
│   ├── scripts/
│   │   ├── setup-chatterbox.ps1                   ← downloads a variant (4 .onnx + companions
│   │   │                                            + tokenizer.json + default voice)
│   │   └── repro-dml-encoder.py                   ← minimal Python repro of the upstream DML
│   │                                                 kernel bugs we work around per-session
│   └── README.md
│
├── Source/
│   ├── InoAgents/                                 ← runtime module (UE-facing — consumes
│   │                                                LiteRT-LM, ORT, llama.cpp via siblings)
│   │   ├── InoAgents.Build.cs
│   │   ├── Public/
│   │   │   ├── InoAgents.h                        ← module interface (FInoAgentsModule)
│   │   │   ├── InoAgentsSettings.h                ← UInoAgentsSettings (UDeveloperSettings —
│   │   │   │                                        ElevenLabs + LiteRT-LM + Chatterbox)
│   │   │   ├── LiteRtLm/                          ← Blueprint-facing LLM types
│   │   │   │   ├── InoLiteRtLmTypes.h             ← model config, message struct, delegates
│   │   │   │   ├── InoLiteRtLmSubsystem.h         ← engine owner, tool registry, chat panel
│   │   │   │   ├── InoLiteRtLmConversation.h      ← stateful chat, sentence/tag flag bitmasks
│   │   │   │   ├── InoLiteRtLmToolBase.h          ← Blueprintable tool base class
│   │   │   │   └── InoLiteRtLmAddNumbersTool.h    ← canonical tool sample
│   │   │   ├── Onnx/                              ← generic ONNX session / tensor API
│   │   │   │   │                                    (built on top of the InoOnnx-supplied
│   │   │   │   │                                    OrtApi vtable via InoAgents::Onnx::GetApi())
│   │   │   │   ├── InoOnnxTypes.h                 ← Dtype / Provider / SessionOptions enums+struct
│   │   │   │   ├── InoOnnxTensor.h                ← FInoOnnxTensor (move-only OrtValue wrapper)
│   │   │   │   └── InoOnnxSession.h               ← FInoOnnxSession (Create, Run, RunAsync)
│   │   │   ├── Chatterbox/                        ← Chatterbox TTS Blueprint surface
│   │   │   │   ├── InoChatterboxTurboNativeTypes.h           ← variant enum, voice/options/result USTRUCTs,
│   │   │   │   │                                    FInoChatterboxTurboNativePerformanceOptions (DML overrides)
│   │   │   │   ├── InoChatterboxTurboNativeSubsystem.h    ← UInoChatterboxTurboNativeSubsystem
│   │   │   │   └── InoChatterboxTurboNativeStreamSynthesize.h ← async-action wrapper
│   │   │   ├── ElevenLabs/                        ← Cloud TTS Blueprint surface
│   │   │   │   ├── InoElevenLabsTypes.h           ← request struct, output-format enum, delegates
│   │   │   │   ├── InoElevenLabsSubsystem.h       ← settings cache + live-action registry
│   │   │   │   └── InoElevenLabsTextToDialogueStream.h ← async-action wrapper
│   │   │   ├── Animation/
│   │   │   │   └── InoAnimationBlueprintHelper.h  ← eye-look + procedural blink helpers
│   │   │   ├── Audio/
│   │   │   │   └── InoAudioFunctionLibrary.h      ← silence/dithered-silence/SaveWav helpers
│   │   │   └── UI/Slate/                          ← in-PIE chat panel public API
│   │   │       ├── SInoChatPanel.h                ← Slate widget
│   │   │       └── InoChatBridge.h                ← UObject glue (UFUNCTION delegate handlers)
│   │   └── Private/
│   │       ├── InoAgents.cpp                      ← thin module lifecycle (no DLL loading —
│   │       │                                        sibling plugins pre-load at PreLoadingScreen)
│   │       ├── InoAgentsLog.h                     ← shared LogInoAgents category
│   │       ├── InoAgentsSettings.cpp
│   │       ├── LiteRtLm/                          ← LLM-side impl
│   │       │   ├── InoLiteRtLmSubsystem.cpp       ← engine owner + chunked download flow
│   │       │   ├── InoLiteRtLmConversation.cpp    ← sentence/tag state machines
│   │       │   ├── InoLiteRtLmConversationWorker.{h,cpp} ← FRunnable, tool agent loop
│   │       │   ├── InoLiteRtLmTypes.cpp           ← model path resolution
│   │       │   ├── InoLiteRtLmToolBase.cpp        ← schema builder
│   │       │   ├── InoLiteRtLmAddNumbersTool.cpp
│   │       │   ├── InoLiteRtLmStubs_NonWindows.cpp ← (compile-only) stubs for iOS/Linux/macOS
│   │       │   └── InoSha256.{h,cpp}              ← model-file integrity check
│   │       ├── Onnx/                              ← ORT-side impl (calls into the
│   │       │   │                                    InoOnnx-supplied OrtApi vtable)
│   │       │   ├── InoOnnxInternal.{h,cpp}        ← CheckOrtStatus, dtype conv, env singleton
│   │       │   ├── InoOnnxTensor.cpp
│   │       │   └── InoOnnxSession.cpp
│   │       ├── Chatterbox/                        ← Chatterbox TTS impl (see "Chatterbox" section)
│   │       │   ├── InoChatterboxTurboNativeSubsystem.cpp  ← Blueprint glue + multi-file download
│   │       │   ├── InoChatterboxTurboNativeRunner.{h,cpp}    ← 4-session pipeline orchestrator
│   │       │   ├── InoChatterboxTurboNativeModels.{h,cpp}    ← session bundle loader (per-session DML routing)
│   │       │   ├── InoChatterboxTurboNativeTokenizer.{h,cpp} ← GPT-2 BPE + paralinguistic tags
│   │       │   ├── InoChatterboxTurboNativeSynthesisWorker.{h,cpp} ← FRunnable + FIFO queue
│   │       │   ├── InoChatterboxTurboNativeDecoderWorker.{h,cpp}   ← parallel decoder for streaming
│   │       │   ├── InoChatterboxTurboNativeTypes.cpp
│   │       │   └── InoChatterboxTurboNativeStreamSynthesize.cpp
│   │       ├── ElevenLabs/                        ← Cloud TTS impl
│   │       │   ├── InoElevenLabsSubsystem.cpp
│   │       │   └── InoElevenLabsTextToDialogueStream.cpp
│   │       ├── Animation/InoAnimationBlueprintHelper.cpp
│   │       ├── Audio/InoAudioFunctionLibrary.cpp
│   │       ├── UI/Slate/                          ← chat panel widgets + theme
│   │       │   ├── SInoChatPanel.cpp
│   │       │   ├── InoChatBridge.cpp
│   │       │   ├── InoChatStyle.{h,cpp}
│   │       │   ├── SInoChatHeader.{h,cpp}
│   │       │   ├── SInoChatInput.{h,cpp}
│   │       │   ├── SInoMessageBubble.{h,cpp}
│   │       │   ├── SInoStatusDot.{h,cpp}
│   │       │   └── SInoToolPill.{h,cpp}
│   │       └── SmokeTests/                        ← dev-time console commands
│   │           ├── InoSmokeTestCommon.{h,cpp}     ← shared helpers (model paths, JSON, etc.)
│   │           ├── InoLoadEngineTest.cpp          ← Ino.LoadEngineTest
│   │           ├── InoGenerateTest.cpp            ← Ino.GenerateTest
│   │           ├── InoConversationTest.cpp       ← Ino.ConversationTest
│   │           ├── InoToolCallTest.cpp            ← Ino.ToolCallTest
│   │           ├── InoStreamTest.cpp              ← Ino.StreamTest
│   │           ├── InoLiteRtLmSubsystemLoadTest.{h,cpp}     ← Ino.LiteRtLm.SubsystemLoadTest
│   │           ├── InoLiteRtLmConversationSendTest.{h,cpp}  ← Ino.LiteRtLm.ConversationSendTest
│   │           ├── InoLiteRtLmConversationStreamTest.{h,cpp}← Ino.LiteRtLm.ConversationStreamTest
│   │           ├── InoLiteRtLmConversationToolTest.{h,cpp}  ← Ino.LiteRtLm.ConversationToolTest
│   │           ├── InoLiteRtLmConversationContextTest.{h,cpp} ← Ino.LiteRtLm.ConversationContextTest
│   │           ├── InoLiteRtLmToolRegistryTest.cpp ← Ino.LiteRtLm.ToolRegistryTest
│   │           ├── InoLiteRtLmShowChatPanelTest.cpp ← Ino.LiteRtLm.ShowChatPanel / HideChatPanel
│   │           ├── InoOnnxTest.cpp                ← Ino.Onnx.ProvidersTest, .SessionFromFileTest
│   │           ├── InoOnnxListDmlAdaptersTest.cpp ← Ino.Onnx.ListDmlAdaptersTest
│   │           ├── InoLlamaCppBackendInfoTest.cpp ← Ino.LlamaCpp.BackendInfoTest
│   │           ├── InoLlamaCppVtableTest.cpp      ← Ino.LlamaCpp.VtableTest
│   │           ├── InoChatterboxTurboNativeTest.cpp          ← Ino.Chatterbox.* (load/tokenizer/embed/AR/encoder/decoder/synth)
│   │           ├── InoChatterboxTurboNativeSubsystemTest.{h,cpp}    ← Ino.Chatterbox.SubsystemSynthTest
│   │           ├── InoChatterboxTurboNativeStreamSynthTest.{h,cpp}  ← Ino.Chatterbox.StreamSynthTest
│   │           └── InoElevenLabsDialogueStreamTest.{h,cpp} ← Ino.ElevenLabsDialogueStreamTest
│   └── (no Source/ThirdParty — sibling plugins own all third-party staging)
│
├── InoNeuTtsNative/                                ← NeuTTS Nano + Air sub-module (its
│   │                                                 own Source/InoNeuTtsNative/ tree —
│   │                                                 see "NeuTTS (Nano + Air)" section
│   │                                                 below for the full layout)
│   ├── Public/{InoNeuTtsTypes.h, InoNeuTtsSubsystem.h, InoNeuTtsSettings.h, ...}
│   └── Private/{InoNeuTtsRunner, InoNeuTtsSynthesisWorker, InoNeuTtsSubsystem.cpp,
│                InoNeuTtsCommon, InoNeuTtsPromptBuilder, InoNeuTtsVoiceRegistry,
│                BP async-actions, SmokeTests/}
│
└── (no Binaries/ThirdParty — sibling plugins own all runtime binaries)
```

**The three runtimes are NOT in this layout.** They live in sibling plugins, each in its own repo with its own `CLAUDE.md`:

- `Plugins/InoLiteRT/` — LiteRT + LiteRT-LM (Bazel-built, owns its `Source/ThirdParty/` and `LiteRT/` workspace).
- `Plugins/InoOnnx/`   — ONNX Runtime + DirectML (NuGet prebuilts, owns its `Source/ThirdParty/` and `OnnxRuntime/` workspace).
- `Plugins/InoLlama/`  — llama.cpp (release prebuilts, owns its `Source/ThirdParty/` and `LlamaCpp/` workspace).

All three pre-load their DLLs/.so at `LoadingPhase=PreLoadingScreen`, strictly before InoAgents' `Default`-phase StartupModule runs. From InoAgents' perspective the runtimes are simply available: `#include "litert/lm/engine.h"` / `#include "onnxruntime_c_api.h"` / `#include "llama.h"`, with the import libs and runtime staging handled by the sibling Build.cs files via the `"InoLiteRT"` / `"InoOnnx"` / `"InoLlama"` entries in `InoAgents.Build.cs`'s `PublicDependencyModuleNames`.

## Build system: handled by sibling plugins

InoAgents itself has no third-party build involvement. LiteRT-LM is built from source with Bazel by **`InoLiteRT`**; ONNX Runtime is downloaded as Microsoft prebuilts and patched / renamed by **`InoOnnx`**; llama.cpp is downloaded as ggml-org release artifacts by **`InoLlama`**. Adding new `litert_lm_*` / `Ort*` / `llama_*` calls in InoAgents only requires the corresponding `#include` — the headers are exposed via the sibling plugins' `PublicSystemIncludePaths`, the import libs are in their `PublicAdditionalLibraries`, and the runtime DLLs/.so are pre-loaded by their PreLoadingScreen-phase StartupModule. See `Plugins/InoLiteRT/CLAUDE.md`, `Plugins/InoOnnx/CLAUDE.md`, and `Plugins/InoLlama/CLAUDE.md` for each runtime's build / pin / packaging story. If a build error references a missing symbol, the fix is on the sibling-plugin side, not in InoAgents.

## ONNX Runtime API surface (used by Chatterbox / NeuTTS Nano / future ONNX consumers)

The ONNX Runtime itself — version pinning, prebuilt staging, Win64 / Android packaging, the DLL renames (`onnxruntime.dll` → `InoOnnxRuntime.dll`, `DirectML.dll` → `InoDml.dll`), the PE delay-import patch, the `OrtApi*` vtable load via `OrtGetApiBase` — is owned by the sibling **`InoOnnx`** plugin. See `Plugins/InoOnnx/CLAUDE.md` for that side of the story. From InoAgents' perspective the runtime is simply available: `#include "onnxruntime_c_api.h"`, get the cached vtable with `InoAgents::Onnx::GetApi()` (declared in InoOnnx's `InoOnnx.h`), and call ORT through it. There is no `Init()` / `Shutdown()` work in InoAgents — InoOnnx pre-loads at `LoadingPhase=PreLoadingScreen`, so the API is callable by the time `FInoAgentsModule::StartupModule` runs.

What InoAgents itself owns on top of that vtable is a small, model-agnostic **C++ wrapper** for ONNX Runtime — `FInoOnnxSession` + `FInoOnnxTensor` + supporting types. Every ONNX-model consumer in the plugin (Chatterbox Turbo's four sessions, NeuTTS Nano's NeuCodec decoder) is built on top of this layer; no ONNX-layer changes are needed to add new ONNX consumers.

### C++ API surface

Public headers live under `Source/InoAgents/Public/Onnx/`:

| Header | What it declares |
|---|---|
| `InoOnnxTypes.h` | `EInoOnnxDtype` (12 element types, UE enum), `EInoOnnxProvider` (Cpu / Xnnpack / Nnapi / WebGpu / DirectMl / Cuda / TensorRt), `EInoOnnxGraphOptimizationLevel`, `FInoOnnxSessionOptions` (provider priority list, thread counts, graph-opt level, custom session config map, profiling toggle) |
| `InoOnnxTensor.h` | `FInoOnnxTensor` — move-only wrapper around `OrtValue`. Factories: `Create(Dtype, Shape)`, `CreateFloat32/Int64(Shape)`, `CreateFromBufferCopy<T>(Shape, Data)`, `Adopt(Native, Dtype, Shape)`. Typed access: `GetData<T>()`, `GetMutableData<T>()`, `CopyToArray<T>(Out)`. Detail::TDtypeOf<T> for compile-time dtype deduction. |
| `InoOnnxSession.h` | `FInoOnnxSession` — the main class. Static `Create(ModelPath, Options)` / `CreateFromMemory(Bytes, Options)` factories return `TUniquePtr`. Sync `Run(Inputs, OutOutputs)` and async `RunAsync(MoveTemp(Inputs), OnComplete)`. Metadata accessors for input/output names, shapes, dtypes. `LogMetadata()` dumps the session's I/O to LogInoAgents. |

Private helpers under `Source/InoAgents/Private/Onnx/`:

| File | Role |
|---|---|
| `InoOnnxInternal.{h,cpp}` | `CheckOrtStatus` (uniform error log + release), dtype translations, provider-name debug strings, graph-opt-level translation, lazy-init `GetGlobalOrtEnv()`. Pulls the `OrtApi*` from `InoAgents::Onnx::GetApi()` (defined by sibling InoOnnx) and uses it for every call. |
| `InoOnnxTensor.cpp` | Tensor factories, OrtAllocator-backed allocation, data pointer access with sizeof(T) vs dtype-size consistency check. |
| `InoOnnxSession.cpp` | Options builder, provider registration with fallback logging, I/O metadata caching (OrtAllocator-owned name strings freed immediately), Run/RunAsync implementation. |

**Design principles worth preserving:**

- No Blueprint exposure in this layer. Per-model consumers (`UInoChatterboxTurboNativeSubsystem`, `UInoNeuTtsSubsystem`, future vision wrappers, etc.) add Blueprint-friendly APIs on top.
- Sessions are thread-safe for `Run()` per ORT guarantees; FInoOnnxTensors are move-only to avoid surprise-cost deep clones on the LLM streaming hot path.
- Exception-free — uses the C API (`onnxruntime_c_api.h`), not the C++ API (`onnxruntime_cxx_api.h` throws `Ort::Exception`). UE modules default `bEnableExceptions=false`; keeping the whole stack exception-free avoids per-module opt-ins.
- Positional I/O ordering for `Run()`. A named-map variant would cost a hash lookup per inference which matters on AR token loops — callers who need names can wrap trivially at their own layer.

### Execution providers

`FInoOnnxSessionOptions::ExecutionProviders` is the priority list every consumer uses to ask for accelerators. Which providers actually register depends on what the InoOnnx-staged build of ORT was compiled with — see `Plugins/InoOnnx/CLAUDE.md` for the per-platform list. As of the current pin: Windows has CPU + DirectML; Android has CPU + XNNPACK + NNAPI + WebGPU.

DirectML is selected via `EInoOnnxProvider::DirectMl` in the priority list, with `DirectMlAdapterIndex` controlling which D3D12 device to bind (0 = default adapter — usually the primary display GPU; higher indices target secondary dGPUs, eGPUs, or NPUs that enumerate later under Windows 11 24H2+ driver builds).

DirectML is a young EP with known kernel-level bugs that show up on real models. InoAgents' Chatterbox subsystem handles this with per-session opt-in flags, defaulting all sessions to CPU and letting callers flip individual sessions to GPU after verifying — see the "Chatterbox Turbo TTS" section below for the matrix of which Chatterbox sessions actually work on DirectML at the current pin.

Provider fallback: if a caller requests `[DirectMl, Cpu]` on a platform without a D3D12 device (or with a corrupt DML install), DirectML registration silently fails and the session is built with the remaining providers. The same applies to `[Xnnpack, Nnapi, Cpu]` on Android where NNAPI isn't registered. `FInoOnnxSession::GetActiveProviders()` reports what actually made it onto the session.

### Smoke tests

Three console commands under `Source/InoAgents/Private/SmokeTests/`, auto-registered as `FAutoConsoleCommand` globals (same pattern as the LLM smoke tests):

| Command | Args | What it does |
|---|---|---|
| `Ino.Onnx.ProvidersTest` | none | Calls `OrtApi::GetAvailableProviders` via the cached API vtable and logs every entry. Doubles the module-startup check; useful after Live Coding or as a first diagnostic. |
| `Ino.Onnx.SessionFromFileTest <abs-path-to-model.onnx>` | 1 | Loads the ONNX model, calls `FInoOnnxSession::LogMetadata()` (dumps I/O shapes + dtypes + active providers). If all inputs have concrete shapes, allocates zero-filled inputs and runs one forward pass; reports load time + run time + output shapes. Exercises the full Session + Tensor API end-to-end with no per-model code. |
| `Ino.Onnx.ListDmlAdaptersTest` | none | Enumerates D3D12 adapters via `IDXGIFactory` and logs each one's description, vendor, and dedicated VRAM, alongside the `DirectMlAdapterIndex` value that selects it. Use this when a machine has multiple GPUs or an NPU and you need to pick the right index for `FInoChatterboxTurboNativePerformanceOptions::DirectMlAdapterIndex` (or any other DML-using session). |

## Chatterbox Turbo TTS (the first ONNX consumer — shipping)

Chatterbox Turbo is the first real-world consumer of the ONNX Runtime layer. It's Resemble AI's 350M-parameter English TTS model with voice cloning, paralinguistic tags (`[laugh]`, `[cough]`, `[chuckle]`), and a distilled single-step decoder. Runs on top of the same `FInoOnnxSession` / `FInoOnnxTensor` primitives described above — no ONNX-layer changes required.

### Canonical source (trust this first)

- **Official ONNX weights**: [`ResembleAI/chatterbox-turbo-ONNX`](https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX) — exported by Xenova (HF Staff), published under MIT. Repository also ships `tokenizer.json` + `tokenizer_config.json` + `config.json` + `generation_config.json` + `preprocessor_config.json` at the root.
- **Official reference inference script**: the "Usage → Chatterbox-Turbo" block of the model card's README. This is Resemble AI's canonical Python loop — our C++ port is a direct translation of it.
- **Community C++ port** (useful cross-reference): [`DDATT/Chatterbox-turbo-cpp`](https://github.com/DDATT/Chatterbox-turbo-cpp) — a working C++ implementation using ONNX Runtime. Skips `speech_encoder` at runtime and loads precomputed `.bin` files for the speaker conditioning; includes a Claude-assisted BPE tokenizer port.
- **Xenova community fork** (slightly different I/O shape): [`onnx-community/chatterbox-ONNX`](https://huggingface.co/onnx-community/chatterbox-ONNX) and [`onnx-community/chatterbox-multilingual-ONNX`](https://huggingface.co/onnx-community/chatterbox-multilingual-ONNX). These expose `exaggeration` + `position_ids` inputs on `embed_tokens`; the official Turbo ONNX does **not**. Do not cross-mix the two — their `embed_tokens` signatures are incompatible.
- **Resemble source repo**: [`resemble-ai/chatterbox`](https://github.com/resemble-ai/chatterbox) (PyTorch training / research code, Apache 2.0).
- **Deployment anecdotes** (iPhone/Mac offline run): [HF Discussion #42](https://huggingface.co/ResembleAI/chatterbox/discussions/42) — reports ~3.2 GB peak RAM for fp32 and flags the conditional_decoder's attention ops as the bottleneck.
- **Watermarker**: [`resemble-ai/perth`](https://github.com/resemble-ai/perth) (PerTh implicit watermarking, used optionally post-generation).

### The four ONNX graphs

All four live under the `onnx/` subfolder of the HF repo. Each graph has a paired `{filename}_data` weights file (ONNX external-data format for models > 2 GB). Quantization variants are suffixed, using the naming convention the official `download_model()` helper uses:

| `dtype` arg | Graph filename | Notes |
|---|---|---|
| `fp32` (default) | `<name>.onnx` | No suffix. Matches the weights bundled with the original Chatterbox Turbo checkpoint. |
| `fp16` | `<name>_fp16.onnx` | Half-precision weights + activations. Good CPU speed / RAM tradeoff. |
| `q8` | `<name>_quantized.onnx` | **NOTE the unusual `_quantized` suffix, not `_q8`.** INT8 quantization. |
| `q4` | `<name>_q4.onnx` | 4-bit weights. Smallest footprint. |
| `q4f16` | `<name>_q4f16.onnx` | 4-bit weights + fp16 activations. |

The four `<name>` values:

| Name | Purpose | Runs |
|---|---|---|
| `speech_encoder` | Reference-audio → speaker conditioning tensors | Once per voice (can be cached / precomputed). |
| `embed_tokens` | Text token ids → hidden embeddings | Once on the full prompt, then once per generated token. |
| `language_model` | Llama-3-style 24-layer autoregressive sampler over speech tokens (KV-cached) | Once per generated speech token, up to `max_new_tokens`. |
| `conditional_decoder` | Speech tokens + speaker conditioning → 24 kHz waveform | Once at the end (single-step in Turbo — distilled from the original 10-step). |

### Pipeline constants (hardcoded by the model)

```cpp
constexpr int32   SAMPLE_RATE           = 24000;  // 24 kHz float32 waveform output
constexpr int64_t START_SPEECH_TOKEN    = 6561;
constexpr int64_t STOP_SPEECH_TOKEN     = 6562;
constexpr int64_t SILENCE_TOKEN         = 4299;   // 3× appended before decoder to pad end
constexpr int32   NUM_KV_HEADS          = 16;     // language_model
constexpr int32   HEAD_DIM              = 64;     // language_model
constexpr int32   NUM_HIDDEN_LAYERS     = 24;     // language_model (verified from live model's declared inputs; original non-Turbo Chatterbox has 30 — do not confuse)
constexpr float   REPETITION_PENALTY    = 1.2f;   // default from the reference script
constexpr int32   DEFAULT_MAX_NEW_TOKENS = 1024;  // reference script default
```

### The full pipeline (ports the model-card reference loop)

```
                           ┌─────────────────────────────────────────────────┐
                           │ Inputs                                          │
                           │   text            (string, may contain [laugh]) │
                           │   reference_audio (WAV at 24 kHz mono float32)  │
                           └────────────────────────┬────────────────────────┘
                                                    │
  1.  tokenizer.json → BPE.encode(text) → input_ids (int64[1, seq_len])
                                                    │
  2.  embed_tokens.run({"input_ids": input_ids}) → inputs_embeds (float32[1, seq_len, hidden])
                                                    │
  3.  speech_encoder.run({"audio_values": reference_audio[np.newaxis, :]}) →
          cond_emb            (float32[1, cond_seq, hidden])
          prompt_token        (int64[1, T_prompt])
          speaker_embeddings  (float32[1, spk_dim])
          speaker_features    (float32[1, feat_dim])
                                                    │
  4.  inputs_embeds = concat(cond_emb, inputs_embeds) along axis=1
      attention_mask = ones([1, cond_seq + seq_len], int64)
      position_ids   = arange(0, cond_seq + seq_len, int64)
      past_key_values = { each zero tensor [1, 16, 0, 64] matching the model's input dtype }
      generate_tokens = [[START_SPEECH_TOKEN]]
                                                    │
  5.  FOR i in 0..max_new_tokens:
          logits, *present_key_values =
              language_model.run({
                  inputs_embeds, attention_mask, position_ids, **past_key_values,
              })
          logits = logits[:, -1, :]
          logits = RepetitionPenalty(generate_tokens, logits, penalty=1.2)
          next_token = argmax(logits, axis=-1, keepdims=True).astype(int64)
          generate_tokens = concat(generate_tokens, next_token)
          if next_token == STOP_SPEECH_TOKEN: break
          inputs_embeds  = embed_tokens.run({"input_ids": next_token})     # 1 new token only
          attention_mask = concat(attention_mask, ones([1, 1]))
          position_ids   = position_ids[:, -1:] + 1
          past_key_values = present_key_values    (update every layer's K,V in place)
                                                    │
  6.  speech_tokens = generate_tokens[:, 1:-1]                             # drop START & STOP
      silence_tokens = full([1, 3], SILENCE_TOKEN, int64)                  # pad end
      speech_tokens = concat(prompt_token, speech_tokens, silence_tokens)
                                                    │
  7.  wav = conditional_decoder.run({
               speech_tokens, speaker_embeddings, speaker_features,
           })[0].squeeze(axis=0)                                           # float32 @ 24 kHz
                                                    │
  8.  (optional) perth.PerthImplicitWatermarker().apply_watermark(wav, 24000)
                                                    │
                                                    ▼
                                 PCM waveform, float32, 24 kHz mono
```

### Three subtleties worth calling out

1. **KV-cache dtype discovery.** The official reference script iterates `language_model_session.get_inputs()` and picks `float16` vs `float32` per-input based on the ONNX tensor type, rather than assuming one globally. Our C++ port must do the same — otherwise fp16 quantization variants will fail at runtime with a dtype mismatch. `FInoOnnxSession::GetInputs()` / `GetInputMeta()` already expose the per-input dtype metadata we need.

2. **`embed_tokens` on Turbo takes ONLY `input_ids`.** No `position_ids`, no `exaggeration`. Those inputs exist on `onnx-community/chatterbox-ONNX` (the original non-Turbo model) but were removed during Turbo export. If you copy code from the community fork expecting those inputs, the Turbo graph will reject the call at Run() time with a missing-input error.

3. **Silence padding order matters.** The decoder wants `concat(prompt_token, generated_speech_tokens, silence×3)`. `prompt_token` comes from the speech encoder (first N tokens of the reference voice) and seeds the output before the generated content — it's NOT the same as the text `input_ids`. Swapping or omitting it produces audio that starts with a click or skips the voice-cloning continuity.

### Sizing / deployment realities

- **Total repo size** across all 5 quantization levels: 7.39 GB. A single-dtype deployment bundle is roughly 1.4 GB (fp32) down to ~350 MB (q4) per the HF file listing plus tokenizer/config.
- **RAM**: ~3.2 GB peak on iPhone/Mac at fp32 per HF Discussion #42. Gemma 4 E2B (~2.58 GB) + Chatterbox fp32 = ~5.8 GB resident, leaving ~2 GB for UE + OS on an 8 GB Android. Plan to ship **q4 or q4f16** on Android; fp16 or fp32 is fine on Win64.
- **Bottleneck**: the `conditional_decoder`'s attention layers dominate wall time. Turbo's single-step decoder is already the big win — no further model-side optimization available to us.
- **Streaming**: the reference loop is one-shot (full sentence synthesized before any audio is emitted). First-audio latency is roughly `max_new_tokens × per_token_ms + decoder_ms`. For conversational UX, plan to run the decoder incrementally on chunks of generated speech tokens so audio starts playing before the LM finishes — doable because Turbo's decoder is single-step and cheap per-chunk, but adds orchestration work.

### How it's wired up in the plugin

Actual layout (follows the pattern established by `Source/InoAgents/Onnx` / `Source/InoAgents/LiteRtLm`):

```
Plugins/InoAgents/
├── Chatterbox/                                    ← setup-time model downloader
│   ├── CHATTERBOX_VERSION                         ← HuggingFace revision pin
│   ├── scripts/setup-chatterbox.ps1               ← downloads + stages a variant
│   └── README.md
│
└── Source/InoAgents/
    ├── Public/Chatterbox/
    │   ├── InoChatterboxTurboNativeTypes.h                   ← variant enum, voice / options /
    │   │                                            result USTRUCTs, FInoChatterboxTurboNativeModelEntry
    │   │                                            (Project Settings registry), all delegates
    │   ├── InoChatterboxTurboNativeSubsystem.h            ← UInoChatterboxTurboNativeSubsystem
    │   └── InoChatterboxTurboNativeStreamSynthesize.h        ← Blueprint async-action wrapper
    └── Private/Chatterbox/
        ├── InoChatterboxTurboNativeRunner.{h,cpp}            ← 4-session orchestrator (encoder →
        │                                            embed → AR loop → decoder),
        │                                            owns per-call KV-cache state
        ├── InoChatterboxTurboNativeModels.{h,cpp}            ← LoadFromDir: builds the 4 ORT
        │                                            sessions with the per-session
        │                                            CPU / DML overrides applied
        ├── InoChatterboxTurboNativeTokenizer.{h,cpp}         ← GPT-2 BPE + paralinguistic-tag
        │                                            handling, parsed from tokenizer.json
        ├── (mono WAV reader / writer + PCM helpers used to resolve
        │    FInoChatterboxTurboNativeVoice::WavFilePath / ::ReferenceSamples are
        │    static methods on UInoAudioFunctionLibrary, in InoAgents
        │    Public/Audio/ — shared with any future TTS sub-module)
        ├── InoChatterboxTurboNativeSynthesisWorker.{h,cpp}   ← FRunnable + FIFO queue
        ├── InoChatterboxTurboNativeDecoderWorker.{h,cpp}     ← parallelises conditional_decoder
        │                                            chunks during streaming so the AR
        │                                            loop and decode overlap
        ├── InoChatterboxTurboNativeStreamSynthesize.cpp
        ├── InoChatterboxTurboNativeSubsystem.cpp          ← Blueprint glue + multi-file
        │                                            download flow (HEAD probe + GET +
        │                                            .partial staging)
        └── (smoke tests live under Private/SmokeTests/, see Smoke tests section)
```

None of this touches `Source/InoAgents/Public/Onnx/` or its Private siblings — the TTS layer is strictly a consumer of `FInoOnnxSession`. If you need to add model-agnostic ONNX capabilities (e.g. new dtype support, new provider), do it there first before the Chatterbox layer.

### Quantization variants the subsystem can load

`EInoChatterboxTurboNativeVariant` covers the five dtypes published in the HF repo. Approximate on-disk size for the four runtime files (the four `<name>` columns above) plus weights companions:

| Enum value | HF dtype string | On-disk | Notes |
|---|---|---|---|
| `Q4F16` (default) | `q4f16` | ~510 MB | 4-bit weights + fp16 activations. Smallest + fastest. |
| `FP16` | `fp16` | ~1.5 GB | Half-precision throughout. Essentially identical quality to fp32 on Chatterbox. |
| `Q4` | `q4` | ~640 MB | 4-bit weights, fp32 activations. Use on x86 without AVX-512 FP16. |
| `FP32` | (no suffix) | ~3.2 GB | Reference-quality benchmark. Rarely worth shipping. |
| `Quantized` | `quantized` | ~1.0 GB | INT8 throughout. Quality varies by sentence. |

Only one variant is resident at a time — switching is `UnloadModels()` then a fresh `LoadModelsAsync(NewConfig)`. The on-disk staging path is `<PersistentDownloadDir>/InoAgents/Models/Chatterbox/<variant>/` (matching `setup-chatterbox.ps1`); each variant downloads independently and lives in its own subdirectory so previously-downloaded variants survive a switch.

### Subsystem API surface

`UInoChatterboxTurboNativeSubsystem` mirrors `UInoLiteRtLmSubsystem`'s ergonomics — game-instance-scoped, async load with progress, async synth with cancellation. Public methods that matter:

- `LoadModelsAsync(Config, OnLoaded)` — resolves missing files via the Project Settings entry (`UInoAgentsSettings::ChatterboxModels`), downloads them sequentially with `OnDownloadProgress` (HEAD-probe pass for aggregate total → GET pass with `.partial` staging + atomic rename → ThreadPool dispatch into `Models::LoadFromDir` + tokenizer parse), then fires `OnLoaded(true, "")` on the game thread. Optional `default_voice.wav` (~714 KB) is downloaded as a non-required file alongside the model, so the minimal "load + synth" flow can be a no-args `SynthesizeAsync` call (no reference voice required from the caller).
- `SynthesizeAsync(Text, Voice, Options, OnComplete)` — one-shot synthesis. Worker dispatches the runner, runner produces the full 24 kHz mono int16 PCM LE waveform in `Result.AudioSamples`, marshals back to the game thread.
- `SynthesizeStreamAsync(Text, Voice, Options, OnAudioChunk, OnComplete, StreamChunkTokens=20)` — same machinery, but the runner re-runs the conditional decoder every `StreamChunkTokens` AR-loop tokens and fires `OnAudioChunk` with each delta. Last chunk has `bIsFinal=true`, then `OnComplete` fires with the concatenated waveform. Set `StreamChunkTokens` to 0 to fall back to one-shot semantics.
- `CancelSynthesis()` — cooperative abort. Currently-running AR iteration finishes (tens of ms), worker unwinds, every queued + in-flight item terminates with `OnComplete(false, ..., "Cancelled")`. Auto-fired by `UnloadModels` and PIE end.
- `IsModelDownloaded(Variant)` — pure file-stat probe. Safe to poll from a UMG widget (no SHA check, no I/O beyond directory enumeration). Returns true when the four `.onnx` files + `tokenizer.json` exist non-empty in the variant's resolved directory; the `.onnx_data` companions and `config.json` / `generation_config.json` are not part of the required set.

`FInoChatterboxTurboNativeVoice` resolution priority (per call): `WavFilePath` (24 kHz mono PCM int16 or float32 — no silent resampling) → `ReferenceSamples` (24 kHz mono int16 PCM LE bytes) → `PrecomputedConditioningPath` (RESERVED for Phase E; setting this in Phase D errors with a clear message) → `<variant_dir>/default_voice.wav` (auto-downloaded, MIT-licensed).

### Per-session execution-provider overrides (DirectML caveats)

Chatterbox's four ORT sessions don't all behave well on every accelerator. `FInoChatterboxTurboNativePerformanceOptions` exposes a "force CPU" flag per session, **all defaulting to true**, plus a global `bPreferDirectMl` (Windows) and `DirectMlAdapterIndex`. The defaults are deliberately conservative — flip individual flags only after verifying on the target hardware.

Empirical state with ORT 1.24.3:

| Session | Windows / DirectML | Android / XNNPACK | Default |
|---|---|---|---|
| `speech_encoder` | ❌ `E_INVALIDARG` at `MultiHeadAttention` (`MLOperatorAuthorImpl.cpp:2508`) | ❌ XNNPACK NHWC transformer rewrites `AveragePool` into `com.ms.internal.nhwc` domain; AAR is missing the matching kernel | CPU |
| `embed_tokens` | ❌ `E_INVALIDARG` at a `Slice` op on iter 1 of the AR loop | ⚠ unverified | CPU |
| `language_model` | ❌ silent numerical corruption on fp16 (audible noise instead of speech); crash on q4f16 | ⚠ unverified | CPU |
| `conditional_decoder` | ✅ correct | ⚠ unverified | CPU (safe), flip to GPU for ~15-20% speedup |

Confirmed upstream-side via a minimal Python repro using stock `onnxruntime-directml 1.24.3` (`Plugins/InoAgents/Chatterbox/scripts/repro-dml-encoder.py`). Microsoft has moved DirectML to "sustained engineering"; do not expect these to be fixed soon. The verified-safe Windows opt-in for performance is `bConditionalDecoderOnCpu=false` + `bPreferDirectMl=true` + the other three flags left at their defaults — that's the configuration our smoke tests exercise when DirectML is requested.

### Streaming via incremental decoder runs

`FInoChatterboxTurboNativeRunner`'s streaming path keeps the language-model AR loop running on its own thread while a parallel `FInoChatterboxTurboNativeDecoderWorker` re-runs `conditional_decoder` on rolling chunks of generated speech tokens. The first chunk fires `OnAudioChunk` once `StreamChunkTokens` (default 20, ~0.6 s of audio) tokens are ready, dropping the typical first-audio latency from "max_new_tokens × per_token_ms + decoder_ms" to roughly "20 × per_token_ms + first decoder_ms" — under a second for short utterances on a modern desktop. The decoder worker exists because `conditional_decoder` is the most expensive single op in the pipeline; running it inline on the AR thread would stall token generation while audio rendered, defeating the latency win.

## NeuTTS (Nano + Air) — `InoNeuTtsNative` sub-module

Neuphonic's NeuTTS — a Qwen2-derived GGUF backbone (Nano ~120M / Air ~360M
active params) that emits FSQ speech tokens, fed into NeuCodec's ONNX
decoder to produce 24 kHz mono waveforms with voice cloning from a
reference voice. Sub-module of the `InoAgents` plugin sitting alongside
Chatterbox Turbo + ElevenLabs as a third TTS option, with different
voice character, smaller LM footprint than Chatterbox, and different
licensing (NeuTTS Open License 1.0, not Apache 2.0).

This is the **first and currently only consumer of the llama.cpp
runtime** in the plugin. It validates the llama.cpp integration with a
real-world workload and establishes the architecture for any future
GGUF-format consumer (more TTS models, Whisper / ASR, general small
LLMs, etc).

### Canonical source — trust the vendor first

The vendor's Python reference is checked into the repo as a git
submodule for cross-checking against any C++ port:

- **Vendor Python**: `Plugins/InoAgents/NeuTTS/vendor/neutts/neutts.py`
  (and `phonemizers.py`). The C++ pipeline (`InoNeuTtsSynthesisWorker.cpp`'s
  `RunSynthesis` + `RunStreamingSynthesis`) is a direct port of vendor's
  `_infer_ggml` + `_infer_stream_ggml` + `_linear_overlap_add`. Match to
  vendor was audited in a structured pass — see "Audit pass / vendor
  alignment" below.
- **Backbones (HF)**: `neuphonic/neutts-nano-q4-gguf` (195 MB),
  `neuphonic/neutts-air-q4-gguf` (~430 MB), `neuphonic/neutts-nano-q8-gguf`,
  multilingual variants (`neutts-nano-{german,french,spanish}-{q4,q8}-gguf`).
- **Codec decoder (HF)**: `neuphonic/neucodec-onnx-decoder` (~530 MB
  fp32, single codebook, 50 Hz token rate, 24 kHz output) — same decoder
  for every backbone.
- **NeuCodec encoder**: `neuphonic/neucodec` — PyTorch-only, used
  **offline** via `Plugins/InoAgents/NeuTTS/scripts/build-voices.py` to
  produce reference-voice FSQ codes.

### Nano vs Air — same code path

NeuTTS Nano and NeuTTS Air run on **identical** UE code paths. Same
chat template, same special tokens, same sampler, same decoder, same
context size. The only difference is the GGUF file's parameter count
(~120M vs ~360M) — encoded inside the file itself, not in our config.

Practical differences (not in code, but worth knowing for tuning):

|                       | Nano (Q4) | Air (Q4)            |
|---|---|---|
| On-disk size          | ~195 MB   | ~430–530 MB         |
| Resident RAM          | ~250 MB   | ~600 MB             |
| Tokens/sec on CPU     | ~120 tok/s | ~30–40 tok/s (~3× slower) |
| Quality               | Decent    | Noticeably better   |
| Languages             | en/de/fr/es variants | English only |

There is no `EInoNeuTtsVariant` enum and no per-variant settings array.
A single `BackboneModels` array in Project Settings lists every entry
(any mix of Nano + Air); `FInoNeuTtsConfig::BackboneModelName` selects
one by `DisplayName`.

### Architecture

NeuTTS is a **pure consumer** of three sibling-plugin runtimes:

- **`InoLlama`** for the GGUF backbone, accessed via `InoAgents::LlamaCpp::GetApi()`'s
  `FLlamaCppApi` vtable.
- **`InoOnnx`** for the NeuCodec decoder, via the generic `FInoOnnxSession`
  wrapper in `InoAgents/Source/InoAgents/Public/Onnx/`.
- **`InoSpeakNG`** for runtime IPA phonemization of input text + voice
  reference text.

Plus **`InoNodes`** for the generic file downloader and SHA-256 helpers.

```
UInoNeuTtsSubsystem  (UGameInstanceSubsystem)
├── ActiveVoice + ActiveVoiceName    (mirror of cache identity, game thread)
├── CurrentBackboneName              (skip redundant LoadModel for same backbone)
├── CurrentCancelFlag                (TSharedPtr<atomic<bool>>; auto-fired by
│                                     UnloadModel / Deinitialize / CancelSynthesis)
└── Runner                           (TSharedPtr<FInoNeuTtsRunner>)
      ├── llama_model* + llama_context* + cached llama_vocab*
      ├── llama_token StopTokenId      (resolved once at load via vocab scan)
      ├── TUniquePtr<FInoOnnxSession>  (NeuCodec decoder)
      └── TUniquePtr<FInoNeuTtsVoiceCache>
            ├── VoiceName              (identity check)
            ├── ResolvedRefPhones      (whitespace-normalized RefText phonemization)
            ├── SpeechTokensBlock      (pre-built "<|speech_N1|>...<|speech_NK|>" string)
            ├── PrefixTokens           (pre-tokenized cacheable prefix, ~50–100 toks)
            └── KvSnapshot             (post-prefix KV state from llama_state_seq_get_data)

free functions (in InoNeuTtsSynthesisWorker.cpp, dispatched via Async on ThreadPool):
    InoNeuTtsNative::RunSynthesis           (one-shot, full waveform)
    InoNeuTtsNative::RunStreamingSynthesis  (chunked + overlap-add)
```

There is no FRunnable thread per synth — both synth functions run on a
ThreadPool worker dispatched by the subsystem; they own no long-lived
state. The runner is shared via TSharedPtr so an in-flight synth
survives `UnloadModel` cleanly.

### Lifecycle

```
1. (configure once)  Project Settings → Plugins → Ino NeuTTS Native
                       → BackboneModels[] + DecoderModels[]
                       Each entry: DisplayName, DownloadUrl, LocalFileName,
                       ExpectedSha256, FileSizeBytes, Language, Quantization

2. (optional, async) DownloadModelAsync(Config, OnComplete, OnProgress)
                       Pre-stage models without loading. Useful for
                       app-startup downloads or download-progress UI.
                       OR: just call IsModelDownloaded(Config) to query.

3. (async) LoadModelAsync(Config, OnLoaded, OnDownloadProgress)
                       Downloads if needed (re-using the same
                       InoNodes::Download::DownloadFilesAsync pipeline as
                       DownloadModelAsync), then ThreadPool-loads the
                       GGUF + ONNX, runs decoder + backbone warmup.

4. (async) SetActiveVoiceAsync(Voice, OnReady)
                       Tokenizes the cacheable prefix, prefills it into
                       seq 0, snapshots KV state. ~200–600 ms once.
                       REQUIRED before any synth.

5. (async, N times) SynthesizeAsync(Text, Options, OnComplete)
                  OR  SynthesizeStreamAsync(Text, Options, ChunkTokens,
                                            OnAudioChunk, OnComplete)

6. (anytime)        CancelSynthesis()              cooperative abort
                    ClearActiveVoice()             drop voice cache
                    UnloadModel()                  drop runner

7. (auto on Deinitialize / PIE end)  Cancel + Unload
```

To switch voice: call `SetActiveVoiceAsync(Other)` again. To switch
backbone: call `LoadModelAsync(NewConfig)` — the active voice is
cleared automatically. There is **no per-voice synth overload**;
voice flows through `SetActiveVoiceAsync` exclusively. See "Why no
per-voice synth" below.

### The synth pipeline (per call, on the ThreadPool worker)

Mirrors vendor's `_infer_ggml` step-for-step. Numbers in parens are
rough breakdown for a typical Nano-Q4 / 45-char input / 650-code
reference / 2.5 s audio synth on CPU.

1. **Phonemize input + reference text** via `UInoSpeakNGBPLibrary::Phonemize`.
   Reference text uses the cache's pre-resolved phonemes when set;
   otherwise live-phonemized. (~1–5 ms)
2. **Whitespace-normalize** both via `InoNeuTtsNative::NormalizePhones` —
   collapses any whitespace run to a single space and trims edges.
   Matches vendor's `phones.split() + " ".join()`.
3. **Build prompt** via `InoNeuTtsNative::BuildSynthesisPrompt`. Cache
   hit reuses the pre-built `<|speech_N1|>...` block string from
   `Cache->SpeechTokensBlock`; cache miss builds it from `RefCodes`
   on the fly via `BuildSpeechTokensBlock`. Format (vendor-identical):
   ```
   user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} {input_phones}<|TEXT_PROMPT_END|>
   assistant:<|SPEECH_GENERATION_START|><|speech_N1|><|speech_N2|>...<|speech_NK|>
   ```
4. **Tokenize** via `TokenizePrompt` (parse_special=true, add_special=false).
   `<|...|>` control tokens resolve to single ids in NeuTTS's Qwen2-extended
   vocab. Two-pass: stack-buffer probe → real allocation. (~2–5 ms for
   the full ~735-token prompt)
5. **Prefill — cached or full**:
   - **Cache hit**: `Memcmp` first N tokens against `Cache->PrefixTokens`
     (guards against context-dependent BPE merges across the prefix /
     middle boundary). On match → `llama_memory_clear` →
     `llama_state_seq_set_data` to restore the snapshot →
     `llama_decode` only the suffix tokens (input_phones + RefCodes
     block).
   - **Cache miss / Memcmp fail / state-API unavailable**: fallback —
     `llama_memory_clear` → `llama_decode` the full prompt.
   - This step is **the dominant cost** of a synth on CPU — linear in
     prompt token count, ~5–15 ms / token of prefill.
6. **Build sampler chain** (matches llama-cpp-python's effective default
   chain — vendor's call relies on these defaults):
   `top_k(50) → top_p(0.95, min_keep=1) → min_p(0.05, min_keep=1) →
    temp(1.0) → dist(seed)`. Each is configurable on `FInoNeuTtsOptions`.
7. **AR loop** (`RunArLoop`) — `llama_sampler_sample` →
   `llama_token_to_piece(special=true)` → `llama_sampler_accept` →
   `llama_decode` of the new single token. Stop conditions:
   - `Next == StopTokenId` (`<|SPEECH_GENERATION_END|>` resolved at
     runner Create time)
   - `llama_vocab_is_eog(Vocab, Next)`
   - `Iter >= MaxNewTokens` (default 2048)
   - cancel flag (atomic, checked every 64 iterations)
   - **All stop checks are skipped while `Generated < MinNewTokens`**
     (default 50) — mirrors vendor's torch path's `min_new_tokens=50`
     guard against rare premature stops.
8. **Regex-parse `<|speech_(\d+)|>`** ids from the accumulated text
   into `TArray<int32>`. UE's `FRegexMatcher` is fine — runs once
   post-AR-loop in the one-shot path; in streaming, `ParseAndAppendSpeechIds`
   runs per generated token (small input each time, no measurable cost).
9. **NeuCodec decode** — `FInoOnnxTensor::CreateFromBufferCopy<int32>({1, 1, N})`
   → `Session->Run` → output is float32 `[1, 1, N_samples]` waveform.
   Shape matches vendor's `np.array(speech_ids, dtype=np.int32)[np.newaxis, np.newaxis, :]`.
10. **PCM conversion** — float32 clamped to `[-1, 1]` → int16 LE via
    `UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono` (round-half-
    away-from-zero). Output: 24 kHz mono int16 PCM little-endian
    `TArray<uint8>` — same contract as Chatterbox so it feeds directly
    into `UStreamingSoundWave::AppendAudioDataFromRAW` (RuntimeAudioImporter).
11. **Marshal back to game thread** via `AsyncTask(ENamedThreads::GameThread)`
    so the dynamic completion delegate fires on the right thread under
    a `TWeakObjectPtr` guard.

### Voice cache — KV-prefix snapshot

The synth prompt has a fixed-per-voice prefix
(`user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} `)
sitting BEFORE the variable input phones. `SetActiveVoiceAsync` walks
`FInoNeuTtsRunner::PrimeVoice` which:

1. Resolves `Voice.RefPhones` (or live-phonemizes `Voice.RefText`,
   normalizing whitespace).
2. Builds `BuildSynthesisPromptPrefix(ResolvedRefPhones)` — byte-
   identical to the prefix portion of the full `BuildSynthesisPrompt`.
3. Tokenizes the prefix → `PrefixTokens`.
4. `llama_memory_clear` + `llama_decode(PrefixTokens)` to prefill the
   prefix into seq 0.
5. `llama_state_seq_get_size` + `llama_state_seq_get_data` to snapshot
   the post-prefix KV state into a flat byte array.
6. Pre-builds the `<|speech_N|>` block string via `BuildSpeechTokensBlock`
   so synths skip 650 `FString::Printf` + concatenations each time.
7. Stores all of the above on `FInoNeuTtsVoiceCache` owned by the runner.

Per-synth cache hit: ~95 % of the prefill cost stays — it's the ~650-token
RefCodes block AFTER the variable input phones that dominates, and that
can't be cached as a prefix (causal attention only). So the **realistic
synth speedup is ~5–10 % of total wall-clock**. The bigger wins are:

- **API ergonomics** — `SetActiveVoiceAsync` once, voice-less synth N
  times.
- **No per-synth speech-tokens-block rebuild** — saves ~10–15 ms per
  synth and 650 transient FString allocations.
- **No per-synth ref-phones re-phonemization** — saves another ~1–5 ms.

The Memcmp guard at synth time defends against context-dependent BPE
merges that could differ between the standalone prefix tokenization
and the full-prompt tokenization. NeuTTS's IPA + special-token prompt
should always match (clean BPE word boundary at the trailing space),
but the guard is cheap insurance — on mismatch we log a warning and
fall back to full prefill without losing correctness.

`InoLlama` vtable entries this depends on: `llama_state_seq_get_size`
/ `_get_data` / `_set_data` and `llama_memory_seq_rm`. If those resolve
to nullptr on a given `llama.dll` (older builds), `PrimeVoice` errors
out cleanly and synth runs the full-prefill path.

### Why no per-voice synth overload

Earlier the subsystem had `SynthesizeAsync(Text, Voice, Options, ...)`
and a separate voice-less variant for the active voice. We dropped the
per-voice overload in commit `fac495d` because:

- It invited callers to thread `Voice` through every synth call, which
  defeats the prime-once intent.
- API surface was twice as wide for no upside.
- Switching voices is a deliberate operation (~200–600 ms re-prime);
  hiding it behind a per-call argument was misleading about cost.

To do an "occasional one-off synth in a different voice without
disturbing the primed voice", call `SetActiveVoiceAsync` twice (other
→ original) and accept the 2× re-prime cost. This case is niche; the
common case is "one character, one voice, many utterances".

### Streaming synth

`SynthesizeStreamAsync` interleaves the AR loop with rolling decoder
runs to drop first-audio latency. Constants match vendor's
`_infer_stream_ggml`:

| Const | Value | Meaning |
|---|---|---|
| `kCodecHopLength` | 480 | 50 Hz token rate at 24 kHz output |
| `kOverlapFrames`  | 1   | overlap frames around each chunk |
| `kLookforward`    | 5   | tokens past the chunk's right edge |
| `kLookback`       | 50  | tokens before the chunk's left edge |
| `ChunkTokens`     | 25  | new tokens per emit (= 0.5 s audio) |

**Decode-threshold off-by-one fix vs vendor.** Vendor's threshold is
`>= ChunkTokens + kLookforward` (== 30) but the actual decode window
extends to `n_decoded_tokens + ChunkTokens + kLookforward + kOverlapFrames`
(== 31). Python's slice silently clamps; C++ TArrayView reads past the
end into uninitialized memory. Our threshold is `>= ChunkTokens +
kLookforward + kOverlapFrames` (== 31) — wait one extra token before
the first decode, but always have a valid window. Comment in
`InoNeuTtsSynthesisWorker.cpp` flags this for future readers.

**Linear overlap-add** (`LinearOverlapAdd`, port of vendor's
`_linear_overlap_add` with `power=1.0`) blends each chunk's edge into
its neighbours via a triangular weight `(0.5 - |t - 0.5|)`. Output
samples are weighted-summed and divided by the running weight sum, so
overlapping regions average to unit gain rather than clipping.

Each emitted chunk goes through `OnAudioChunk(Bytes, bIsFinal)` on
the game thread. The final irregular chunk fires with `bIsFinal=true`
before `OnComplete` delivers the concatenated full waveform.

### Audit pass / vendor alignment

Subsequent to the initial port, a structured audit cross-checked
every step against `vendor/neutts/neutts.py` + `phonemizers.py`. Result
(committed in `c794fc6`): one HIGH correctness bug + four worthwhile
improvements landed, no blockers remained.

| What | Status |
|---|---|
| Prompt template byte-identical to vendor | ✅ |
| Tokenization flags match | ✅ |
| Streaming chunking constants + threshold off-by-one fix | ✅ |
| Linear overlap-add power=1 | ✅ |
| NeuCodec input shape `[1, 1, N]` int32 | ✅ |
| Voice prime / restore correctness (Memcmp guard, KV trim) | ✅ |
| **Sampler chain — added top_p(0.95) + min_p(0.05)** | ✅ fixed |
| **Phoneme whitespace normalization** | ✅ fixed |
| **min_new_tokens=50 robustness** | ✅ fixed |
| **Speech-tokens block cached at PrimeVoice** | ✅ fixed (perf) |
| **Backbone warmup at LoadModel** | ✅ fixed (perf) |
| Watermark (vendor's `perth.PerthImplicitWatermarker`) | ❌ omitted by design (Python+PyTorch only) |

### Project Settings

`UInoNeuTtsNativeSettings` (Project Settings → Plugins → Ino NeuTTS
Native, `Config = Game`):

- `BackboneModels` (`TArray<FInoNeuTtsBackboneEntry>`) — every NeuTTS
  backbone you want available. Each entry: `DisplayName`,
  `DownloadUrl`, `LocalFileName`, `ExpectedSha256`, `FileSizeBytes`,
  `Language` (eSpeak code), `Quantization` (informational).
- `DecoderModels` (`TArray<FInoNeuTtsDecoderEntry>`) — same shape minus
  the language fields. Same NeuCodec decoder works for every backbone.

Files land at
`<FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/<LocalFileName>`.

A `+BackboneModels=…` array seed is committed in
`Config/DefaultGame.ini` so the smoke tests have something to download.

### Public API surface (subsystem)

`UInoNeuTtsSubsystem` (UGameInstanceSubsystem; all public methods on
the game thread, all delegates fire on the game thread):

**Model lifecycle**
- `void LoadModelAsync(Config, OnLoaded, OnDownloadProgress)` —
  download (if needed) + ThreadPool-load. `OnDownloadProgress` is a
  single-cast `FInoNeuTtsDownloadProgressDelegate(const FInoDownloadProgress&)`
  carrying the shared InoNodes progress struct.
- `void DownloadModelAsync(Config, OnComplete, OnDownloadProgress)` —
  download only, no load. Pre-stage at startup or drive a separate UI.
- `bool IsModelDownloaded(Config) const` — pure file-stat check
  (existence + non-zero size). UMG-safe.
- `void UnloadModel()` — drop runner. Safe mid-synth (worker holds a
  TSharedPtr).
- `bool IsModelLoaded() const`

**Active voice**
- `void SetActiveVoiceAsync(Voice, OnReady)` — phonemize +
  prefill-snapshot the prefix off-thread. Required before synth.
- `void ClearActiveVoice()`
- `bool HasActiveVoice() const` / `FString GetActiveVoiceName() const`

**Synthesis**
- `void SynthesizeAsync(Text, Options, OnComplete)` — uses active voice.
- `void SynthesizeStreamAsync(Text, Options, ChunkTokens, OnAudioChunk, OnComplete)` —
  streaming variant. `ChunkTokens=0` uses the default of 25.
- `void CancelSynthesis()` — cooperative abort. Fires `OnComplete` with
  `bSuccess=false / ErrorMessage="Cancelled"`.
- `bool IsSynthInFlight() const`

**Voice loading**
- `bool LoadVoiceFromFile(FilePath, OutVoice)` — parse one
  `.nvoice.json` into a `FInoNeuTtsVoice`.
- `TArray<FInoNeuTtsVoice> ListBundledVoices()` — scan
  `Plugins/InoAgents/NeuTTS/voices/`.

**Audio format helpers (Pure)**
- `int32 GetSampleRate()` — 24000.
- `int32 GetNumChannels()` — 1.

**Blueprint async-action wrappers** under `Public/`:
- `UInoNeuTtsSynthesize` — one-shot. Pins: `OnComplete`, `OnError`.
- `UInoNeuTtsStreamSynthesize` — streaming. Pins: `OnAudioChunk`,
  `OnComplete`, `OnError`. Both consume the active voice; no Voice pin.

### Configuration structs

`FInoNeuTtsConfig` (passed to `LoadModelAsync` / `DownloadModelAsync`
/ `IsModelDownloaded`):
- `BackboneModelName` — Project Settings entry's DisplayName, "" =
  first entry.
- `DecoderModelName` — same shape for the decoder array.
- `Backbone` — `FInoLlamaModelParams` (mmap, mlock, GPU offload count,
  vocab-only, etc — see `InoLlama`).
- `BackboneContext` — `FInoLlamaContextParams` (n_ctx, batch sizes,
  threads, flash attention, KV dtype).
- `DecoderOnnx` — `FInoOnnxSessionOptions` (CPU vs DirectML / NNAPI /
  WebGPU, thread counts, graph optimization, etc).
- `bWarmupDecoderOnLoad` (default true) — tiny dummy ONNX inference at
  load time to pay JIT / kernel-selection / mem-pattern setup once.
- `bWarmupBackboneOnLoad` (default true) — tiny 1-token prefill +
  decode through the GGUF at load time, then `llama_memory_clear`. Pays
  cold-start kernel JIT / KV allocation jitter at load instead of on
  first synth.

`FInoNeuTtsOptions` (passed to each synth):
- `MaxNewTokens` (default 2048) — hard cap on AR-loop iterations.
- `MinNewTokens` (default 50) — skip stop-token / EOG checks until at
  least this many tokens emitted. Matches vendor's torch
  `min_new_tokens=50` guard against rare premature stops.
- `Temperature` (default 1.0), `TopK` (default 50), `TopP` (default
  0.95), `MinP` (default 0.05) — sampler chain knobs. Defaults match
  llama-cpp-python's effective chain when only temperature + top_k are
  overridden by vendor.
- `RandomSeed` (default -1) — `-1` = fresh time-based seed.

### Voice format — `.nvoice.json`

```json
{
  "Name":      "jo",
  "Language":  "en-us",
  "RefText":   "...transcript of the source WAV...",
  "RefPhones": "h@l'oU D'e@ ...",       // optional pre-bake; empty = phonemize lazily
  "RefCodes":  [5234, 7891, 4099, ...]   // 50 Hz FSQ speech tokens, ~650 for a 13 s ref
}
```

Generated offline by
`Plugins/InoAgents/NeuTTS/scripts/build-voices.py` (PyTorch +
neucodec + phonemizer; uses vendor's `samples/*.{pt,txt}` as input).
Five voices ship under `voices/`: `jo`, `dave`, `greta`, `juliette`,
`mateo`.

Voices are NOT in the model registry — they're lightweight JSON
artefacts (~10 KB each) that ship with the plugin. Future work:
runtime voice cloning via NeuCodec ONNX encoder (currently
PyTorch-only; would need an ONNX export).

### Why phonemization is offline-only-by-default

NeuTTS was trained on IPA phonemes (from espeak-ng). The standard
runtime phonemizer is espeak-ng itself, which is **GPLv3** — a
copyleft dependency that would poison commercial games shipping
NeuTTS via static linkage. We sidestep this by:

- Pre-baking `RefPhones` offline via the build-voices script so
  voice JSONs ship phonemized (zero runtime espeak dependency for the
  reference text path).
- Wrapping espeak-ng in a separate **`InoSpeakNG`** plugin that links
  espeak-ng as a runtime DLL/.so (dynamic linkage, GPLv3 still applies
  to that plugin in isolation but the linkage interface is compatible
  with CCG sec. 4 / LGPL-style dynamic-link carve-out for proprietary
  application code).

Input text is still phonemized at runtime via InoSpeakNG. v2 follow-up:
ship an MIT-licensed ONNX G2P (~5 MB) consumed via the existing
`FInoOnnxSession` layer, and drop the InoSpeakNG runtime dependency
entirely.

### File layout

```
Plugins/InoAgents/
├── NeuTTS/                                          ← NeuTTS-specific assets / vendor / scripts
│   ├── vendor/                                      ← git submodule, neuphonic/neutts pinned
│   │   ├── neutts/{neutts.py, phonemizers.py}       ← canonical reference inference loop
│   │   ├── examples/                                ← upstream Python examples
│   │   └── samples/{jo, dave, ...}.{wav, txt, pt}   ← reference voices source
│   ├── voices/{jo, dave, greta, juliette, mateo}.nvoice.json
│   ├── scripts/build-voices.py                      ← offline encoder (PyTorch + neucodec)
│   └── models/                                      ← optional dev-time drop (gitignored)
│
└── Source/InoNeuTtsNative/
    ├── InoNeuTtsNative.Build.cs                     ← module deps (InoLlama, InoOnnx,
    │                                                   InoSpeakNG, InoNodes, InoAgents,
    │                                                   RuntimeAudioImporter, Json,
    │                                                   DeveloperSettings)
    │
    ├── Public/
    │   ├── InoNeuTtsNative.h                        ← module interface
    │   ├── InoNeuTtsLog.h                           ← LogInoNeuTts category
    │   ├── InoNeuTtsTypes.h                         ← USTRUCTs (Voice, Config, Options,
    │   │                                              Result), single-cast delegates,
    │   │                                              multicast async-action delegates
    │   ├── InoNeuTtsSettings.h                      ← UInoNeuTtsNativeSettings + entry structs
    │   ├── InoNeuTtsSubsystem.h                     ← UInoNeuTtsSubsystem (the public API)
    │   ├── InoNeuTtsSynthesize.h                    ← BP async action: NeuTTS Synthesize
    │   └── InoNeuTtsStreamSynthesize.h              ← BP async action: NeuTTS Synthesize Streaming
    │
    └── Private/
        ├── InoNeuTtsNative.cpp                      ← thin module lifecycle
        ├── InoNeuTtsCommon.{h,cpp}                  ← path resolvers (BackboneModels +
        │                                               DecoderModels lookups), shared
        │                                               TokenizePrompt, NormalizePhones
        ├── InoNeuTtsSettings.cpp
        ├── InoNeuTtsRunner.{h,cpp}                  ← FInoNeuTtsRunner: model + ctx +
        │                                               vocab + decoder + voice cache
        │                                               (PrimeVoice, ClearVoiceCache,
        │                                               HasCachedVoice, GetVoiceCache)
        ├── InoNeuTtsPromptBuilder.{h,cpp}           ← BuildSynthesisPrompt (RefCodes +
        │                                               String overloads),
        │                                               BuildSynthesisPromptPrefix,
        │                                               BuildSpeechTokensBlock
        ├── InoNeuTtsSynthesisWorker.{h,cpp}         ← RunSynthesis + RunStreamingSynthesis
        │                                               free functions; NOT an FRunnable —
        │                                               called directly from a ThreadPool
        │                                               worker dispatched by the subsystem
        ├── InoNeuTtsVoiceRegistry.{h,cpp}           ← .nvoice.json parser + bundled-voice scan
        ├── InoNeuTtsSubsystem.cpp                   ← Blueprint glue + async-load chain +
        │                                               InoNodes::Download::DownloadFilesAsync
        │                                               wiring + active-voice state
        ├── InoNeuTtsSynthesize.cpp                  ← BP async action impl
        ├── InoNeuTtsStreamSynthesize.cpp
        └── SmokeTests/
            ├── InoNeuTtsLoadTest.cpp
            ├── InoNeuTtsSynthTest.cpp
            ├── InoNeuTtsStreamSynthTest.cpp
            └── InoNeuTtsVoiceRegistryTest.cpp
```

The smoke tests bypass the subsystem and call `FInoNeuTtsRunner::Create`
+ `InoNeuTtsNative::RunSynthesis` / `RunStreamingSynthesis` directly so
they work outside PIE.

### Smoke tests

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.NeuTts.LoadTest [backbone DisplayName]` | Resolves Project Settings → loads GGUF + ONNX + decoder warmup + backbone warmup → logs description, stop-token id, and decoder I/O. | no |
| `Ino.NeuTts.SynthTest [voice=jo] [backbone] [text...]` | Full pipeline (load → live phonemize → synth → save WAV at `Saved/InoNeuTtsTest.wav`). Logs RTF + sample count. | no |
| `Ino.NeuTts.StreamSynthTest [voice=jo] [backbone] [chunk_tokens=25] [text...]` | Streaming variant: per-chunk arrival timing log + TTFA + final WAV at `Saved/InoNeuTtsStreamTest.wav`. | no |
| `Ino.NeuTts.VoiceRegistryTest` | Scans `Plugins/InoAgents/NeuTTS/voices/`; logs every parsed `.nvoice.json` with code count + lang. | no |

### Performance notes (CPU, alderlake-class)

- Model load: **~1.5 s warm** (backbone ~0.3 s + ORT codec ~1.2 s).
  Cold-disk add ~1–3 s for the OS file cache.
- **Backbone warmup** at load adds ~50–200 ms but removes the same
  jitter from the first user-visible synth.
- LM throughput (Q4 GGUF, CPU): **~120 tok/s on Nano**, **~30–40 tok/s
  on Air**.
- NeuCodec decoder: **~70 ms per second of audio**.
- Full synth, Nano-Q4, ~45-char input + ~650-code reference + 2.5 s
  audio: **~10 s wall-clock, RTF 0.25×**. Dominant cost is prefill,
  linear in `RefCodes.Num()`.
- **Streaming TTFA**: ~30 tokens × per-token-ms + first decoder run ≈
  ~0.5–1.0 s after `SynthesizeStreamAsync` is enqueued, depending on
  prompt size.

Vulkan offload via `Backbone.NumGpuLayers > 0` is available on hosts
with a working Vulkan driver (registered at module startup by InoLlama)
but currently untested. Set to 999 to "offload all" — same field works
for both Nano and Air.

### Roadmap

| Tracked | Notes |
|---|---|
| ONNX G2P phonemizer (drop InoSpeakNG runtime dep) | MIT-licensed ~5 MB, would unblock pure-permissive shipping |
| Runtime voice cloning | needs NeuCodec ONNX encoder export (currently PyTorch-only) |
| Multi-voice cache | hold N voice snapshots simultaneously, identified by Voice.Name; useful for multi-character demos |
| Watermarking | vendor's `perth` is Python+PyTorch; native port not pursued |

## UE-side integration architecture

The UE-facing API lives under `Source/InoAgents/Public/` (Blueprint-visible types) with mirroring private impl under `Source/InoAgents/Private/`. LLM-facing types are prefixed `LiteRtLm` rather than `InoAgents` on purpose — future versions of this plugin may host multiple LLM backends (OpenAI, Anthropic, llama.cpp) and each backend's classes live in their own subdirectory. Naming the classes after the backend from day one makes the boundary explicit. The same pattern applies to the other subsystems: `Chatterbox*` for the on-device TTS, `ElevenLabs*` for the cloud TTS, `*ChatPanel*` for the dev chat UI, etc.

There is **no "agent component"** that bundles everything together. Earlier drafts of this file described a `UInoLiteRtLmAgentComponent` + `UInoAgentsStreamingAudioComponent` + `UInoLiteRtLmDialogueQueue` trio that wired LLM tokens directly into a TTS playback queue; that scaffolding was removed in favour of letting Blueprint / C++ callers wire the subsystems together themselves (the demo project's character actor is the integration point). What remains is a set of independently-useful subsystems and helpers, listed below.

```
Blueprint / C++ ─┬─ UInoLiteRtLmSubsystem            (UGameInstanceSubsystem)
                 │     Owns LiteRtLmEngine*, tool registry, the in-PIE chat
                 │     panel. LoadModelAsync auto-downloads + SHA-256-verifies
                 │     models from URLs in UInoAgentsSettings; a chunked
                 │     range-based download (500 MB chunks) keeps multi-GB
                 │     downloads inside TArray<uint8>'s int32 size limit.
                 │     Single-conversation invariant — see the header.
                 │     OnDownloadProgress fires during download.
                 │
                 ├─ UInoLiteRtLmConversation         (UObject, BlueprintType)
                 │     One stateful chat with the loaded engine. Owns one
                 │     native LiteRtLmConversation* plus a pinned worker
                 │     thread (FInoLiteRtLmConversationWorker). Multicast
                 │     delegates fire on the game thread, in order:
                 │       OnUserMessage(Text)              — synchronous echo
                 │       OnToken(RawText, CleanText)      — per chunk
                 │       OnSentence(RawText, CleanText)   — per split boundary
                 │       OnSentenceBoundary()             — split signal (no payload)
                 │       OnToolCalled(Name, ArgsJson, ResultJson) — diagnostic
                 │       OnComplete(FullText)             — terminal (success)
                 │       OnError(ErrorMessage)            — terminal (failure)
                 │     Configurable bitmasks:
                 │       SentenceSplitFlags — newline + which punctuation+space
                 │                            triggers OnSentence (default: \n,
                 │                            ". ", ", ", "? ", "! ")
                 │       TagStripFlags      — which delimiter pairs get stripped
                 │                            from CleanText (default: [...] {...})
                 │     Per-turn context injection (NOT in chat history):
                 │       SetSystemContext / SetUserContext (+ Add/Get/Clear)
                 │       merged into the user message via [Context]/[/Context]
                 │       tags by BuildMergedContext — see the Gemma 4 chat-template
                 │       limitation around extra_context for why this isn't a
                 │       template var.
                 │     Lifecycle: Cancel (abort in-flight stream),
                 │       IsStreamingInFlight, Shutdown (deterministic teardown,
                 │       safe inside a delegate handler — unlike CollectGarbage),
                 │       SubmitDeferredToolResult (header-only stub, see "Tool
                 │       calling flow" below).
                 │
                 ├─ FInoLiteRtLmModelConfig         (USTRUCT, BlueprintType)
                 │     Plain struct (NOT a UDataAsset). Fields:
                 │       ModelFileName — resolved via LiteRtLmResolveModelPath:
                 │         1. PersistentDownloadDir/InoAgents/Models/ (cached)
                 │         2. Plugins/InoAgents/Models/ (legacy dev drop)
                 │         3. auto-download from UInoAgentsSettings URL
                 │       Backend (Cpu / Gpu), MaxNumTokens, SystemMessage
                 │
                 ├─ UInoLiteRtLmToolBase            (Blueprintable abstract UObject)
                 │     Subclass, set ToolName/Description/Parameters, override
                 │     Execute(ArgsJson)→ResultJson. Schema is built automatically
                 │     from the properties via BuildSchemaJson.
                 │     UInoLiteRtLmAddNumbersTool ships as the canonical example
                 │     (used by Ino.LiteRtLm.ConversationToolTest).
                 │
                 ├─ UInoChatterboxTurboNativeSubsystem      (UGameInstanceSubsystem)
                 │     On-device TTS. Owns the 4 ORT sessions
                 │     (speech_encoder / embed_tokens / language_model /
                 │     conditional_decoder) plus the GPT-2 BPE tokenizer.
                 │     LoadModelsAsync auto-downloads missing files (sequential
                 │     HEAD-probe + GET, .partial staging, atomic rename) and
                 │     fires OnDownloadProgress.
                 │     SynthesizeAsync — one-shot, OnComplete with full waveform.
                 │     SynthesizeStreamAsync — re-runs conditional_decoder every
                 │     N AR tokens (default 20) and fires OnAudioChunk on the
                 │     game thread for each delta, then OnComplete with the
                 │     concatenated waveform.
                 │     Output: 24 kHz mono int16 PCM little-endian bytes — feed
                 │     directly into UStreamingSoundWave::AppendAudioDataFromRAW
                 │     (RuntimeAudioImporter) or save via SaveInt16PcmAsWav.
                 │     CancelSynthesis cooperatively aborts queued + in-flight.
                 │
                 ├─ UInoChatterboxTurboNativeStreamSynthesize  (UBlueprintAsyncActionBase)
                 │     Latent Blueprint node ("Chatterbox Stream Synthesize")
                 │     that wraps SynthesizeStreamAsync with three exec pins:
                 │     OnAudioChunk / OnComplete / OnError.
                 │
                 ├─ UInoElevenLabsSubsystem         (UGameInstanceSubsystem)
                 │     Caches settings (API key, base URL, default model id,
                 │     default output format) at PIE start; anchors live HTTP
                 │     actions via a UPROPERTY TSet so they survive GC; fires
                 │     CancelAll() at Deinitialize so post-PIE responses can't
                 │     dispatch into freed UObjects.
                 │
                 ├─ UInoElevenLabsTextToDialogueStream  (UBlueprintAsyncActionBase)
                 │     Latent node for POST /v1/text-to-dialogue/stream.
                 │     OnAudioChunk (delta bytes) / OnComplete (full bytes) /
                 │     OnError. Multiple output formats via
                 │     EInoElevenLabsOutputFormat (MP3 / PCM / u-law).
                 │     Caller owns playback — there is no built-in audio
                 │     component; feed the bytes into RuntimeAudioImporter
                 │     or your own audio pipeline.
                 │
                 ├─ UInoAgentsSettings              (UDeveloperSettings)
                 │     Project Settings → Plugins → InoAgents. Three sections:
                 │       ElevenLabs : ApiKey, BaseUrl, DefaultModelId,
                 │                    DefaultOutputFormat
                 │       LiteRT-LM  : Models — array of {DisplayName, FileName,
                 │                    DownloadUrl, ExpectedSha256}
                 │       Chatterbox : ChatterboxModels — array of {DisplayName,
                 │                    Variant, HuggingFaceRepoUrl, Revision}
                 │
                 ├─ Slate chat panel                 (dev / debug UI)
                 │     SInoChatPanel + UInoChatBridge. Bridge holds a UPROPERTY
                 │     ref to the conversation, owns the UFUNCTION handlers
                 │     bound via AddDynamic, and forwards events into the panel
                 │     via TWeakPtr. Subsystem methods ShowChatPanel /
                 │     HideChatPanel + console commands
                 │     Ino.LiteRtLm.ShowChatPanel / Ino.LiteRtLm.HideChatPanel
                 │     drive it. Wraps the panel inside a viewport widget;
                 │     PIE-end auto-hides via the PrePIEEnded hook.
                 │
                 ├─ UInoAnimationBlueprintHelper    (UBlueprintFunctionLibrary)
                 │     Stateless animation helpers exposed for the demo
                 │     character:
                 │       CalculateGaze — merged eye + head tracking. Returns
                 │         ARKit-style eye blend shape weights AND a local-space
                 │         head FRotator, with a three-state attention machine
                 │         (Tracking / EyeGlance / FullGlance) for natural
                 │         periodic look-away behaviour. Two bApply* flags let
                 │         the caller decide whether to use the outputs or fall
                 │         back to the base animation for eyes and head
                 │         independently.
                 │       CalculateBlinkWeight — procedural blink state machine:
                 │         random intervals with occasional long "stare" pauses,
                 │         asymmetric ease-in close / ease-out open, double
                 │         blinks, SpeakingIntensity input to accelerate rate
                 │         during speech.
                 │     Pass the previous frame's output back as PreviousState /
                 │     PreviousResult for frame-rate-independent smoothing.
                 │
                 └─ UInoAudioFunctionLibrary        (UBlueprintFunctionLibrary)
                     GenerateEmptyRawAudio (silent PCM in any
                     ERuntimeRAWAudioFormat — note that unsigned PCM uses the
                     midpoint as silence, not zero); GenerateDitheredSilence
                     (low-amplitude white noise so neural lip-sync models stay
                     in their training distribution during pauses, default
                     ~-76 dBFS); SaveInt16PcmAsWav (write PCM bytes + RIFF
                     header — useful for verifying Chatterbox output).
                            │
                            ▼
                  FInoLiteRtLmConversationWorker (FRunnable, one per conversation)
                            │   Owns the native LiteRtLmConversation + config.
                            │   Multi-round agent loop. Marshals every delegate
                            │   broadcast back to the game thread via AsyncTask.
                            ▼
                  LiteRtLm.dll  (pure C API — litert_lm_conversation_*)
```

### Threading model (non-negotiable)

- **`StartupModule` never blocks on model load.** Gemma 4 E2B is ~3.2 GB; synchronous load would freeze the editor for 5–30 seconds. `LoadModelAsync` dispatches via `Async(EAsyncExecution::ThreadPool, ...)`, calls `litert_lm_engine_create` there, and marshals the `FOnInoLiteRtLmModelLoaded` delegate back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`. A `TWeakObjectPtr<UInoLiteRtLmSubsystem>` guards against the subsystem being torn down while the load is in flight.
- **Inference never runs on the game thread.** Every `UInoLiteRtLmConversation` owns an `FInoLiteRtLmConversationWorker` (an `FRunnable` on a dedicated `FRunnableThread`). Messages enter the worker via a `TQueue<FString, EQueueMode::Spsc>` whose producer is `EnqueueMessage` on the game thread. The worker calls `litert_lm_conversation_send_message_stream` which itself is non-blocking — it returns immediately and fires the C callback from LiteRT-LM's own internal thread. The worker uses two `FEvent`s:
  - **QueueEvent** (auto-reset) — wakes the worker thread when a new message is enqueued or `Stop` is called.
  - **StreamEvent** (manual-reset) — signalled by the stream callback when a round reaches `is_final` or errors. The worker thread blocks on this inside `RunOneStreamRound` to serialise rounds within a send.
- **Tokens marshal back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.** The static C callback never touches `UObject` state directly — it copies `chunk` / `error_msg` into `FString`s (which own their storage), dispatches an `OnToken` broadcast via `AsyncTask`, and then signals `StreamEvent` for terminal callbacks. The worker thread's `ProcessMessage` eventually dispatches the terminal `OnComplete` / `OnError` via the same `AsyncTask` pattern, so observers always see `OnToken`s in order followed by exactly one terminal broadcast.
- **TUniquePtr<FInoLiteRtLmConversationWorker> ordering.** Because the worker is a forward-declared type in the public `UInoLiteRtLmConversation` header, UHT's generated `.gen.cpp` emits both the default constructor and the `FVTableHelper` hot-reload helper constructor inline. Both must be declared out-of-line in the header and defined in `InoLiteRtLmConversation.cpp` (where `InoLiteRtLmConversationWorker.h` is fully included) so the `TDefaultDelete<FInoLiteRtLmConversationWorker>` deleter instantiation lands in a TU with the complete type. Leaving any of them implicit produces C4150 "delete of pointer to incomplete type" — see the comments at the top of the class in `InoLiteRtLmConversation.h`.
- **One worker per conversation.** LiteRT-LM conversations are stateful (KV cache) and not thread-safe. Concurrent conversations mean multiple native conversations, each with its own pinned worker thread. LiteRT-LM also appears to reject creating a second native conversation on the same engine while a prior one is still alive, so tests that run back-to-back must call `Conversation->Shutdown()` (synchronous worker teardown) before constructing the next one. `CollectGarbage` from inside a delegate handler is NOT a valid substitute — parallel GC workers racing the in-flight delegate's write access trigger `FMRSWRecursiveAccessDetector` ensure fires.
- **Never call inference from `Tick`.** Not even once.

### Tool calling flow

1. A Blueprint or C++ class subclasses `UInoLiteRtLmToolBase`, sets `ToolName`, `Description`, and `Parameters` (array of `FInoLiteRtLmToolParameter`), and overrides `Execute(FString ArgumentsJson) → FString ResultJson`. The base class builds the OpenAI-style function-call JSON schema automatically from these properties via `BuildSchemaJson()`.
2. `UInoLiteRtLmSubsystem::RegisterTool` validates the schema built from the tool's properties and checks that `function.name` matches `ToolName` before storing the tool in its internal `TMap<FName, TObjectPtr<UInoLiteRtLmToolBase>>`. Unparseable schemas are rejected with a clear error log.
3. `UInoLiteRtLmSubsystem::CreateConversation` calls `BuildToolsJsonForConversation` which serialises every registered tool's schema into a JSON array via `FJsonSerializer::Serialize` with `TCondensedJsonPrintPolicy`. The conversation config is built by `litert_lm_conversation_config_create()` (no-arg) and populated via `litert_lm_conversation_config_set_tools(...)` + `litert_lm_conversation_config_set_enable_constrained_decoding(..., true)`. When no tools are registered, neither setter is called and the conversation behaves as a plain chat.
4. When the model emits a tool call, LiteRT-LM delivers the chunk to the static C callback as an **OpenAI-compatible** envelope with `tool_calls` at the **top level** of the assistant message (NOT as a `content[*]` part):
   ```json
   {"role":"assistant",
    "tool_calls":[
      {"type":"function",
       "function":{"name":"add_numbers","arguments":{"a":27,"b":15}}}]}
   ```
   `OnStreamChunk` walks `content[*]` for text parts AND the top-level `tool_calls[*]` independently, so a single chunk can in principle carry either or both.
5. Tool-call entries are re-serialised (arguments sub-object → compact JSON string) and queued into `StreamPendingToolCalls`. Text from tool-call rounds is suppressed — `OnToken` only sees tokens from the final text-producing round, never intermediate tool-call JSON.
6. When the round's `is_final` callback fires, the worker thread wakes from `StreamEvent->Wait`, sees non-empty `StreamPendingToolCalls`, and runs `ExecuteToolSynchronously` for each. That method:
   - Allocates a pooled `FEvent` (auto-reset).
   - Dispatches an `AsyncTask` to the game thread that looks up the tool via `Subsystem->FindTool(ToolName)`, calls `Tool->Execute(ArgsJson)` inside a try/catch, and triggers the `FEvent`.
   - Blocks on the `FEvent`. The game thread is never blocked because the outer `SendMessageAsync` is already async.
   - Returns the result JSON string (or a `"\"ERROR: ...\""` literal if the tool was missing / the subsystem was GC'd / Execute threw).
7. The worker builds a single `{"role":"tool","content":[{"type":"tool_response",...}, ...]}` message bundling every executed tool's result, sends that via a fresh `litert_lm_conversation_send_message_stream` on the **same** native conversation (reusing the KV cache), and loops back to round N+1.
8. Eventually a round produces final text with no tool calls. The worker dispatches `OnComplete` with the accumulated text of **that round only** — callers never see intermediate tool-call rounds. `OnToolCalled` fires once per tool execution on the game thread, strictly before the terminal `OnComplete`, as a diagnostic.
9. A safety cap (`kMaxAgentLoopRounds = 8`) bounds the loop. Hitting it dispatches `OnError("Agent loop exceeded N rounds...")` rather than spinning forever.

**Why this is not a deadlock trap.** Tools are executed via AsyncTask on the game thread while the worker blocks on an `FEvent`. The game thread itself is not blocked — `SendMessageAsync` has already returned control to the caller, so the game thread is free to run ticks, process more AsyncTasks, and eventually execute the tool. The worker wakes up when the tool is done.

**Deferred tool results.** `UInoLiteRtLmConversation::SubmitDeferredToolResult` is declared in the public API but currently stubbed — it logs a warning and is a no-op. A future update will wire it through the worker's agent loop so tools that need to do their own async work (network, disk I/O, user confirmation dialogs) can unblock the worker with a fresh result later. The method exists in the header now so Blueprint consumers can wire it up ahead of the implementation landing.

## Smoke tests

Development-time console commands. Two groups: the Phase 1 group exercises the native C API directly (no UObjects), and the UE API group exercises the UE-facing API surface end-to-end through PIE. Both groups stay in the codebase so a regression in either layer can be diagnosed without the other being a suspect.

Both groups live in `Source/InoAgents/Private/SmokeTests/`, one file per command, and register themselves as `FAutoConsoleCommand` globals at file scope so they become available the moment the module's DLL loads.

Invoke from the editor's Output Log command input. UE API tests require **PIE** (the subsystem is a `UGameInstanceSubsystem`), Phase 1 tests do not.

### Phase 1 — native C API layer (no UE API)

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.LoadEngineTest` | LiteRT-LM engine can be constructed and destroyed without crashing. | no |
| `Ino.GenerateTest [prompt]` | Raw text generation via `session_generate_content` (no chat template). | no |
| `Ino.ConversationTest [prompt]` | Chat-template API via `conversation_send_message` actually follows instructions. | no |
| `Ino.ToolCallTest [prompt]` | Full tool-calling agent loop at the raw C API layer: user prompt → model emits tool call → we execute inline → tool result → final answer. Uses a local `add_numbers(a,b)` helper directly, NOT the UE API `UInoLiteRtLmAddNumbersTool`. | no |
| `Ino.StreamTest [prompt]` | Non-blocking streaming via `generate_content_stream` with worker→game-thread marshaling through `AsyncTask`. First non-blocking smoke test. | no |

All Phase 1 tests except `StreamTest` are synchronous (freeze the editor for 2–15 s). They resolve the default model at `Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm` via `InoSmokeTest::ResolveDefaultModelPath()` and call the LiteRT-LM C API directly — no UObjects, no subsystem, no conversations. Their purpose is to prove the native integration works independently of the UE API layer.

### UE-facing LiteRT-LM API

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.LiteRtLm.SubsystemLoadTest` | `UInoLiteRtLmSubsystem::LoadModelAsync` dispatches to a ThreadPool worker, marshals `FOnInoLiteRtLmModelLoaded` back to the game thread, and `IsModelLoaded` reports true afterward. Non-blocking. | **yes** |
| `Ino.LiteRtLm.ConversationSendTest` | `UInoLiteRtLmConversation` round-trips a non-streaming "What is 2 plus 2?" prompt through the worker's agent loop (streaming internally) and delivers the full accumulated text via `OnComplete`. | **yes** |
| `Ino.LiteRtLm.ConversationStreamTest [prompt]` | Streaming surface: binds `OnToken` in addition to `OnComplete` and logs each chunk with per-stream elapsed time. Cross-checks that the locally-accumulated tokens match the `FullText` delivered to `OnComplete`. | **yes** |
| `Ino.LiteRtLm.ToolRegistryTest` | Registry-only check (no model load): constructs a `UInoLiteRtLmAddNumbersTool`, registers it, looks it up, serialises `BuildToolsJsonForConversation`, invokes `Execute_Execute` via the BlueprintNativeEvent wrapper, unregisters, and verifies `FindTool` returns null. Fastest tool smoke test; useful as a pre-flight before running the full agent loop. | **yes** |
| `Ino.LiteRtLm.ConversationToolTest [prompt]` | **The headline test.** Registers a `UInoLiteRtLmAddNumbersTool`, creates a conversation with `tools_json` + constrained decoding, binds all four delegates (`OnToken` / `OnToolCalled` / `OnComplete` / `OnError`), sends "What is 27 plus 15?", watches the multi-round agent loop run, and logs PASS if `OnToolCalled` fired with `add_numbers` + result `"42"` AND `OnComplete`'s text contains `"42"` or `"forty-two"`. | **yes** |
| `Ino.LiteRtLm.ConversationContextTest` | Exercises `SetSystemContext` / `SetUserContext` end-to-end: injects game state (location, time) and player state (name, class) into the conversation, sends a prompt requiring the context, and checks that the model's response references the injected values. Validates the context → user message prepend pipeline. | **yes** |
| `Ino.LiteRtLm.ShowChatPanel [tools]` / `Ino.LiteRtLm.HideChatPanel` | Brings up / tears down the in-PIE Slate chat panel (`SInoChatPanel` + `UInoChatBridge`). Optional `tools` arg pre-registers `UInoLiteRtLmAddNumbersTool` so the panel can exercise the agent loop. Useful for interactive smoke testing — type messages, watch streaming tokens fill the bubble, see tool-call pills render. | **yes** |

### Chatterbox (TTS) API

All under `Source/InoAgents/Private/SmokeTests/InoChatterboxTurboNativeTest.cpp`, `InoChatterboxTurboNativeSubsystemTest.cpp`, and `InoChatterboxTurboNativeStreamSynthTest.cpp`. The granular tests stage on intermediate steps so a regression at any layer of the pipeline is bisectable without running a full synth.

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.Chatterbox.LoadModelsTest` | Loads the 4 ORT sessions + the GPT-2 tokenizer for a variant (no synthesis). Confirms session creation under whatever DML / CPU routing is in effect. | yes |
| `Ino.Chatterbox.TokenizerTest` | BPE encode / decode round-trip over a corpus, including paralinguistic tags. | no |
| `Ino.Chatterbox.EmbedTest` / `EmbedRawTest` | `embed_tokens` forward pass, with and without an explicit token ID array. | no |
| `Ino.Chatterbox.ARStepTest` | Single forward pass through `language_model` against synthetic conditioning. | no |
| `Ino.Chatterbox.ARLoopTest` | Full autoregressive loop with no voice — proves the KV-cache wiring + dtype discovery. | no |
| `Ino.Chatterbox.EncoderTest` | `speech_encoder` against a staged 24 kHz WAV. | no |
| `Ino.Chatterbox.DecodeTest` | AR loop + `conditional_decoder` with synthetic conditioning. | no |
| `Ino.Chatterbox.SynthTest` | End-to-end one-shot via `FInoChatterboxTurboNativeRunner` (bypasses the subsystem). | no |
| `Ino.Chatterbox.SubsystemSynthTest [variant] [max_new_tokens] [text...]` | End-to-end via `UInoChatterboxTurboNativeSubsystem::SynthesizeAsync`. Exercises the public game-instance API end-to-end. | **yes** |
| `Ino.Chatterbox.StreamSynthTest [variant] [chunk_tokens] [max_new_tokens] [text...]` | Streaming variant — logs each `OnAudioChunk` arrival with token count + delta byte size, verifies `bIsFinal=true` lands exactly once before `OnComplete`. | **yes** |

### ElevenLabs (cloud TTS) API

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.ElevenLabsDialogueStreamTest` | Streams a short two-line dialogue through `UInoElevenLabsTextToDialogueStream`, logs total bytes received + chunk count + per-format header bytes. Requires `ElevenLabsApiKey` set in Project Settings. | **yes** |

Every UE API observer UCLASS uses the same pattern: `NewObject` + `AddToRoot`, bind dynamic delegates via `AddDynamic`, run the workflow, and in `Finish()` call `Conversation->Shutdown()` for deterministic teardown before clearing UPROPERTY refs and `RemoveFromRoot`. Do NOT call `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true)` from inside a delegate handler — parallel GC workers race the in-flight delegate's write access and trip `FMRSWRecursiveAccessDetector`. `Shutdown()` is the safe alternative because it only resets the worker `TUniquePtr`; it never touches delegate state.

All dynamic delegate handlers on observer UCLASSes MUST take `FString` **by value**, not `const FString&`. UE's `BindDynamic` does strict method-pointer matching against the delegate's declared signature, and every delegate in this plugin declares `FString` by value. A handler with `const FString&` compiles fine on its own but fails at the `BindDynamic` call site with a cryptic `cannot convert argument` error.

### Shared helpers + adding new tests

Shared helpers (model path resolution, JSON parsing, tool-call extraction, assistant text extraction) live in `InoSmokeTestCommon.{h,cpp}` under the `InoSmokeTest` namespace. Test-specific helpers live in the test file's anonymous namespace.

To add a new smoke test, drop a new `.cpp` (and optional `.h` for observer UCLASSes) into `Private/SmokeTests/`. UBT auto-picks up `.cpp` files under `Private/`; no `Build.cs` changes needed. `Private/SmokeTests/` is already on the include path via `PrivateIncludePaths`.

Smoke tests are compiled into every build configuration. For now they're gated behind console commands and never run unless explicitly invoked. If any individual test grows shipping-sensitive logic, wrap that file in `#if !UE_BUILD_SHIPPING` as a follow-up change.

## Platform support

### LiteRT-LM (LLM)

LiteRT-LM platform availability is owned by **InoLiteRT** — see
`Plugins/InoLiteRT/CLAUDE.md` for which platforms ship which artifacts.
From InoAgents' perspective it's a binary "available or not" question:

| Platform              | LiteRT-LM via InoLiteRT |
|---|---|
| Windows (Win64, MSVC) | ✅ available |
| Android (arm64-v8a)   | ✅ available |
| Android (x86_64)      | ✅ available |
| iOS / Linux / macOS   | ⏳ stubs only |

`InoLiteRtLmStubs_NonWindows.cpp` is guarded by
`#if !PLATFORM_WINDOWS && !PLATFORM_ANDROID`, so it compiles in for
iOS / Linux / macOS only. The stubs make every `litert_lm_*` symbol
return null / non-zero so the InoAgents subsystem fails gracefully via
its `OnLoaded` delegate on those platforms.

### ONNX Runtime + llama.cpp

Sibling-plugin owned. `Plugins/InoOnnx/CLAUDE.md` and `Plugins/InoLlama/CLAUDE.md` list the per-platform support matrix and which providers / backends ship in each artifact. From InoAgents' perspective the picture is the same as for LiteRT-LM: the runtime is either available on the platform or not, and InoAgents code calls into it via `InoAgents::Onnx::GetApi()` / `InoAgents::LlamaCpp::GetApi()` in either case.

The UE API (subsystems, conversations, tools, delegates) is **identical across platforms**. On platforms where a sibling runtime isn't yet staged, the InoAgents subsystems fail gracefully — `UInoLiteRtLmSubsystem::LoadModelAsync` returns "Native engine failed" via `FOnInoLiteRtLmModelLoaded`, `FInoOnnxSession::Create` returns nullptr with a clear error log, and every non-runtime feature (ElevenLabs cloud TTS, Slate chat panel, animation/audio helpers) keeps working normally.

### Android specifics

All native packaging on Android — UPL XML, `<soLoadLibrary>` order, GPU/NNAPI accelerator staging — is owned by the sibling plugins (`InoLiteRT`, `InoOnnx`, `InoLlama`) at `LoadingPhase=PreLoadingScreen`. By the time InoAgents' `Default`-phase `FInoAgentsModule::StartupModule` runs, all three runtimes are mapped into the process. What InoAgents itself owns on Android:

- **Model file distribution.** The 2.6–5 GB `.litertlm` model file cannot ship inside the APK (Play Store limit is 200 MB base APK). The subsystem auto-downloads to `FPaths::ProjectPersistentDownloadDir()` on first use; `android.permission.INTERNET` is required (already enabled for ElevenLabs). Same pattern for the Chatterbox ONNX bundle and the NeuTTS Nano GGUF + NeuCodec ONNX.
- **Stubs.** `InoLiteRtLmStubs_NonWindows.cpp` excludes `PLATFORM_ANDROID` so Android links against the real LiteRT-LM symbols staged by InoLiteRT.

## Model file distribution

Gemma 4 `.litertlm` model files are 2.5–5 GB and **must never be committed**. Models are auto-downloaded on first use from URLs configured in Project Settings → Plugins → InoAgents → LiteRT-LM → Models.

### Model path resolution

`LiteRtLmResolveModelPath(ModelFileName)` (in `InoLiteRtLmTypes.h/.cpp`) checks two locations in order:

1. **`FPaths::ProjectPersistentDownloadDir() / "InoAgents/Models/"`** — where auto-downloaded models are cached. This is UE's canonical location for runtime-acquired content that persists across sessions and app updates. Platform-appropriate (sandboxed on mobile, app-support on macOS).
2. **`Plugins/InoAgents/Models/`** — legacy dev-time path. The plugin's `.gitignore` excludes `Models/` so the 2.5+ GB file never lands in git.

If neither location has the file, `UInoLiteRtLmSubsystem::LoadModelAsync` looks up the `ModelFileName` in the `UInoAgentsSettings::Models` array to find the download URL, then downloads via `FHttpModule` and saves to `PersistentDownloadDir`. The subsystem fires `OnDownloadProgress(Percent, BytesReceived, TotalBytes)` during download for loading screens.

### Model config

Models are configured via `FInoLiteRtLmModelConfig` — a **plain USTRUCT** (not a UDataAsset). Build one in Blueprint via a Make node (or in C++ as a struct literal), set `ModelFileName`, `Backend`, `MaxNumTokens`, `SystemMessage`, and pass it to `UInoLiteRtLmSubsystem::LoadModelAsync`.

Phase 1 smoke tests under `InoAgents.*` still hardcode the model path via `InoSmokeTest::ResolveDefaultModelPath()` because they bypass the UE API and call the C functions directly.

### System message format

The system message is passed to LiteRT-LM's C API as a **plain text string** (NOT wrapped in JSON). The C API's `engine.cc:214-226` tries to JSON-parse the input; when parsing fails (because raw text isn't valid JSON), it falls back to using the raw string as the "content" field: `{"role":"system","content":"Your prompt here..."}`. This is what Gemma's Jinja2 chat template expects — wrapping as `{"type":"text","text":"..."}` would produce a content object that the template silently drops.

### Shipping builds

Auto-download to `PersistentDownloadDir` is **implemented** and works for both dev and shipping:
- First run downloads from the configured Hugging Face URL (~2.5–5 GB, 2–10 min)
- `OnDownloadProgress` fires for loading-screen UI
- Subsequent runs use the cached file (loads in <1 second with XNNPACK cache)
- SHA-256 verification is NOT yet implemented (planned)

### Model sources

Hugging Face, Apache 2.0, public (no gating, no auth):
- `litert-community/gemma-4-E2B-it-litert-lm` — 2.58 GB, Text + Image + Audio
- `litert-community/gemma-4-E4B-it-litert-lm` — 3.65 GB, Text + Image + Audio

## How to update the runtime versions

Runtime version bumps happen in the sibling plugins, not here — see `Plugins/InoLiteRT/CLAUDE.md` (LiteRT-LM SHA), `Plugins/InoOnnx/CLAUDE.md` (ONNX Runtime + DirectML), and `Plugins/InoLlama/CLAUDE.md` (llama.cpp release tag) for each plugin's update script and watch-outs. After any of those bumps, re-run InoAgents' UE-API smoke tests (`Ino.LiteRtLm.*`, `Ino.Onnx.*`, `Ino.Chatterbox.*`, `Ino.NeuTts.*`) to confirm InoAgents still talks to the new symbols correctly — pre-1.0 LiteRT-LM has churned its C API across SHAs, and a follow-up tweak in InoAgents may be needed.

## What to verify before trusting this file

This file describes design decisions and architectural intent. Specifics drift over time. Before acting on any specific claim:

- **LiteRT-LM version + C API:** owned by InoLiteRT — check
  `Plugins/InoLiteRT/CLAUDE.md` for the pinned SHA and authoritative
  header paths. If the symbol names / signatures InoAgents uses differ
  from what's in the staged header, trust the header — pre-1.0
  LiteRT-LM still churns its C API.
- **ONNX Runtime version + C API:** owned by InoOnnx — check
  `Plugins/InoOnnx/CLAUDE.md` for the pinned version, the staged
  `onnxruntime_c_api.h`, the rename/patch story, and the renamed binaries
  in InoOnnx's `Binaries/ThirdParty/`. Struct-field additions across ORT
  versions are common; the `OrtApi` vtable is versioned so older code
  still works, but new features require bumping `ORT_API_VERSION` checks
  in InoAgents code.
- **llama.cpp version + C API:** owned by InoLlama — check
  `Plugins/InoLlama/CLAUDE.md` for the pinned tag, the staged `llama.h`,
  and the `FLlamaCppApi` vtable surface that InoAgents consumes via
  `InoAgents::LlamaCpp::GetApi()`.
- **Conversation delegate signatures:** read `Source/InoAgents/Public/LiteRtLm/InoLiteRtLmConversation.h`. The list (OnUserMessage, OnToken, OnSentence, OnSentenceBoundary, OnComplete, OnError, OnToolCalled) and the per-event arg shapes are authoritative there — if they shift, this file's diagram in "UE-side integration architecture" goes out of date silently.
- **Chatterbox per-session DML routing:** the matrix under "Per-session execution-provider overrides (DirectML caveats)" describes the empirical state of the current ORT pin + DirectML. Re-verify after each ORT bump in InoOnnx (`Ino.Chatterbox.SubsystemSynthTest` with the relevant `b*OnCpu` flag flipped is the fastest way to spot a regression or a fix).
- **Tooling versions:** Gemma 4 variant specs and modality support may have evolved — confirm against https://ai.google.dev/gemma/docs/core.
