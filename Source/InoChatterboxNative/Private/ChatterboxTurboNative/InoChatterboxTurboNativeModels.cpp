// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxTurboNativeModels.h"

#include "InoAgentsLog.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
    /**
     * Build an FInoOnnxSessionOptions with Chatterbox-appropriate
     * defaults for the current platform, layered with the caller's
     * FInoChatterboxTurboNativePerformanceOptions.
     *
     * Chatterbox's four runtime models are all large-ish autoregressive
     * / convolutional pieces where graph optimization and fast CPU
     * kernels matter a lot. We always enable ALL graph optimizations.
     *
     * Provider priority:
     *   Windows: CPU only for now. DirectML (GPU via D3D12) is a planned
     *            follow-up. WebGPU would be an option on Windows too but
     *            we haven't validated the combination with Chatterbox's
     *            dynamic shapes yet.
     *   Android: XNNPACK first (ARM-NEON-optimized CPU kernels; measurably
     *            faster than the generic CPU provider on aarch64), then
     *            CPU as the guaranteed fallback. NNAPI is available but
     *            we skip it because inconsistency across OEM drivers
     *            (the same class of issue we documented in the ONNX
     *            Runtime section of CLAUDE.md).
     *
     * Performance overlay: IntraOpThreadCount / InterOpThreadCount /
     * bEnableOrtProfiling from the input struct are forwarded verbatim
     * to FInoOnnxSessionOptions. Values of 0 mean "ORT default" and
     * are left untouched.
     */
    /**
     * Build session options for one Chatterbox ORT session.
     *
     * bForceCpu=true routes this specific session to the CPU provider,
     * overriding Performance.bPreferDirectMl. Used for sessions known
     * to have DML compatibility issues (see the per-session flags on
     * FInoChatterboxTurboNativePerformanceOptions). When bForceCpu=false the
     * session follows the default platform + Performance policy.
     */
    FInoOnnxSessionOptions MakeChatterboxOptions(
        const FInoChatterboxTurboNativePerformanceOptions& Performance,
        bool bForceCpu)
    {
        FInoOnnxSessionOptions Options;

        // Graph optimization level. On Windows we run at ORT_ENABLE_ALL
        // (level 3 — includes layout-transformation passes) because the
        // MSVC-side ORT build has full kernel coverage across the
        // Microsoft-internal domains those passes produce.
        //
        // On Android the 1.24.3 AAR is missing kernel registrations in
        // two places that Chatterbox q4f16 hits, so we have to lower
        // the level:
        //
        //   Level 3 (All) → produces 'com.ms.internal.nhwc' domain ops
        //     (via the NhwcTransformer). The AAR lacks
        //     com.ms.internal.nhwc.AveragePool(19), failing the
        //     speech_encoder load:
        //       "Failed to find kernel for com.ms.internal.nhwc.AveragePool(19)"
        //
        //   Level 2 (Extended) → runs op-level fusions including Bias +
        //     Gelu → BiasGelu. The AAR only registers fp32 BiasGelu, and
        //     q4f16's conditional_decoder hands it fp16 tensors:
        //       "Failed to find kernel for com.microsoft.BiasGelu(1)
        //        ... implemented only for tensor(float), but node has
        //        tensor(float16)"
        //
        // Drop to ORT_ENABLE_BASIC (level 1) — constant folding, dead-
        // node elimination, basic transpose optimization. We lose the
        // fusions, so ops like Gelu + Bias run separately (both have
        // fp16 kernels on Android), and no NHWC rewrites happen. Perf
        // cost is real but bounded — a well-quantized Chatterbox q4f16
        // on CPU runs mostly matmul + attention, which aren't fusion-
        // heavy. Separate Bias + Gelu is a handful of extra kernel
        // launches per forward pass, not a vectorization loss.
        //
        // Alternative to re-investigate once Android synth is healthy:
        // per-session opt level (speech_encoder + language_model at
        // Extended, conditional_decoder at Basic) to squeeze some perf
        // back. Not worth the complexity while the baseline is still
        // unverified.
        //
        // This is orthogonal to bForceCpu — graph optimizer passes run
        // at graph-build time based on opt level, not on which
        // providers are registered.
#if PLATFORM_ANDROID
        Options.GraphOptimization = EInoOnnxGraphOptimizationLevel::Basic;
#else
        Options.GraphOptimization = EInoOnnxGraphOptimizationLevel::All;
#endif

#if PLATFORM_ANDROID
        // Android: XNNPACK (ARM-NEON-optimized CPU kernels) when allowed,
        // else CPU-only. DirectML is D3D12-only — no Android analog —
        // so the Performance.bPreferDirectMl flag is ignored here.
        //
        // XNNPACK catch: enabling it requests the NhwcTransformer pass,
        // which rewrites some ops into the 'com.ms.internal.nhwc'
        // domain. The Android AAR of ORT 1.24.3 is missing a kernel
        // registration for `com.ms.internal.nhwc.AveragePool(19)` —
        // the one the speech_encoder's x-vector block uses — and
        // session creation fails with
        //     "Failed to find kernel for com.ms.internal.nhwc.AveragePool(19)
        //      ... Version mismatch. node_version: 19 kernel start version: 11"
        // even though the range nominally covers 19. The fallback to
        // CPU within the same transformed domain trips the same miss.
        //
        // Until the upstream kernel-registration gap closes (or we
        // figure out how to opt a specific session out of the NHWC
        // transformer without dropping XNNPACK wholesale), we reuse
        // the same bForceCpu opt-out the Windows DML path uses:
        //   bForceCpu=true  -> CPU only (no NHWC rewrite, guaranteed to load)
        //   bForceCpu=false -> XNNPACK + CPU fallback (faster for the
        //                      sessions where it works)
        // With the defaults (all four per-session flags on
        // FInoChatterboxTurboNativePerformanceOptions set to true), every session
        // gets CPU-only on Android. Flip specific sessions to false
        // once you've verified they don't hit the NHWC trap.
        if (bForceCpu)
        {
            Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
        }
        else
        {
            Options.ExecutionProviders = {
                EInoOnnxProvider::Xnnpack,
                EInoOnnxProvider::Cpu
            };
        }
#elif PLATFORM_WINDOWS
        // Windows: DirectML (GPU / NPU via D3D12) if requested AND this
        // session isn't explicitly forced to CPU, else CPU only.
        //
        // Fall-through to CPU within DML+CPU is also automatic — if DML
        // registration fails at session creation (no D3D12 device, etc.)
        // ORT silently uses CPU and the session still works.
        if (Performance.bPreferDirectMl && !bForceCpu)
        {
            Options.ExecutionProviders = {
                EInoOnnxProvider::DirectMl,
                EInoOnnxProvider::Cpu
            };
            Options.DirectMlAdapterIndex = Performance.DirectMlAdapterIndex;
        }
        else
        {
            Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
        }
#else
        // Linux / macOS / iOS: CPU only — we haven't staged ORT for
        // those platforms. Anything running on this branch would need
        // a corresponding setup-script and Build.cs addition first.
        Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
#endif

        // Apply caller-supplied tuning. Zero values pass through as
        // "ORT default" — FInoOnnxSession::Create checks > 0 before
        // forwarding to OrtSessionOptionsSetIntraOpNumThreads etc.
        Options.IntraOpThreadCount = Performance.IntraOpThreadCount;
        Options.InterOpThreadCount = Performance.InterOpThreadCount;
        Options.bEnableProfiling   = Performance.bEnableOrtProfiling;

        // -1 = ORT default (WARNING). 0 = verbose — pipe through the
        // Blueprint toggle so users can enable session-level logs when
        // debugging DML or graph-transform issues without touching code.
        Options.LogSeverityLevel = Performance.bEnableVerboseOrtLogging ? 0 : -1;

        return Options;
    }

    /**
     * Create one session from a <component>_<variant>.onnx file under
     * BaseDir. Returns nullptr and writes to OutError on failure.
     *
     * Component is the Chatterbox logical piece name: "speech_encoder",
     * "embed_tokens", "language_model", or "conditional_decoder".
     */
    /** Filename suffix for a variant string: empty for "fp32" (HF
     *  baseline naming), "_" + variant for every other string. Mirrors
     *  ChatterboxVariantToFileSuffix(enum) but operates on the string
     *  form so the existing LoadChatterboxSession(Variant as FString)
     *  signature keeps working. Both sides MUST agree — any session
     *  load path composing filenames needs to use this rule, otherwise
     *  a direct "%s_%s.onnx" composition produces bogus names like
     *  "speech_encoder_fp32.onnx" that don't exist in the HF repo. */
    FString VariantSuffixFromString(const FString& Variant)
    {
        return Variant.Equals(TEXT("fp32"), ESearchCase::IgnoreCase)
            ? FString()
            : FString::Printf(TEXT("_%s"), *Variant);
    }

    TUniquePtr<FInoOnnxSession> LoadChatterboxSession(
        const FString& BaseDir,
        const FString& Variant,
        const TCHAR* Component,
        const FInoChatterboxTurboNativePerformanceOptions& Performance,
        bool bForceCpu,
        FString* OutError)
    {
        const FString FileName = FString::Printf(
            TEXT("%s%s.onnx"), Component, *VariantSuffixFromString(Variant));
        const FString FullPath = FPaths::Combine(BaseDir, FileName);

        if (!IFileManager::Get().FileExists(*FullPath))
        {
            const FString Err = FString::Printf(
                TEXT("%s model not found at %s. ")
                TEXT("Run Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 ")
                TEXT("(or trigger the runtime downloader once Phase D lands)."),
                Component, *FullPath);
            if (OutError) *OutError = Err;
            UE_LOG(LogInoAgents, Error, TEXT("Chatterbox: Session: %s"), *Err);
            return nullptr;
        }

        const FInoOnnxSessionOptions Options = MakeChatterboxOptions(Performance, bForceCpu);

        // Log the session options we're about to apply — useful for
        // diagnosing "which provider is this session actually on" at a
        // glance, separately from the post-load success log. Kept at
        // Verbose because it's four lines per load and the post-load
        // strategy= summary covers the common-case need.
        FString ProviderList;
        for (int32 i = 0; i < Options.ExecutionProviders.Num(); ++i)
        {
            if (i > 0) { ProviderList += TEXT(","); }
            switch (Options.ExecutionProviders[i])
            {
                case EInoOnnxProvider::Cpu:       ProviderList += TEXT("cpu"); break;
                case EInoOnnxProvider::Xnnpack:   ProviderList += TEXT("xnnpack"); break;
                case EInoOnnxProvider::Nnapi:     ProviderList += TEXT("nnapi"); break;
                case EInoOnnxProvider::WebGpu:    ProviderList += TEXT("webgpu"); break;
                case EInoOnnxProvider::DirectMl:  ProviderList += TEXT("dml"); break;
                case EInoOnnxProvider::Cuda:      ProviderList += TEXT("cuda"); break;
                case EInoOnnxProvider::TensorRt:  ProviderList += TEXT("tensorrt"); break;
                default:                          ProviderList += TEXT("?"); break;
            }
        }
        UE_LOG(LogInoAgents, Verbose,
               TEXT("Chatterbox: Session: MakeChatterboxOptions for '%s' ")
               TEXT("(providers=[%s], force_cpu=%s, graph_opt=%d)"),
               Component, *ProviderList,
               bForceCpu ? TEXT("yes") : TEXT("no"),
               (int32)Options.GraphOptimization);

        const double TStart = FPlatformTime::Seconds();

        FString SessionError;
        TUniquePtr<FInoOnnxSession> Session = FInoOnnxSession::Create(FullPath, Options, &SessionError);

        if (!Session.IsValid())
        {
            const FString Err = FString::Printf(
                TEXT("Failed to create ORT session for Chatterbox %s: %s"),
                Component, *SessionError);
            if (OutError) *OutError = Err;
            // FInoOnnxSession::Create already logged the error; don't duplicate.
            return nullptr;
        }

        const double ElapsedMs = (FPlatformTime::Seconds() - TStart) * 1000.0;

        // Log which provider strategy this session is using so the user
        // can tell at a glance whether per-session CPU overrides landed
        // the way they expected. "dml" means DML was requested (may still
        // fall back per-op internally); "cpu" means forced CPU only.
        const TCHAR* StrategyLabel = TEXT("cpu");
#if PLATFORM_WINDOWS
        if (Performance.bPreferDirectMl && !bForceCpu)
        {
            StrategyLabel = TEXT("dml");
        }
#elif PLATFORM_ANDROID
        if (!bForceCpu)
        {
            StrategyLabel = TEXT("xnnpack");
        }
#endif

        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Session: loaded %s (%s, strategy=%s) in %.1f ms -- %d inputs, %d outputs"),
               Component, *Variant, StrategyLabel, ElapsedMs,
               Session->GetInputCount(), Session->GetOutputCount());

        return Session;
    }
}

// ============================================================================
//  FInoChatterboxTurboNativeModels
// ============================================================================

TUniquePtr<FInoChatterboxTurboNativeModels> FInoChatterboxTurboNativeModels::LoadFromDir(
    const FString& BaseDir,
    const FString& Variant,
    FString* OutError,
    const FInoChatterboxTurboNativePerformanceOptions& Performance)
{
    if (BaseDir.IsEmpty() || Variant.IsEmpty())
    {
        const FString Err(TEXT("LoadFromDir: BaseDir or Variant is empty."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoAgents, Error, TEXT("Chatterbox: Session: %s"), *Err);
        return nullptr;
    }

    if (!FPaths::DirectoryExists(BaseDir))
    {
        const FString Err = FString::Printf(
            TEXT("LoadFromDir: directory does not exist: %s. ")
            TEXT("Run setup-chatterbox.ps1 first or let the runtime downloader populate it."),
            *BaseDir);
        if (OutError) *OutError = Err;
        UE_LOG(LogInoAgents, Error, TEXT("Chatterbox: Session: %s"), *Err);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Session: loading models from %s (variant=%s, intra=%d, inter=%d, profiling=%s, dml=%s, adapter=%d, ")
           TEXT("cpu-overrides: enc=%s embed=%s lm=%s dec=%s)..."),
           *BaseDir, *Variant,
           Performance.IntraOpThreadCount,
           Performance.InterOpThreadCount,
           Performance.bEnableOrtProfiling ? TEXT("yes") : TEXT("no"),
           Performance.bPreferDirectMl ? TEXT("yes") : TEXT("no"),
           Performance.DirectMlAdapterIndex,
           Performance.bSpeechEncoderOnCpu      ? TEXT("yes") : TEXT("no"),
           Performance.bEmbedTokensOnCpu        ? TEXT("yes") : TEXT("no"),
           Performance.bLanguageModelOnCpu      ? TEXT("yes") : TEXT("no"),
           Performance.bConditionalDecoderOnCpu ? TEXT("yes") : TEXT("no"));

    TUniquePtr<FInoChatterboxTurboNativeModels> Bundle(new FInoChatterboxTurboNativeModels());
    Bundle->Variant = Variant;
    Bundle->BaseDir = BaseDir;

    // Load each of the four runtime sessions. Any failure short-circuits
    // the whole bundle — a half-loaded set is never useful. Order matches
    // the official reference script (speech_encoder first so voice
    // conditioning is ready before the AR loop needs it).
    //
    // Per-session CPU overrides from Performance.b{Component}OnCpu flow
    // through to LoadChatterboxSession's bForceCpu parameter. See the
    // FInoChatterboxTurboNativePerformanceOptions header for the "why this exists"
    // context and the per-session DML compatibility notes.
    Bundle->SpeechEncoder = LoadChatterboxSession(
        BaseDir, Variant, TEXT("speech_encoder"), Performance,
        Performance.bSpeechEncoderOnCpu, OutError);
    if (!Bundle->SpeechEncoder.IsValid())
    {
        return nullptr;
    }

    Bundle->EmbedTokens = LoadChatterboxSession(
        BaseDir, Variant, TEXT("embed_tokens"), Performance,
        Performance.bEmbedTokensOnCpu, OutError);
    if (!Bundle->EmbedTokens.IsValid())
    {
        return nullptr;
    }

    Bundle->LanguageModel = LoadChatterboxSession(
        BaseDir, Variant, TEXT("language_model"), Performance,
        Performance.bLanguageModelOnCpu, OutError);
    if (!Bundle->LanguageModel.IsValid())
    {
        return nullptr;
    }

    Bundle->ConditionalDecoder = LoadChatterboxSession(
        BaseDir, Variant, TEXT("conditional_decoder"), Performance,
        Performance.bConditionalDecoderOnCpu, OutError);
    if (!Bundle->ConditionalDecoder.IsValid())
    {
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Session: all four %s sessions loaded successfully."),
           *Variant);

    return Bundle;
}

TUniquePtr<FInoChatterboxTurboNativeModels> FInoChatterboxTurboNativeModels::LoadPerSession(
    const FSessionLoadSpec& Enc,
    const FSessionLoadSpec& Embed,
    const FSessionLoadSpec& LM,
    const FSessionLoadSpec& Dec,
    FString*                                OutError,
    const FInoChatterboxTurboNativePerformanceOptions& Performance)
{
    auto ValidateSpec = [&](const FSessionLoadSpec& Spec,
                            const TCHAR* Component,
                            FString* Err) -> bool
    {
        if (Spec.Dir.IsEmpty() || Spec.Variant.IsEmpty())
        {
            if (Err)
            {
                *Err = FString::Printf(
                    TEXT("FInoChatterboxTurboNativeModels::LoadPerSession: %s has empty Dir or Variant."),
                    Component);
            }
            UE_LOG(LogInoAgents, Error,
                   TEXT("Chatterbox: Session: LoadPerSession -- %s spec invalid (Dir='%s', Variant='%s')"),
                   Component, *Spec.Dir, *Spec.Variant);
            return false;
        }
        if (!FPaths::DirectoryExists(Spec.Dir))
        {
            if (Err)
            {
                *Err = FString::Printf(
                    TEXT("LoadPerSession: %s directory missing: %s"),
                    Component, *Spec.Dir);
            }
            UE_LOG(LogInoAgents, Error,
                   TEXT("Chatterbox: Session: LoadPerSession -- %s directory missing: %s"),
                   Component, *Spec.Dir);
            return false;
        }
        return true;
    };

    if (!ValidateSpec(Enc,   TEXT("speech_encoder"),      OutError)) return nullptr;
    if (!ValidateSpec(Embed, TEXT("embed_tokens"),        OutError)) return nullptr;
    if (!ValidateSpec(LM,    TEXT("language_model"),      OutError)) return nullptr;
    if (!ValidateSpec(Dec,   TEXT("conditional_decoder"), OutError)) return nullptr;

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Session: per-session load -- enc=%s embed=%s lm=%s dec=%s, ")
           TEXT("intra=%d inter=%d profiling=%s dml=%s adapter=%d ")
           TEXT("cpu-overrides: enc=%s embed=%s lm=%s dec=%s"),
           *Enc.Variant, *Embed.Variant, *LM.Variant, *Dec.Variant,
           Performance.IntraOpThreadCount,
           Performance.InterOpThreadCount,
           Performance.bEnableOrtProfiling ? TEXT("yes") : TEXT("no"),
           Performance.bPreferDirectMl     ? TEXT("yes") : TEXT("no"),
           Performance.DirectMlAdapterIndex,
           Performance.bSpeechEncoderOnCpu      ? TEXT("yes") : TEXT("no"),
           Performance.bEmbedTokensOnCpu        ? TEXT("yes") : TEXT("no"),
           Performance.bLanguageModelOnCpu      ? TEXT("yes") : TEXT("no"),
           Performance.bConditionalDecoderOnCpu ? TEXT("yes") : TEXT("no"));

    TUniquePtr<FInoChatterboxTurboNativeModels> Bundle(new FInoChatterboxTurboNativeModels());
    // Variant + BaseDir store the speech_encoder's values as the
    // "representative" — used by UI / logs that want a single string
    // for display ("what's loaded?"). For multi-variant bundles the
    // getters are best-effort; callers that need per-session info
    // should go through the session accessors directly.
    Bundle->Variant = Enc.Variant;
    Bundle->BaseDir = Enc.Dir;

    Bundle->SpeechEncoder = LoadChatterboxSession(
        Enc.Dir, Enc.Variant, TEXT("speech_encoder"), Performance,
        Performance.bSpeechEncoderOnCpu, OutError);
    if (!Bundle->SpeechEncoder.IsValid())
    {
        return nullptr;
    }

    Bundle->EmbedTokens = LoadChatterboxSession(
        Embed.Dir, Embed.Variant, TEXT("embed_tokens"), Performance,
        Performance.bEmbedTokensOnCpu, OutError);
    if (!Bundle->EmbedTokens.IsValid())
    {
        return nullptr;
    }

    Bundle->LanguageModel = LoadChatterboxSession(
        LM.Dir, LM.Variant, TEXT("language_model"), Performance,
        Performance.bLanguageModelOnCpu, OutError);
    if (!Bundle->LanguageModel.IsValid())
    {
        return nullptr;
    }

    Bundle->ConditionalDecoder = LoadChatterboxSession(
        Dec.Dir, Dec.Variant, TEXT("conditional_decoder"), Performance,
        Performance.bConditionalDecoderOnCpu, OutError);
    if (!Bundle->ConditionalDecoder.IsValid())
    {
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Session: all four sessions loaded (per-session)."));

    return Bundle;
}

void FInoChatterboxTurboNativeModels::LogMetadata() const
{
    if (!SpeechEncoder.IsValid() || !EmbedTokens.IsValid()
        || !LanguageModel.IsValid() || !ConditionalDecoder.IsValid())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("Chatterbox: Session: LogMetadata -- bundle is incomplete."));
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: ======================================"));
    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: (%s) model metadata"), *Variant);
    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: ======================================"));

    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: --- speech_encoder ---"));
    SpeechEncoder->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: --- embed_tokens ---"));
    EmbedTokens->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: --- language_model ---"));
    LanguageModel->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: --- conditional_decoder ---"));
    ConditionalDecoder->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox: Session: ======================================"));
}
