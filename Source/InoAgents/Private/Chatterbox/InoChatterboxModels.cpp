// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxModels.h"

#include "InoAgentsLog.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
    /**
     * Build an FInoOnnxSessionOptions with Chatterbox-appropriate
     * defaults for the current platform, layered with the caller's
     * FInoChatterboxPerformanceOptions.
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
     * FInoChatterboxPerformanceOptions). When bForceCpu=false the
     * session follows the default platform + Performance policy.
     */
    FInoOnnxSessionOptions MakeChatterboxOptions(
        const FInoChatterboxPerformanceOptions& Performance,
        bool bForceCpu)
    {
        FInoOnnxSessionOptions Options;
        Options.GraphOptimization = EInoOnnxGraphOptimizationLevel::All;

#if PLATFORM_ANDROID
        // Android: XNNPACK (ARM-NEON-optimized CPU kernels, measurably
        // faster than the generic CPU provider on aarch64) + CPU as the
        // guaranteed fallback. DirectML is D3D12-only, no Android analog.
        // bForceCpu is a no-op here — Android already skips DML entirely.
        // Future: WebGPU + NNAPI are in the AAR and could be opt-in.
        Options.ExecutionProviders = {
            EInoOnnxProvider::Xnnpack,
            EInoOnnxProvider::Cpu
        };
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

        return Options;
    }

    /**
     * Create one session from a <component>_<variant>.onnx file under
     * BaseDir. Returns nullptr and writes to OutError on failure.
     *
     * Component is the Chatterbox logical piece name: "speech_encoder",
     * "embed_tokens", "language_model", or "conditional_decoder".
     */
    TUniquePtr<FInoOnnxSession> LoadChatterboxSession(
        const FString& BaseDir,
        const FString& Variant,
        const TCHAR* Component,
        const FInoChatterboxPerformanceOptions& Performance,
        bool bForceCpu,
        FString* OutError)
    {
        const FString FileName = FString::Printf(TEXT("%s_%s.onnx"), Component, *Variant);
        const FString FullPath = FPaths::Combine(BaseDir, FileName);

        if (!IFileManager::Get().FileExists(*FullPath))
        {
            const FString Err = FString::Printf(
                TEXT("Chatterbox %s model not found at %s. ")
                TEXT("Run Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 ")
                TEXT("(or trigger the runtime downloader once Phase D lands)."),
                Component, *FullPath);
            if (OutError) *OutError = Err;
            UE_LOG(LogInoAgents, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        const FInoOnnxSessionOptions Options = MakeChatterboxOptions(Performance, bForceCpu);
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
        StrategyLabel = TEXT("xnnpack");
#endif

        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: loaded %s (%s, strategy=%s) in %.1f ms — %d inputs, %d outputs"),
               Component, *Variant, StrategyLabel, ElapsedMs,
               Session->GetInputCount(), Session->GetOutputCount());

        return Session;
    }
}

// ============================================================================
//  FInoChatterboxModels
// ============================================================================

TUniquePtr<FInoChatterboxModels> FInoChatterboxModels::LoadFromDir(
    const FString& BaseDir,
    const FString& Variant,
    FString* OutError,
    const FInoChatterboxPerformanceOptions& Performance)
{
    if (BaseDir.IsEmpty() || Variant.IsEmpty())
    {
        const FString Err(TEXT("FInoChatterboxModels::LoadFromDir: BaseDir or Variant is empty."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoAgents, Error, TEXT("%s"), *Err);
        return nullptr;
    }

    if (!FPaths::DirectoryExists(BaseDir))
    {
        const FString Err = FString::Printf(
            TEXT("FInoChatterboxModels::LoadFromDir: directory does not exist: %s. ")
            TEXT("Run setup-chatterbox.ps1 first or let the runtime downloader populate it."),
            *BaseDir);
        if (OutError) *OutError = Err;
        UE_LOG(LogInoAgents, Error, TEXT("%s"), *Err);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: loading models from %s (variant=%s, intra=%d, inter=%d, profiling=%s, dml=%s, adapter=%d, ")
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

    TUniquePtr<FInoChatterboxModels> Bundle(new FInoChatterboxModels());
    Bundle->Variant = Variant;
    Bundle->BaseDir = BaseDir;

    // Load each of the four runtime sessions. Any failure short-circuits
    // the whole bundle — a half-loaded set is never useful. Order matches
    // the official reference script (speech_encoder first so voice
    // conditioning is ready before the AR loop needs it).
    //
    // Per-session CPU overrides from Performance.b{Component}OnCpu flow
    // through to LoadChatterboxSession's bForceCpu parameter. See the
    // FInoChatterboxPerformanceOptions header for the "why this exists"
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
           TEXT("Chatterbox: all four %s sessions loaded successfully."),
           *Variant);

    return Bundle;
}

void FInoChatterboxModels::LogMetadata() const
{
    if (!SpeechEncoder.IsValid() || !EmbedTokens.IsValid()
        || !LanguageModel.IsValid() || !ConditionalDecoder.IsValid())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("FInoChatterboxModels::LogMetadata: bundle is incomplete."));
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("========================================"));
    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox (%s) model metadata"), *Variant);
    UE_LOG(LogInoAgents, Log, TEXT("========================================"));

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- speech_encoder ---"));
    SpeechEncoder->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- embed_tokens ---"));
    EmbedTokens->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- language_model ---"));
    LanguageModel->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- conditional_decoder ---"));
    ConditionalDecoder->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT("========================================"));
}
