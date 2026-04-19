# CLAUDE.md — InoAgents plugin

This file provides guidance to Claude Code (claude.ai/code) when working inside `Plugins/InoAgents/`. The hosting demo project is documented in `E:/Projects/InoAgentDemo/CLAUDE.md`.

## Purpose

`InoAgents` is an Unreal Engine 5.7 runtime plugin that embeds **Google Gemma 4** on-device, so UE games and tools can run LLM-powered agents inside the game process with no external server and no cloud dependency.

The plugin is named for "agents" deliberately: the goal is not just text generation but **tool-use / function-calling workflows** running natively in UE, driven from Blueprint.

## Runtimes: LiteRT-LM (for LLM) + ONNX Runtime (for everything else)

The plugin carries **two** on-device ML runtimes, each doing what it's best at:

- **LiteRT-LM** — Google's TFLite-based LLM runtime. Handles Gemma 4 inference (chat, tool calling, streaming). Built from source via Bazel; statically-linked monolithic `LiteRtLm.dll` / `libLiteRtLm.so`. See the sections below.
- **ONNX Runtime** — Microsoft's ONNX inference runtime. Reserved for everything non-LLM: TTS models (Chatterbox Turbo is the first planned consumer), audio codec decoders, future vision / classifier / embedding models. Prebuilt binaries downloaded at setup time under a renamed filename to avoid UE's NNE bundling collisions. See "[ONNX Runtime (the second runtime)](#onnx-runtime-the-second-runtime)" below.

Why two runtimes instead of one: LiteRT-LM is purpose-built for on-device LLM inference (KV-cache, chat-template-aware streaming, quantized weights) and has no credible story for running arbitrary ONNX models. ONNX Runtime is the industry-standard general-purpose runtime and ships prebuilt binaries with sensible execution providers on every target platform. Each runtime is small enough (~14 MB Win64 LLM DLL, ~14 MB Win64 ORT DLL) that shipping both costs less than the engineering cost of trying to force one runtime to do both jobs.

### LiteRT-LM

The plugin uses **LiteRT-LM** (Google's open-source on-device LLM runtime — the successor to the deprecated MediaPipe LLM Inference API) as its LLM inference backend.

- Upstream: https://github.com/google-ai-edge/LiteRT-LM
- Docs: https://ai.google.dev/edge/litert-lm

Why LiteRT-LM and not llama.cpp, ONNX Runtime GenAI, MLX, or MediaPipe:

1. **Single runtime covers every target platform in this plugin's roadmap** — Windows, Linux, macOS, Android, iOS. llama.cpp is great on desktop but painful on UE's Android NDK toolchain. MediaPipe LLM Inference API is deprecated on Android/iOS and has no desktop story. MLX is Mac-only. ONNX Runtime GenAI has no first-class Gemma 4 support.
2. **Google's first-class runtime for Gemma 4.** Pre-built model bundles live at `litert-community/gemma-4-E2B-it-litert-lm` and `litert-community/gemma-4-E4B-it-litert-lm` on Hugging Face.
3. **Native tool-use / function-calling is a framework feature** surfaced through the public C API (`litert_lm_conversation_*`). Not prompt-engineered on top of a raw completion API. This matches the plugin's "agents" mandate.
4. **GPU path on Windows uses DirectX (DXC / D3D12)**, matching the demo project's `DefaultGraphicsRHI=DefaultGraphicsRHI_DX12`. No CUDA / Vulkan / OpenCL runtime to ship alongside the game.
5. **Native Win64 MSVC support is verified.** Upstream CI ships `litert_lm_main.windows_x86_64.exe` as a release artifact; the build toolchain is Bazel + MSVC + bazelisk. We do not depend on WSL for anything.

Known caveat: LiteRT-LM is **pre-1.0** (v0.10.x). Expect upstream API churn — we pin to a specific tag via submodule, never track `main`.

### Known LiteRT-LM v0.10.1 runtime limitations

Two C API features are declared in the header and compile but fail at runtime with Gemma 4 models. Both are disabled in our code (with `#if 0` / default values) and marked with `TODO(litert-upgrade)` comments:

1. **Session config (sampler params + max output tokens).** Passing a non-null `LiteRtLmSessionConfig*` to `litert_lm_conversation_config_create` causes `litert_lm_conversation_create` to return NULL for Gemma 4 models. The C API code itself handles session config cleanly (upstream tests pass with small test models), so the failure is likely Gemma 4-specific — probably inside `SessionConfig::MaybeUpdateAndValidate` when it reconciles user-supplied sampler params against Gemma 4 metadata. A failed attempt also appeared to poison subsequent conversation creations on the same engine (producing error 13 on `send_message_stream`), though the source code does not obviously explain this side-effect. **Workaround:** pass `nullptr` for session config (uses engine defaults for all sampling). The `FInoLiteRtLmSamplerConfig` struct and `MaxOutputTokens` field exist in `FInoLiteRtLmModelConfig` for forward-compatibility but are not applied at runtime.

2. **Activation data type (F16/I16/I8).** `litert_lm_engine_settings_set_activation_data_type` with non-F32 values loads the engine successfully (model file loads, XNNPACK cache regenerates), but `litert_lm_conversation_send_message_stream` returns error 13 (`absl::StatusCode::kInternal`) at runtime. This is corroborated by the upstream source: `engine.cc:332-338` force-overrides activation to F32 for GPU backends, and a TODO bug (`b/433590109`) acknowledges FP16 GPU incompatibilities. For CPU, the XNNPACK delegate configuration likely fails when model tensors don't match the requested activation format. **Workaround:** default `ActivationType` to `F32`. The enum and field exist in `FInoLiteRtLmModelConfig` for forward-compatibility but should not be changed from F32 until a future LiteRT-LM release fixes this. If a user has previously loaded a model with F16 and gets error 13, deleting the XNNPACK cache (next to the model file, or in the custom CacheDir) forces regeneration with F32.

3. **`extra_context` parameter is ignored by Gemma 4.** The C API's `litert_lm_conversation_send_message_stream` accepts an `extra_context` JSON string. The Rust minijinja runtime injects its top-level keys as Jinja2 template variables. However, the Gemma 4 chat template (embedded in the `.litertlm` model file) does **not** reference any custom template variables — it only uses `bos_token`, `messages`, `tools`, `add_generation_prompt`, and `enable_thinking`. Any `extra_context` values are silently dropped by the template engine. Verified by extracting the template from the model binary and by runtime testing with flat key-value JSON. **Workaround:** per-turn dynamic context (game state, player state) is prepended as plain text in the user message, wrapped in `[Context]`/`[/Context]` tags, via `BuildMergedContext()`. The `SetSystemContext`/`SetUserContext` API on `UInoLiteRtLmConversation` feeds into this path.

All three limitations were discovered empirically during development. When upgrading LiteRT-LM, re-test these features first — they are the most impactful unlocks (lower RAM via F16, creative control via temperature, native context injection).

## Integration approach: link, not subprocess

We considered and rejected a subprocess-based integration (spawning `litert_lm_main --multi_turns` and piping stdin/stdout). Reasons:

1. **Tool calling is only available through the C++ / C API, not the CLI.** The CLI's `--multi_turns` mode does plain text chat only. A subprocess backend could never expose the plugin's headline feature.
2. **Subprocess is impossible on iOS** (code signing prohibits `exec` of bundled binaries) **and forbidden by Google Play Protect on Android.** Since phases 2–3 must link LiteRT-LM as a library anyway, doing subprocess for phase 1 would just mean writing the same backend twice.
3. **Crash isolation is real but restart cost is huge.** Reloading a 3.2 GB Gemma 4 model after a subprocess crash takes seconds. Not a win in practice.

The plugin uses **one integration strategy across all five platform phases: linked library via the LiteRT-LM public C API**.

## DLL boundary: pure C API (`c/engine.h`)

The LiteRT-LM public API at `LiteRtLm/vendor/LiteRT-LM/c/engine.h` is a **pure C API** wrapped in `extern "C"`, with `__declspec(dllexport)` already applied on Windows via `LITERT_LM_C_API_EXPORT`. LiteRT-LM was designed from day one to be a DLL boundary, which means the UE integration skips entire classes of pain:

- **No C++ ABI matching.** No libstdc++-vs-MSVC-STL concerns, no iterator ABI, no exception propagation across the DLL boundary, no RTTI worries.
- **No STL types in the boundary.** Opaque pointers (`LiteRtLmEngine*`, `LiteRtLmSession*`, `InoLiteRtLmConversation*`, etc.) + primitive types + C callbacks. Nothing more.
- **No hand-written wrapper layer.** `InoAgentsEngine.cpp` `#include`s `c/engine.h` and calls the C functions directly. UE-side work is marshalling between C types and UE types, not bridging ABIs.

The C API already covers what we need:

- Engine / session / conversation lifecycle with CPU or GPU backend selection
- Streaming generation with C callback (`LiteRtLmStreamCallback`)
- Multimodal input (text / image / audio via `InputData` struct)
- Conversation API with tool calling: `litert_lm_conversation_create` accepts `tools_json` + `messages_json` parameters, `send_message_stream` dispatches tool calls, `cancel_process` cancels inference, constrained decoding is a flag
- Benchmarking

## Model scope

Only the two "E" (edge / on-device) Gemma 4 variants are in scope for this plugin:

| Variant | Effective params | Context | Modalities | File size |
|---|---|---|---|---|
| **E2B** | ~2B | 128K | Text + Image + Audio | ~2.6 GB |
| **E4B** | ~4B | 128K | Text + Image + Audio | ~3.7 GB |

**Both variants are fully multimodal** — verified by inspecting the `.litertlm` containers directly. Both files carry `tf_lite_vision_encoder` + `vision_adapter_280` and `tf_lite_audio_encoder_hw` + `audio_adapter_features/mask` sections, plus `<|image|>` and `<|audio|>` special tokens and template branches for both modalities. E4B differs from E2B only in LLM backbone size (4B vs 2B effective params); the vision and audio encoders are identical. Earlier versions of this file claimed E2B was text+image-only — that was wrong.

Note: **our UE-side wrapper currently passes `nullptr` for `vision_backend_str` and `audio_backend_str`** in `InoLiteRtLmSubsystem.cpp`'s `litert_lm_engine_settings_create` call, so even though the models support images/audio, the plugin's current code path only consumes text. Enabling multimodal input is tracked as future work — requires wiring image/audio payloads into our `UInoLiteRtLmConversation` Blueprint API and building `InputData` arrays for the C API.

Gemma 4's **31B dense** and **26B A4B MoE** server-class variants are **intentionally out of scope** — they are not realistic to run inside a consumer UE game process alongside a renderer (17+ GB VRAM just for weights), and LiteRT-LM is an edge runtime, not a server runtime.

## Repository layout

```
Plugins/InoAgents/
├── InoAgents.uplugin
│
├── LiteRtLm/                                      ← LLM runtime — Bazel build workspace
│   ├── vendor/LiteRT-LM/                          ← git submodule, upstream pinned at v0.10.2
│   ├── overlay/                                   ← files staged into the submodule before build
│   │   └── ino/
│   │       ├── BUILD.bazel                        ← //ino:LiteRtLm target, deps //c:engine
│   │       └── LiteRtLm_exports.cc                ← force-reference stub
│   ├── scripts/                                   ← setup.ps1, build-win64.ps1,
│   │                                                 build-android-arm64.ps1,
│   │                                                 update-litert.ps1, clean.ps1
│   ├── LITERT_LM_TAG
│   └── README.md
│
├── OnnxRuntime/                                   ← ONNX runtime — downloads prebuilts, stages
│   ├── ONNXRUNTIME_VERSION                        ← pinned ORT version (e.g. "1.24.3")
│   ├── scripts/
│   │   └── setup-onnxruntime.ps1                  ← download + stage Win64 + Android AAR
│   ├── .cache/                                    ← downloaded ZIPs/AARs (gitignored)
│   └── README.md
│
├── Source/
│   ├── InoAgents/                                 ← runtime module (UE-facing, wraps both runtimes)
│   │   ├── InoAgents.Build.cs
│   │   ├── Public/
│   │   │   ├── InoAgents.h                        ← module interface (FInoAgentsModule)
│   │   │   ├── LiteRtLm/                          ← Blueprint-facing LLM types (subsystem, tools, etc.)
│   │   │   └── Onnx/                              ← generic ONNX session / tensor API
│   │   │       ├── InoOnnxTypes.h                 ← Dtype / Provider / SessionOptions enums+struct
│   │   │       ├── InoOnnxTensor.h                ← FInoOnnxTensor (move-only OrtValue wrapper)
│   │   │       └── InoOnnxSession.h               ← FInoOnnxSession (Create, Run, RunAsync)
│   │   └── Private/
│   │       ├── InoAgents.cpp                      ← module lifecycle + DLL loading for both runtimes
│   │       ├── InoAgentsLog.h                     ← shared LogInoAgents category
│   │       ├── LiteRtLm/                          ← LLM-side impl (subsystem, conversation worker)
│   │       ├── Onnx/                              ← ORT-side impl
│   │       │   ├── InoOnnxModule.{h,cpp}          ← Init/Shutdown/GetApi; dynamic DLL loading
│   │       │   ├── InoOnnxInternal.{h,cpp}        ← CheckOrtStatus, dtype conv, env singleton
│   │       │   ├── InoOnnxTensor.cpp
│   │       │   └── InoOnnxSession.cpp
│   │       └── SmokeTests/                        ← dev-time console commands
│   │           ├── InoLoadEngineTest.cpp          ← Ino.LoadEngineTest (LLM)
│   │           ├── InoGenerateTest.cpp            ← Ino.GenerateTest (LLM)
│   │           ├── InoConversationTest.cpp        ← Ino.ConversationTest (LLM)
│   │           ├── InoToolCallTest.cpp            ← Ino.ToolCallTest (LLM)
│   │           ├── InoStreamTest.cpp              ← Ino.StreamTest (LLM)
│   │           └── InoOnnxTest.cpp                ← Ino.Onnx.ProvidersTest,
│   │                                                 Ino.Onnx.SessionFromFileTest
│   └── ThirdParty/
│       ├── InoAgentsLibrary/                      ← LLM external module
│       │   ├── Public/litert/lm/engine.h          ← staged C API header
│       │   ├── Win64/LiteRtLm.lib                 ← staged import library
│       │   ├── InoAgentsLibrary.Build.cs          ← per-platform linking + UPL hook
│       │   └── InoAgentsLibrary_UPL_Android.xml   ← APK packaging for LLM .so files
│       └── InoOnnxRuntime/                        ← ONNX external module
│           ├── Public/                            ← staged ORT headers (onnxruntime_c_api.h, etc.)
│           ├── InoOnnxRuntime.Build.cs            ← NO implicit linking; dynamic load at runtime
│           └── InoOnnxRuntime_UPL_Android.xml     ← APK packaging for libInoOnnxRuntime.so
│
└── Binaries/ThirdParty/
    ├── InoAgentsLibrary/                          ← LLM runtime binaries
    │   ├── Win64/
    │   │   ├── LiteRtLm.dll                       ← Bazel-built wrapper (~14 MB)
    │   │   ├── libLiteRt.dll                      ← LiteRT core (~11 MB)
    │   │   ├── libGemmaModelConstraintProvider.dll
    │   │   ├── libLiteRtWebGpuAccelerator.dll
    │   │   └── libLiteRtTopKWebGpuSampler.dll
    │   └── Android/arm64-v8a/
    │       ├── libLiteRtLm.so                     ← Bazel-built (monolithic ~49 MB)
    │       ├── libGemmaModelConstraintProvider.so
    │       ├── libLiteRtGpuAccelerator.so
    │       ├── libLiteRtOpenClAccelerator.so
    │       ├── libLiteRtTopKOpenClSampler.so
    │       ├── libLiteRtTopKWebGpuSampler.so
    │       └── libLiteRtWebGpuAccelerator.so
    └── InoOnnxRuntime/                            ← ONNX runtime binaries
        ├── Win64/
        │   └── InoOnnxRuntime.dll                 ← RENAMED from onnxruntime.dll (~14 MB)
        └── Android/arm64-v8a/
            └── libInoOnnxRuntime.so               ← RENAMED from libonnxruntime.so (~25 MB)
```

**`LiteRtLm/vendor/LiteRT-LM/`** is a git submodule (`https://github.com/google-ai-edge/LiteRT-LM.git`) pinned at tag **v0.10.2** (commit `476c0bd`). We never edit files inside the submodule directly. Version bumps happen via `LiteRtLm/scripts/update-litert.ps1`, which updates the submodule pointer and re-runs the overlay + build.

**`LiteRtLm/overlay/`** holds files that need to land inside the submodule's source tree at build time (for example, a custom `BUILD.bazel` target that produces our DLL). The overlay is tracked in the plugin repo. `setup.ps1` copies overlay files into the submodule and adds them to the submodule's `.git/info/exclude` so the submodule working tree stays clean from git's perspective.

**`Source/ThirdParty/InoAgentsLibrary/`** is a UE `External` module that tells UBT how to link and stage LiteRT-LM's libraries on each platform. Win64 branch uses the classic UE Third-Party pattern: `PublicAdditionalLibraries.Add(...)` for the `.lib` import library, `PublicDelayLoadDLLs.Add(...)` for the runtime `.dll`s, `RuntimeDependencies.Add(...)` for packaging. Android branch is different: `PublicAdditionalLibraries.Add(<path>.so)` directly (no import libs on Android — the `.so`'s export table is the link target), `RuntimeDependencies.Add(...)` for staging, and `AdditionalPropertiesForReceipt.Add("AndroidPlugin", <UPL XML>)` for injecting `<soLoadLibrary>` + `<resourceCopies>` into UE's APK packager. See the "Platform support → Android specifics" section below.

## Build system: Bazel 7.6.1 via bazelisk

LiteRT-LM is built from source with Bazel. We do not use the prebuilt artifacts from upstream GitHub releases — we control the build ourselves so we can produce a `cc_binary(linkshared=1)` DLL target that isn't in their shipped targets.

Key facts about the upstream Bazel setup:

- **Pinned Bazel version: 7.6.1** (`vendor/LiteRT-LM/.bazelversion`). Bazelisk auto-fetches this on first build.
- **Legacy `WORKSPACE` mode** (`common --noenable_bzlmod`). We do not try to use bzlmod.
- **Upstream `.bazelrc` has a `build:windows` config** that we inherit. Key flags it already sets for us:
  - `--config=monolithic` — on Windows, all transitive deps link statically into a single shared object. This is exactly what we want — one `LiteRtLm.dll` with everything inside.
  - `--cxxopt=/std:c++20` — C++20 mode
  - `--copt=/arch:AVX2` — AVX2 baseline (any x86_64 CPU since ~2013)
  - `--copt=/DLITERT_DISABLE_OPENCL_SUPPORT=1` — OpenCL disabled (we use D3D12 anyway)
  - `--shell_executable="C:/Program Files/Git/bin/bash.exe"` — **upstream hardcodes Git at this exact path** for shell genrules
  - `startup --windows_enable_symlinks` + `--enable_runfiles` — requires Developer Mode enabled
- **Windows static-library export quirk (IMPORTANT).** Upstream `build:windows --legacy_whole_archive=0` disables `--whole-archive` on Windows (upstream bug `b/469455895`). This has a non-obvious consequence: when our `cc_binary(linkshared=1)` target depends on a `cc_library` like `//c:engine`, **MSVC's linker only pulls in `.obj` files from that static library that are referenced by already-included code**. A `__declspec(dllexport)` annotation on a function in an otherwise-unreferenced `.obj` is silently dropped — the build succeeds, but the function never reaches the DLL's export table. See "Custom Bazel target" for how we work around this with a force-reference stub.

## Custom Bazel target

We define exactly one new Bazel target via the overlay, in a new package inside the submodule:

- **`//ino:LiteRtLm`** — a `cc_binary(linkshared=1, linkstatic=1)` that depends on `//c:engine` (the full CPU + GPU target). We originally started with `//c:engine_cpu` during Milestone D bring-up and swapped to `//c:engine` once end-to-end UE integration was verified working.
- Built output: `LiteRtLm.dll` (automatic Windows naming from `cc_binary(linkshared=1)`)
- Reuses `build:windows --config=monolithic` from upstream `.bazelrc` — all transitive deps link statically into the single DLL
- **We do NOT use a `/DEF:` file** (unlike upstream's `runtime/engine:litert_lm_main`). `/DEF:` is *exclusive* — it overrides `__declspec(dllexport)` and the linker's `/OPT:REF` dead-strips anything not listed. Trying it produced a 12 MB DLL with zero `litert_lm_*` exports.

### Why `LiteRtLm_exports.cc` exists

Bazel's `cc_binary` rule requires at least one source file. Ours, `overlay/ino/LiteRtLm_exports.cc`, serves two roles:

1. **DllMain stub** (standard Windows DLL entry point).
2. **Force-reference of every `litert_lm_*` C API function** — a `volatile` array of function pointers that takes the address of each exported API function. Because `LiteRtLm_exports.cc` is part of the `cc_binary`'s own `srcs` (not a static library), its `.obj` is always linked. Taking the address of each function creates hard link-time references, forcing MSVC to pull in the `.obj` files from `//c:engine`'s static archive. Once those `.obj` files are pulled in, the `__declspec(dllexport)` annotations on their symbols (via `LITERT_LM_C_API_EXPORT` in `c/engine.h`) drive the export table.

**Maintenance:** the force-reference array must be kept in sync with the functions declared in `c/engine.h`. When LiteRT-LM is upgraded, re-extract the current list with:

```bash
awk '/^LITERT_LM_C_API_EXPORT$/{flag=1; next} flag{
     match($0, /litert_lm_[a-zA-Z_0-9]+/);
     print substr($0, RSTART, RLENGTH); flag=0}' \
     vendor/LiteRT-LM/c/engine.h
```

and reconcile against `LiteRtLm_exports.cc`. A missing entry results in the corresponding symbol silently dropping from `LiteRtLm.dll` — the build succeeds, but callers fail at UE link time with "unresolved external symbol".

### Upstream bugs worked around in BUILD.bazel

- **`litert_lm_set_min_log_level`**: upstream `c/litert_lm_logging.h` declares this function without `__declspec(dllexport)`, while `c/engine.h` declares the *same function* with it. Since `litert_lm_logging.cc` includes only the non-exporting header, its `.obj` is compiled without the export marker, and force-reference alone is insufficient. Work around with an additive `/EXPORT:litert_lm_set_min_log_level` linkopt in our BUILD.bazel. File upstream issue and remove the linkopt when fixed.

### `libGemmaModelConstraintProvider.dll`

Our `LiteRtLm.dll` depends on `libGemmaModelConstraintProvider.dll` at runtime. This is an upstream **prebuilt** (LFS-tracked) binary at `vendor/LiteRT-LM/prebuilt/windows_x86_64/libGemmaModelConstraintProvider.dll`, ~13 MB. Some `cc_library` target in the `//c:engine` dep graph declares it as a data dependency, and Bazel symlinks it into `bazel-bin/ino/` alongside our DLL. `build-win64.ps1` copies the symlink target (the real file) into `Binaries/ThirdParty/InoAgentsLibrary/Win64/`. `InoAgentsLibrary.Build.cs` must list it in `PublicDelayLoadDLLs` and `RuntimeDependencies` so UE stages it alongside the executable. Without it, `LiteRtLm.dll` fails to load at runtime. The same file (`.so` form) ships in `prebuilt/android_arm64/libGemmaModelConstraintProvider.so` for the Android build.

## ONNX Runtime (the second runtime)

Separate from LiteRT-LM. Covers every ONNX-model consumer the plugin will ever have — TTS (Chatterbox Turbo is the first), audio codec decoders, and any future vision / classifier / embedding workload. The infrastructure is **model-agnostic** — no Chatterbox-specific code exists in this layer.

### Version pin + setup

Pinned version lives in `OnnxRuntime/ONNXRUNTIME_VERSION`, currently **1.24.3** (latest version that has both a Windows GitHub Releases ZIP AND an Android AAR on Maven Central; newer Windows releases sometimes appear before the Android counterpart).

Setup is a single script:

```powershell
cd Plugins/InoAgents/OnnxRuntime/scripts
./setup-onnxruntime.ps1
```

The script is idempotent — re-running after a clean checkout downloads the official Microsoft prebuilts (Win64 ZIP from GitHub Releases; Android AAR from Maven Central), extracts headers + .dll/.so, and stages into the third-party + Binaries trees. Caches downloads in `OnnxRuntime/.cache/` so re-runs are fast.

Bumping the pin: edit `ONNXRUNTIME_VERSION`, delete `Source/ThirdParty/InoOnnxRuntime/.ort_version` (or let the script detect drift), re-run `setup-onnxruntime.ps1`, re-run the smoke tests (`Ino.Onnx.ProvidersTest`, `Ino.Onnx.SessionFromFileTest`). Commit the `ONNXRUNTIME_VERSION` bump together with any API tweaks the new version requires.

### Why we rename the DLL / .so (important — don't undo this)

Both the Win64 DLL and the Android .so are **renamed** during staging:

- Windows: `onnxruntime.dll` → `InoOnnxRuntime.dll`
- Android: `libonnxruntime.so` → `libInoOnnxRuntime.so`

This is not cosmetic. UE 5.7 ships **multiple** unrelated copies of ONNX Runtime via bundled plugins — at time of writing:

- `Engine/Plugins/NNE/NNERuntimeORT/Binaries/ThirdParty/Onnxruntime/Win64/onnxruntime.dll` (UE's NNE runtime, ORT 1.19.x)
- `Engine/Plugins/Marketplace/RuntimeM558be8d6854bV8/.../Win64/onnxruntime.dll` and `.../Android/arm64-v8a/libonnxruntime.so` (a Marketplace plugin's ORT 1.19.2)

If we were to ship our ORT under its default name:

- **Windows** — LoadLibrary caches DLLs by **base name**, so when our `FPlatformProcess::GetDllHandle` ran with our full path, Windows would return whichever `onnxruntime.dll` was already loaded into the process (usually NNE's older one). Our `OrtApi::GetApi(ORT_API_VERSION=24)` would return nullptr because the cached DLL only implements API 19.
- **Android** — clang's linker at libUnreal.so build time would resolve our code's `OrtGetApiBase` reference against whichever `libonnxruntime.so` appeared first on the link path (often the Marketplace plugin's 1.19.2), recording a versioned symbol reference `OrtGetApiBase@VERS_1.19.2`. At runtime the APK only contains our 1.24.3 `.so` (tagged `VERS_1.24.3`), so the dynamic linker aborts the process during libUnreal.so init — silently, before UE's logger is up, no `.log` file, no stack trace.

Both were diagnosed during Phase 3 of the integration. The rename + switch to **dynamic loading via GetProcAddress / dlsym** eliminates every collision possibility. We isolate fully: `libUnreal.so` has **zero** Ort* symbol references, and ours is the only file the runtime binds to.

### Dynamic loading, not static linking

Direct consequence of the above: `InoOnnxRuntime.Build.cs` does NOT call `PublicAdditionalLibraries` or `PublicDelayLoadDLLs` on any platform. It only:

- adds `Source/ThirdParty/InoOnnxRuntime/Public/` to `PublicSystemIncludePaths` (ORT headers used at compile time for the `OrtApi` / `OrtValue` / etc. struct definitions)
- stages the runtime via `RuntimeDependencies` (Windows) and `AdditionalPropertiesForReceipt("AndroidPlugin", <UPL>)` (Android, with explicit `<copyFile>` into the APK's `lib/arm64-v8a/`)

At runtime, `FInoAgentsModule::StartupModule` calls `InoAgents::Onnx::Init()` which uses `FPlatformProcess::GetDllHandle` on the renamed file, then `FPlatformProcess::GetDllExport("OrtGetApiBase")` to resolve the single entry point. From that point on, everything routes through the `OrtApi*` vtable returned by `GetApiBase()->GetApi(ORT_API_VERSION)` — never through any static-linked symbol. The `OrtApi*` is cached in a file-static and exposed through `InoAgents::Onnx::GetApi()` for every consuming file in the plugin.

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
| `InoOnnxModule.{h,cpp}` | `Init` / `Shutdown` / `GetApi`. Dynamic DLL/.so loading with per-platform path resolution. |
| `InoOnnxInternal.{h,cpp}` | `CheckOrtStatus` (uniform error log + release), dtype translations, provider-name debug strings, graph-opt-level translation, lazy-init `GetGlobalOrtEnv()`. |
| `InoOnnxTensor.cpp` | Tensor factories, OrtAllocator-backed allocation, data pointer access with sizeof(T) vs dtype-size consistency check. |
| `InoOnnxSession.cpp` | Options builder, provider registration with fallback logging, I/O metadata caching (OrtAllocator-owned name strings freed immediately), Run/RunAsync implementation. |

**Design principles worth preserving:**

- No Blueprint exposure in this layer. Per-model consumers (future `UInoChatterboxTtsSubsystem`, vision wrappers, etc.) add Blueprint-friendly APIs on top.
- Sessions are thread-safe for `Run()` per ORT guarantees; FInoOnnxTensors are move-only to avoid surprise-cost deep clones on the LLM streaming hot path.
- Exception-free — uses the C API (`onnxruntime_c_api.h`), not the C++ API (`onnxruntime_cxx_api.h` throws `Ort::Exception`). UE modules default `bEnableExceptions=false`; keeping the whole stack exception-free avoids per-module opt-ins.
- Positional I/O ordering for `Run()`. A named-map variant would cost a hash lookup per inference which matters on AR token loops — callers who need names can wrap trivially at their own layer.

### Execution providers

Phase 4 ships with:

- **Windows**: CPU, Azure (Azure is provider-unless-you-explicitly-register; we don't. It comes listed by `GetAvailableProviders` because ORT was compiled with it, but nothing in the plugin requests it.)
- **Android**: CPU, XNNPACK, NNAPI, WebGPU — all four available in the 1.24.3 Android AAR and usable by requesting them in `FInoOnnxSessionOptions::ExecutionProviders`.

Not yet shipped:

- **DirectML** — Windows GPU via D3D12, matches UE's renderer cleanly. Will add via a separate redistributable (Microsoft ships DirectML as a NuGet / standalone DLL); needs its own unique filename to avoid collisions just like the core ORT DLL.
- **CUDA / TensorRT** — intentionally not shipping. Require 150+ MB of NVIDIA runtime libs alongside every game; DirectML covers the same ground via D3D12 for every GPU vendor.

Provider fallback: if a caller requests `[Xnnpack, Nnapi, Cpu]` on a platform where NNAPI isn't registered, that provider is skipped with a warning log and the session is built with the remaining providers. `FInoOnnxSession::GetActiveProviders()` reports what actually made it.

### Smoke tests

Two console commands under `Source/InoAgents/Private/SmokeTests/InoOnnxTest.cpp`, auto-registered as `FAutoConsoleCommand` globals (same pattern as the LLM smoke tests):

| Command | Args | What it does |
|---|---|---|
| `Ino.Onnx.ProvidersTest` | none | Calls `OrtApi::GetAvailableProviders` via the cached API vtable and logs every entry. Doubles the module-startup check; useful after Live Coding or as a first diagnostic. |
| `Ino.Onnx.SessionFromFileTest <abs-path-to-model.onnx>` | 1 | Loads the ONNX model, calls `FInoOnnxSession::LogMetadata()` (dumps I/O shapes + dtypes + active providers). If all inputs have concrete shapes, allocates zero-filled inputs and runs one forward pass; reports load time + run time + output shapes. Exercises the full Session + Tensor API end-to-end with no per-model code. |

## Toolchain requirements (Windows host)

A developer machine needs all of the following before `scripts/build-win64.ps1` or `scripts/build-android-arm64.ps1` can succeed. All Android builds use the Windows host as the cross-compilation host — we do not build LiteRT-LM on Android itself.

| Requirement | How |
|---|---|
| **Developer Mode enabled** | Settings → System → For developers → Developer Mode → On. Bazel needs symlink-creation rights. |
| **Visual Studio 2022** with the C++ workload | Community edition is fine. MSVC toolset 14.38+ required. |
| **`BAZEL_VC` user env var** | Set to `<VS install>\VC` — e.g. `C:\Program Files\Microsoft Visual Studio\2022\Community\VC`. Single backslashes (Windows env var UI takes raw strings, no escaping). |
| **Bazelisk on PATH** | `winget install Bazel.Bazelisk`. Bazelisk auto-downloads the Bazel version pinned by `.bazelversion`. |
| **Git at `C:\Program Files\Git`** | Required because upstream `.bazelrc` hardcodes `C:/Program Files/Git/bin/bash.exe` for shell genrules. |
| **Python 3 on PATH** | Used by Bazel's protobuf / XNNPACK build rules. Any 3.10+ works. |
| **Windows long paths enabled** | `HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`. Necessary but NOT sufficient — `link.exe` does not transparently use the `\\?\` prefix for its input files, so some tools in the build still hit MAX_PATH even with long paths on. The short `--output_base` below is the actual fix. |
| **40+ GB free on the Bazel output drive** | First cold build of LiteRT-LM + all transitive deps is ~20 GB of caches + outputs. |
| **Short Bazel `--output_base`** (MANDATORY, not optional) | `scripts/build-win64.ps1` passes `bazelisk --output_base=C:/b/ino build ...`. Matches upstream CI's pattern (`D:/w-<hash>/`) from `.github/workflows/ci-build-win.yml`. Without this, intermediate filenames like `…/crate_index__macro_rules_attribute-proc_macro-0.2.2/…cgu.0.rcgu.o` exceed 260 chars and `link.exe` fails with `LNK1181: cannot open input file`. `setup.ps1` creates `C:/b/ino` on first run — no admin needed on a modern Windows 10/11 user profile. |
| **Antivirus exclusion for `C:\b\`** | Real-time scanning of Bazel's output root slows cold builds 3–5x. Not mandatory but strongly recommended. |

For Android cross-compilation (`build-android-arm64.ps1`), additionally:

| Requirement | How |
|---|---|
| **Android SDK** (any modern version) | Install via Android Studio. UE 5.7's `SetupAndroid.bat` also auto-installs it to `%LOCALAPPDATA%\Android\Sdk\` if the editor's Android packaging has been run at least once. |
| **Android NDK r28b or newer** | Install via Android Studio's SDK Manager → SDK Tools → NDK (Side by side) → check "Show Package Details" → select 28.0 or newer. Do **not** use UE's NDK r27.2 — LiteRT-LM's Bazel config requires r28+. The two NDKs coexist under `%LOCALAPPDATA%\Android\Sdk\ndk\` as separate subdirectories; `build-android-arm64.ps1` auto-detects the newest r28+ and points `ANDROID_NDK_HOME` at it for the child Bazel process only. |
| **Second Bazel output base** | `build-android-arm64.ps1` uses `C:/b/ino-android` instead of `C:/b/ino` so the Android and Windows builds don't fight over the same action cache. Same MAX_PATH rationale. |

The preflight check for all of this lives in `LiteRtLm/scripts/setup.ps1` and should be the first thing a new dev runs. The Android script does its own additional preflight (NDK detection, version parsing) when invoked.

## UE-side integration architecture

The UE-facing API lives under `Source/InoAgents/Public/LiteRtLm/` and `Private/LiteRtLm/`. Names are prefixed `LiteRtLm` rather than `InoAgents` on purpose — future versions of this plugin will host multiple backends (OpenAI, Anthropic, llama.cpp) and each backend's classes live in their own subdirectory. Naming the classes after the backend from day one makes the boundary explicit.

```
Blueprint ─┬─ UInoLiteRtLmAgentComponent  (USceneComponent, all-in-one)
           │     THE primary entry point. Drop on actor, set ModelConfig +
           │     VoiceId, call SendMessage. Internally owns + wires:
           │       child UInoAgentsStreamingAudioComponent (3D audio)
           │       UInoLiteRtLmDialogueQueue (ordered TTS)
           │       UInoLiteRtLmConversation (LLM chat)
           │     Delegates (pass-through): OnModelLoaded, OnToken,
           │       OnSentence(RawText,CleanText), OnComplete, OnError,
           │       OnAudioFinished, OnDownloadProgress
           │     Config: ModelConfig (FInoLiteRtLmModelConfig struct),
           │       VoiceId, TtsRequestTemplate, PauseDurationMs
           │
           ├─ UInoLiteRtLmSubsystem       (UGameInstanceSubsystem)
           │     owns LiteRtLmEngine*, tool registry, ShowChatPanel/
           │     HideChatPanel. LoadModelAsync auto-downloads models
           │     from URLs configured in UInoAgentsSettings.
           │     OnDownloadProgress fires during download.
           │
           ├─ UInoLiteRtLmConversation    (UObject, BlueprintType)
           │     owns one native InoLiteRtLmConversation* plus a pinned
           │     worker thread. Multicast delegates:
           │       OnToken(Chunk)
           │       OnSentence(RawText, CleanText)  — per newline
           │       OnNewLine()                     — pause signal
           │       OnComplete(FullText)
           │       OnError(ErrorMessage)
           │       OnToolCalled(Name, ArgsJson, ResultJson)
           │
           ├─ FInoLiteRtLmModelConfig     (USTRUCT, BlueprintType)
           │     plain struct (NOT a UDataAsset). Fields:
           │       ModelFileName — resolved via LiteRtLmResolveModelPath:
           │         1. PersistentDownloadDir/InoAgents/Models/ (cached)
           │         2. Plugins/InoAgents/Models/ (legacy dev)
           │         3. auto-download from UInoAgentsSettings URL
           │       Backend (Cpu / Gpu), MaxNumTokens, SystemMessage
           │
           ├─ UInoAgentsSettings       (UDeveloperSettings)
           │     unified Project Settings page under Plugins → InoAgents.
           │     Two sections:
           │       ElevenLabs: ApiKey, BaseUrl, DefaultModelId, OutputFormat
           │       LiteRT-LM → Models: array of {DisplayName, FileName, URL}
           │
           ├─ UInoLiteRtLmToolBase         (Blueprintable abstract UObject base class)
           │
           ├─ UInoAgentsStreamingAudioComponent  (UAudioComponent subclass)
           │     plays PCM int16 / PCM float32 / MP3 bytes at runtime.
           │     FeedAudioBytes + FinalizeStream + PlayAudio + StopAndReset.
           │     Pre-buffer before Play (configurable PreBufferMs).
           │     MP3 decoded via bundled minimp3 (CC0, single-header).
           │
           ├─ UInoLiteRtLmDialogueQueue    (UObject)
           │     auto-binds to conversation OnSentence + OnNewLine.
           │     Dispatches ElevenLabs TTS in parallel per-sentence,
           │     plays audio back in strict order via the streaming
           │     audio component. Pause slots between lines.
           │
           ├─ UInoElevenLabsSubsystem     (UGameInstanceSubsystem)
           │     caches settings, anchors live HTTP actions, CancelAll
           │     on PIE end.
           │
           └─ UInoElevenLabsTextToDialogueStream  (UBlueprintAsyncActionBase)
                 latent Blueprint node for /v1/text-to-dialogue/stream.
                 OnAudioChunk / OnComplete / OnError.
                      │
                      ▼
          FInoLiteRtLmConversationWorker   (FRunnable, one per conversation)
                 Owns the native InoLiteRtLmConversation and ConversationConfig.
                 Multi-round agent loop: user msg → tool calls → tool
                 results → final text. Marshals via AsyncTask(GameThread).
                      │
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
3. `UInoLiteRtLmSubsystem::CreateConversation` calls `BuildToolsJsonForConversation` which serialises every registered tool's schema into a JSON array via `FJsonSerializer::Serialize` with `TCondensedJsonPrintPolicy`. The array plus `enable_constrained_decoding=true` are passed to `litert_lm_conversation_config_create`. When no tools are registered, both are left at their defaults and the conversation behaves as a plain chat.
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

### UE-facing API

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.LiteRtLm.SubsystemLoadTest` | `UInoLiteRtLmSubsystem::LoadModelAsync` dispatches to a ThreadPool worker, marshals `FOnInoLiteRtLmModelLoaded` back to the game thread, and `IsModelLoaded` reports true afterward. Non-blocking. | **yes** |
| `Ino.LiteRtLm.ConversationSendTest` | `UInoLiteRtLmConversation` round-trips a non-streaming "What is 2 plus 2?" prompt through the worker's agent loop (streaming internally) and delivers the full accumulated text via `OnComplete`. | **yes** |
| `Ino.LiteRtLm.ConversationStreamTest [prompt]` | Streaming surface: binds `OnToken` in addition to `OnComplete` and logs each chunk with per-stream elapsed time. Cross-checks that the locally-accumulated tokens match the `FullText` delivered to `OnComplete`. | **yes** |
| `Ino.LiteRtLm.ToolRegistryTest` | Registry-only check (no model load): constructs a `UInoLiteRtLmAddNumbersTool`, registers it, looks it up, serialises `BuildToolsJsonForConversation`, invokes `Execute_Execute` via the BlueprintNativeEvent wrapper, unregisters, and verifies `FindTool` returns null. Fastest tool smoke test; useful as a pre-flight before running the full agent loop. | **yes** |
| `Ino.LiteRtLm.ConversationToolTest [prompt]` | **The headline test.** Registers a `UInoLiteRtLmAddNumbersTool`, creates a conversation with `tools_json` + constrained decoding, binds all four delegates (`OnToken` / `OnToolCalled` / `OnComplete` / `OnError`), sends "What is 27 plus 15?", watches the multi-round agent loop run, and logs PASS if `OnToolCalled` fired with `add_numbers` + result `"42"` AND `OnComplete`'s text contains `"42"` or `"forty-two"`. | **yes** |
| `Ino.LiteRtLm.ConversationContextTest` | Exercises `SetSystemContext` / `SetUserContext` end-to-end: injects game state (location, time) and player state (name, class) into the conversation, sends a prompt requiring the context, and checks that the model's response references the injected values. Validates the context → user message prepend pipeline. | **yes** |

Every UE API observer UCLASS uses the same pattern: `NewObject` + `AddToRoot`, bind dynamic delegates via `AddDynamic`, run the workflow, and in `Finish()` call `Conversation->Shutdown()` for deterministic teardown before clearing UPROPERTY refs and `RemoveFromRoot`. Do NOT call `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true)` from inside a delegate handler — parallel GC workers race the in-flight delegate's write access and trip `FMRSWRecursiveAccessDetector`. `Shutdown()` is the safe alternative because it only resets the worker `TUniquePtr`; it never touches delegate state.

All dynamic delegate handlers on observer UCLASSes MUST take `FString` **by value**, not `const FString&`. UE's `BindDynamic` does strict method-pointer matching against the delegate's declared signature, and every delegate in this plugin declares `FString` by value. A handler with `const FString&` compiles fine on its own but fails at the `BindDynamic` call site with a cryptic `cannot convert argument` error.

### Shared helpers + adding new tests

Shared helpers (model path resolution, JSON parsing, tool-call extraction, assistant text extraction) live in `InoSmokeTestCommon.{h,cpp}` under the `InoSmokeTest` namespace. Test-specific helpers live in the test file's anonymous namespace.

To add a new smoke test, drop a new `.cpp` (and optional `.h` for observer UCLASSes) into `Private/SmokeTests/`. UBT auto-picks up `.cpp` files under `Private/`; no `Build.cs` changes needed. `Private/SmokeTests/` is already on the include path via `PrivateIncludePaths`.

Smoke tests are compiled into every build configuration. For now they're gated behind console commands and never run unless explicitly invoked. If any individual test grows shipping-sensitive logic, wrap that file in `#if !UE_BUILD_SHIPPING` as a follow-up change.

## Platform support

### LiteRT-LM (LLM)

| Platform | Status | Build script | Artifacts |
|---|---|---|---|
| **Windows (Win64, MSVC)** | ✅ full (CPU + GPU via D3D12/WebGPU) | `LiteRtLm/scripts/build-win64.ps1` | `LiteRtLm.dll` + `libLiteRt.dll` + 3 prebuilt `.dll` files |
| **Android (arm64-v8a)** | ✅ full (CPU + GPU via OpenCL / WebGPU) | `LiteRtLm/scripts/build-android-arm64.ps1` | `libLiteRtLm.so` + 6 prebuilt `.so` files |
| iOS | ⏳ stubs only | — | `InoLiteRtLmStubs_NonWindows.cpp` returns nullptr |
| Linux | ⏳ stubs only | — | same |
| macOS | ⏳ stubs only | — | same |

### ONNX Runtime

| Platform | Status | Setup script | Artifacts | Available providers |
|---|---|---|---|---|
| **Windows (Win64)** | ✅ full | `OnnxRuntime/scripts/setup-onnxruntime.ps1` | `InoOnnxRuntime.dll` (~14 MB, renamed) | CPU, Azure (DirectML on roadmap) |
| **Android (arm64-v8a)** | ✅ full | same script (downloads AAR) | `libInoOnnxRuntime.so` (~25 MB, renamed) | CPU, XNNPACK, NNAPI, WebGPU |
| iOS | ⏳ not staged | — | — | — |
| Linux | ⏳ not staged | — | — | — |
| macOS | ⏳ not staged | — | — | — |

The UE API (subsystem, conversation, tools, delegates) is **identical across platforms**. Only the platform branches in `InoAgentsLibrary.Build.cs` + `InoOnnxRuntime.Build.cs` differ. On unimplemented platforms, the plugin still links cleanly — calls to `LoadModelAsync` fail gracefully with a "Native engine failed" error via the `FOnInoLiteRtLmModelLoaded` delegate, ORT calls via `FInoOnnxSession::Create` return nullptr with a clear error, and every other feature (ElevenLabs TTS, streaming audio, chat panel) works normally.

### Android specifics

- **NDK coexistence.** UE 5.7 requires NDK **r27.2** (hardcoded in `SetupAndroid.bat`) for the UE C++ build. LiteRT-LM's Bazel build requires NDK **r28b or newer**. The two NDKs install side-by-side under `%LOCALAPPDATA%\Android\Sdk\ndk\` as separate subdirectories and coexist without conflict — UBT uses the 27.2 tree, `build-android-arm64.ps1` auto-detects and uses the newest r28+. Do NOT point both at the same NDK; UE 5.7 is tightly coupled to r27.2 and LiteRT-LM wants r28+.
- **Two Bazel output bases.** `C:/b/ino` for Win64, `C:/b/ino-android` for Android arm64. Same `--disk_cache` pattern, different action-cache keys so the two platform builds don't thrash each other.
- **UPL (Unreal Plugin Language) XML.** `Source/ThirdParty/InoAgentsLibrary/InoAgentsLibrary_UPL_Android.xml` drives the APK packaging — `<soLoadLibrary>` emits `System.loadLibrary()` calls in the Java launcher in the correct order (`libLiteRt` → `libGemmaModelConstraintProvider` → `libLiteRtLm`, because `libLiteRtLm.so` imports from `libLiteRt.so`), and `<resourceCopies>` stages the 5 on-demand GPU accelerator `.so` files into `lib/arm64-v8a/` without explicit preloading (the LiteRT engine `dlopen`s them at runtime when `backend=gpu` is requested).
- **`FInoAgentsModule::StartupModule` path.** Windows code explicitly `FPlatformProcess::GetDllHandle`s five DLLs in a specific order. Android code skips that entirely — the Android linker + UPL's `<soLoadLibrary>` handle preloading before `StartupModule` even runs. StartupModule on Android does the same `litert_lm_set_min_log_level(0)` smoke test as Windows, which proves the link resolved correctly.
- **Two GPU accelerator paths.** Windows ships only WebGPU (→ D3D12 via Dawn). Android ships **both** WebGPU and OpenCL accelerator `.so` files — LiteRT picks whichever works on the target device at runtime. This means the Android APK is larger (~41 MB of prebuilt GPU `.so` files vs ~38 MB on Windows) but works on a wider range of GPUs (older Adreno / Mali that lack WebGPU drivers but have OpenCL).
- **Model file.** The 2.6–5 GB `.litertlm` model file cannot ship inside the APK (Play Store limit is 200 MB base APK). Use the same auto-download-to-`PersistentDownloadDir` mechanism as Windows — `FPaths::ProjectPersistentDownloadDir()` resolves correctly on Android and the UE HTTP module works over Wi-Fi and cellular without any Android-specific setup beyond adding `android.permission.INTERNET` to the project's Android permissions (already done for ElevenLabs).
- **Stubs.** `InoLiteRtLmStubs_NonWindows.cpp` is guarded by `#if !PLATFORM_WINDOWS && !PLATFORM_ANDROID`. When the Android `.so` is present, Android builds link against the real symbols. Remove the guard on a per-platform basis as each new platform is ported.

Future platforms (iOS, Linux, macOS): upstream `.bazelrc` already has `--config=ios_arm64`, `build:linux_x86_64`, and `build:macos_arm64`. The pattern will be the same — write a `build-<platform>.ps1`, add a platform branch in `InoAgentsLibrary.Build.cs`, tighten the stubs guard.

## Windows gotchas

- **Five runtime DLLs to ship alongside the executable.** All must end up in `Binaries/ThirdParty/InoAgentsLibrary/Win64/`:
  - `LiteRtLm.dll` (~14 MB) — our Bazel-built wrapper, dynamically links against `libLiteRt.dll`
  - `libLiteRt.dll` (~11 MB) — LiteRT core runtime (Bazel-built; produced because we pass `--define=litert_link_capi_so=true`)
  - `libGemmaModelConstraintProvider.dll` (~13 MB) — upstream prebuilt constraint provider
  - `libLiteRtWebGpuAccelerator.dll` (~21 MB) — upstream prebuilt WebGPU → D3D12 accelerator
  - `libLiteRtTopKWebGpuSampler.dll` (~17 MB) — upstream prebuilt GPU top-K sampler
  All five are handled by `build-win64.ps1` on the Bazel side and by `InoAgentsLibrary.Build.cs` on the UE side (listed in both `PublicDelayLoadDLLs` and `RuntimeDependencies`).
- **DLL load order is critical.** With `--define=litert_link_capi_so=true`, `LiteRtLm.dll` imports from `libLiteRt.dll`. Windows resolves imports at `LoadLibrary` time, so `FInoAgentsModule::StartupModule` must load `libLiteRt.dll` **before** `LiteRtLm.dll` or the load fails with `GetLastError=126` (missing import). Order: `libGemmaModelConstraintProvider.dll` → `libLiteRt.dll` → `LiteRtLm.dll` → two GPU accelerator DLLs.
- **GPU accelerator DLLs must be pre-loaded too.** LiteRT's engine internally calls `LoadLibraryA("libLiteRtWebGpuAccelerator.dll")` by filename when `backend=gpu` is requested. Windows searches relative to the process executable (UE's `Engine/Binaries/Win64/`), not the plugin's DLL directory. If the GPU DLLs aren't pre-loaded with full paths first, the engine's internal `LoadLibraryA` returns null and GPU init crashes silently. Pre-loading by full path puts the module into the process's cached loaded-modules table, and subsequent `LoadLibraryA` calls by filename resolve to the already-loaded module.
- **Dual LiteRT instance problem.** Before `--define=litert_link_capi_so=true`, our `LiteRtLm.dll` statically linked the full LiteRT core, AND `libLiteRt.dll` was also loaded for the GPU accelerators. Two copies of LiteRT in the same process caused heap corruption when objects crossed the boundary. The `litert_link_capi_so=true` build tells Bazel to externalize the LiteRT core into `libLiteRt.dll` so everyone (our wrapper, the GPU DLLs) shares one instance. Do NOT remove this define or GPU inference crashes.
- **Delay-load the DLLs.** `InoAgentsLibrary.Build.cs` uses `PublicDelayLoadDLLs.Add(...)` for all five DLLs so the game / editor launches even if they're missing. `StartupModule` calls `FPlatformProcess::GetDllHandle` explicitly and surfaces a `UE_LOG` error on failure — no `FMessageDialog` fallback.
- **MSVC runtime.** Build with `/MD` (dynamic CRT) to match UE. `/MT` would link successfully but produce two CRTs in the same process at runtime, causing silent heap corruption across allocator boundaries. Upstream `build:windows` already handles this correctly — no explicit override needed in our overlay.
- **Force-reference the C API symbols.** See "Custom Bazel target → Why `LiteRtLm_exports.cc` exists". Without this, the DLL builds but exports no `litert_lm_*` functions because MSVC drops unreferenced `.obj` files from static libraries, and upstream disables `--whole-archive` on Windows.

## Model file distribution

Gemma 4 `.litertlm` model files are 2.5–5 GB and **must never be committed**. Models are auto-downloaded on first use from URLs configured in Project Settings → Plugins → InoAgents → LiteRT-LM → Models.

### Model path resolution

`LiteRtLmResolveModelPath(ModelFileName)` (in `InoLiteRtLmTypes.h/.cpp`) checks two locations in order:

1. **`FPaths::ProjectPersistentDownloadDir() / "InoAgents/Models/"`** — where auto-downloaded models are cached. This is UE's canonical location for runtime-acquired content that persists across sessions and app updates. Platform-appropriate (sandboxed on mobile, app-support on macOS).
2. **`Plugins/InoAgents/Models/`** — legacy dev-time path. The plugin's `.gitignore` excludes `Models/` so the 2.5+ GB file never lands in git.

If neither location has the file, `UInoLiteRtLmSubsystem::LoadModelAsync` looks up the `ModelFileName` in the `UInoAgentsSettings::Models` array to find the download URL, then downloads via `FHttpModule` and saves to `PersistentDownloadDir`. The subsystem fires `OnDownloadProgress(Percent, BytesReceived, TotalBytes)` during download for loading screens.

### Model config

Models are configured via `FInoLiteRtLmModelConfig` — a **plain USTRUCT** (not a UDataAsset). Set `ModelFileName`, `Backend`, `MaxNumTokens`, `SystemMessage` directly on the agent component's details panel, or build one in Blueprint via a Make node and pass to `LoadModelAsync`.

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

## How to update LiteRT-LM

```
cd Plugins/InoAgents/LiteRtLm/scripts
./update-litert.ps1 v0.11.0     # or any tag
```

The script:
1. `cd vendor/LiteRT-LM && git fetch --tags && git checkout <tag>`
2. Updates `LITERT_LM_TAG` file
3. Re-runs overlay application (`setup.ps1`)
4. Re-runs `build-win64.ps1`
5. Reports the new SHA for you to commit

**Never** stage the submodule pointer bump without re-running the build to verify upstream still compiles. LiteRT-LM is pre-1.0; breaking changes between tags are possible.

## How to update ONNX Runtime

```
# 1. Edit the pinned version
edit Plugins/InoAgents/OnnxRuntime/ONNXRUNTIME_VERSION    # e.g. 1.25.0

# 2. Re-stage (downloads new artifacts, renames to InoOnnxRuntime.dll / libInoOnnxRuntime.so)
cd Plugins/InoAgents/OnnxRuntime/scripts
./setup-onnxruntime.ps1

# 3. Rebuild the editor + repackage for Android, then run the smoke tests:
#      Ino.Onnx.ProvidersTest
#      Ino.Onnx.SessionFromFileTest <a known-good .onnx>

# 4. Commit the ONNXRUNTIME_VERSION bump + any Build.cs tweaks + the newly staged binaries.
```

Watch-outs when bumping:

- Android releases on Maven Central sometimes lag the Windows GitHub Releases by a week. If the Maven 404s for your target version, either wait or down-grade Windows to match.
- ORT 1.x has generally maintained API backwards compatibility (our `GetApi(ORT_API_VERSION)` call asks for the compile-time version; older DLLs return nullptr, newer DLLs return a subset-compatible OrtApi). If a bump breaks compilation, it's usually a removed-in-major or a new `[[nodiscard]]` annotation — the `CheckOrtStatus` helper should already catch the latter.
- If the NNE-bundled ORT in a future UE release matches or exceeds our pinned version, the rename remains the right isolation. Do not remove it.

## What to verify before trusting this file

This file describes design decisions and architectural intent. Specifics drift over time. Before acting on any specific claim:

- **LiteRT-LM version:** check `LiteRtLm/vendor/LiteRT-LM/.bazelversion` and `git -C LiteRtLm/vendor/LiteRT-LM describe --tags` for the actual pinned version.
- **ONNX Runtime version:** check `OnnxRuntime/ONNXRUNTIME_VERSION`. If the staged binaries disagree with the pin, the setup script needs to be re-run.
- **Actual Bazel target names:** read `LiteRtLm/vendor/LiteRT-LM/c/BUILD` and `runtime/engine/BUILD` — target names may have moved between versions.
- **The public C API:** read `LiteRtLm/vendor/LiteRT-LM/c/engine.h` directly. If the symbol names or signatures differ from what this file describes, trust the header.
- **The ONNX Runtime C API:** read `Source/ThirdParty/InoOnnxRuntime/Public/onnxruntime_c_api.h`. Struct field additions across ORT versions are common; the `OrtApi` vtable is versioned so older code still works, but new features require bumping `ORT_API_VERSION` checks in our code.
- **Plugin scaffold state:** open `Source/ThirdParty/InoAgentsLibrary/InoAgentsLibrary.Build.cs` and `Source/ThirdParty/InoOnnxRuntime/InoOnnxRuntime.Build.cs`. If either references the pre-rename / pre-dynamic-loading pattern, a regression slipped through — both should look like "no `PublicAdditionalLibraries`, no `PublicDelayLoadDLLs`, dynamic load only".
- **Renamed ORT DLL / .so still in place:** `Binaries/ThirdParty/InoOnnxRuntime/Win64/InoOnnxRuntime.dll` and `.../Android/arm64-v8a/libInoOnnxRuntime.so`. Do NOT undo the rename — the "why" is in the "ONNX Runtime (the second runtime)" section above.
- **Tooling versions:** Gemma 4 variant specs and modality support may have evolved — confirm against https://ai.google.dev/gemma/docs/core.
