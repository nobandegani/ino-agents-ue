// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxModels.h"

#include "InoAgentsLog.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
    /**
     * Build an FInoOnnxSessionOptions with Chatterbox-appropriate
     * defaults for the current platform.
     *
     * Chatterbox's three runtime models are all large-ish autoregressive
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
     */
    FInoOnnxSessionOptions MakeChatterboxOptions()
    {
        FInoOnnxSessionOptions Options;
        Options.GraphOptimization = EInoOnnxGraphOptimizationLevel::All;

#if PLATFORM_ANDROID
        Options.ExecutionProviders = {
            EInoOnnxProvider::Xnnpack,
            EInoOnnxProvider::Cpu
        };
#else
        Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
#endif

        return Options;
    }

    /**
     * Create one session from a <component>_<variant>.onnx file under
     * BaseDir. Returns nullptr and writes to OutError on failure.
     *
     * Component is the Chatterbox logical piece name: "language_model",
     * "embed_tokens", or "conditional_decoder".
     */
    TUniquePtr<FInoOnnxSession> LoadChatterboxSession(
        const FString& BaseDir,
        const FString& Variant,
        const TCHAR* Component,
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

        const FInoOnnxSessionOptions Options = MakeChatterboxOptions();
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
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: loaded %s (%s) in %.1f ms — %d inputs, %d outputs"),
               Component, *Variant, ElapsedMs,
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
    FString* OutError)
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
           TEXT("Chatterbox: loading models from %s (variant=%s)..."),
           *BaseDir, *Variant);

    TUniquePtr<FInoChatterboxModels> Bundle(new FInoChatterboxModels());
    Bundle->Variant = Variant;
    Bundle->BaseDir = BaseDir;

    // Load each of the three runtime sessions. Any failure short-circuits
    // the whole bundle — a half-loaded set is never useful.
    Bundle->LanguageModel = LoadChatterboxSession(BaseDir, Variant, TEXT("language_model"), OutError);
    if (!Bundle->LanguageModel.IsValid())
    {
        return nullptr;
    }

    Bundle->EmbedTokens = LoadChatterboxSession(BaseDir, Variant, TEXT("embed_tokens"), OutError);
    if (!Bundle->EmbedTokens.IsValid())
    {
        return nullptr;
    }

    Bundle->ConditionalDecoder = LoadChatterboxSession(BaseDir, Variant, TEXT("conditional_decoder"), OutError);
    if (!Bundle->ConditionalDecoder.IsValid())
    {
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: all three %s sessions loaded successfully."),
           *Variant);

    return Bundle;
}

void FInoChatterboxModels::LogMetadata() const
{
    if (!LanguageModel.IsValid() || !EmbedTokens.IsValid() || !ConditionalDecoder.IsValid())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("FInoChatterboxModels::LogMetadata: bundle is incomplete."));
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("========================================"));
    UE_LOG(LogInoAgents, Log, TEXT("Chatterbox (%s) model metadata"), *Variant);
    UE_LOG(LogInoAgents, Log, TEXT("========================================"));

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- language_model ---"));
    LanguageModel->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- embed_tokens ---"));
    EmbedTokens->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT(""));
    UE_LOG(LogInoAgents, Log, TEXT("--- conditional_decoder ---"));
    ConditionalDecoder->LogMetadata();

    UE_LOG(LogInoAgents, Log, TEXT("========================================"));
}
