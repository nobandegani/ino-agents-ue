// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSEngineBackend.h"

#include "InoNeuTTSCommon.h"  // BackendToLiteRtLmString, ActivationTypeToInt

#include "InoAgentsLog.h"

#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"

#include "litert/lm/engine.h"

TUniquePtr<FInoNeuTTSEngineBackend> FInoNeuTTSEngineBackend::Create(
    const FString& ModelPath,
    EInoNeuTTSBackend Backend,
    EInoNeuTTSActivationType ActivationType,
    int32 MaxNumTokens,
    const FString& CacheDir,
    int32 PrefillChunkSize,
    FString& OutError)
{
    TUniquePtr<FInoNeuTTSEngineBackend> Inst(new FInoNeuTTSEngineBackend());
    if (!Inst->Initialize(ModelPath, Backend, ActivationType, MaxNumTokens,
                          CacheDir, PrefillChunkSize, OutError))
    {
        return nullptr;
    }
    return Inst;
}

FInoNeuTTSEngineBackend::~FInoNeuTTSEngineBackend()
{
    if (Engine) litert_lm_engine_delete(Engine);
}

bool FInoNeuTTSEngineBackend::Initialize(
    const FString& ModelPath,
    EInoNeuTTSBackend Backend,
    EInoNeuTTSActivationType ActivationType,
    int32 MaxNumTokens,
    const FString& CacheDir,
    int32 PrefillChunkSize,
    FString& OutError)
{
    const FTCHARToUTF8 PathUtf8(*ModelPath);
    const char* BackendStr = InoNeuTTSNative::BackendToLiteRtLmString(Backend);

    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        PathUtf8.Get(), BackendStr,
        /*vision_backend_str=*/nullptr,
        /*audio_backend_str=*/nullptr);
    if (!Settings)
    {
        OutError = TEXT("litert_lm_engine_settings_create returned NULL");
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Engine] %s"), *OutError);
        return false;
    }

    // Activation precision (always set — F32 == 0 == LiteRT-LM default,
    // but explicit is clearer than implicit).
    litert_lm_engine_settings_set_activation_data_type(
        Settings, InoNeuTTSNative::ActivationTypeToInt(ActivationType));

    // Override max_num_tokens only when the caller set a non-zero value.
    // Zero means "use the value baked into the .litertlm bundle"
    // (2048 for the converted NeuTTS Nano — see build_litertlm.py).
    if (MaxNumTokens > 0)
    {
        litert_lm_engine_settings_set_max_num_tokens(Settings, MaxNumTokens);
    }

    if (!CacheDir.IsEmpty())
    {
        const FTCHARToUTF8 CacheDirUtf8(*CacheDir);
        litert_lm_engine_settings_set_cache_dir(Settings, CacheDirUtf8.Get());
    }

    if (PrefillChunkSize > 0)
    {
        litert_lm_engine_settings_set_prefill_chunk_size(Settings, PrefillChunkSize);
    }

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Engine] creating engine (backend=%s, activation=%d, ")
        TEXT("max_tokens=%d, prefill_chunk=%d, cache_dir='%s')"),
        ANSI_TO_TCHAR(BackendStr),
        InoNeuTTSNative::ActivationTypeToInt(ActivationType),
        MaxNumTokens, PrefillChunkSize, *CacheDir);

    const double T0 = FPlatformTime::Seconds();
    Engine = litert_lm_engine_create(Settings);
    // The engine consumes the settings synchronously; we still call delete
    // to free the settings wrapper.
    litert_lm_engine_settings_delete(Settings);

    if (!Engine)
    {
        OutError = TEXT("litert_lm_engine_create returned NULL — bundle load failed");
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Engine] %s"), *OutError);
        return false;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Engine] backbone loaded in %.1f ms"),
        (FPlatformTime::Seconds() - T0) * 1000.0);
    return true;
}

bool FInoNeuTTSEngineBackend::RunSynthesis(
    const FString& FullPrompt,
    int32 MaxNewTokens,
    TArray<int32>& OutSpeechIds,
    FString& OutError,
    TFunction<bool()> CancelCheck)
{
    OutSpeechIds.Reset();
    if (!Engine)
    {
        OutError = TEXT("Engine not initialized");
        return false;
    }
    if (CancelCheck && CancelCheck()) { OutError = TEXT("Cancelled"); return false; }

    // Build a SessionConfig with the SAFE setters proven by the Phase 0b
    // bisect probe: `set_apply_prompt_template(false)` (bypass baked
    // chat template — we ARE the template) and `set_max_output_tokens`
    // (cap generation length / warmup early-out). Do NOT call
    // `set_sampler_params` — that triggers the TOP_K CPU-sampler
    // regression. Bundle's baked TOP_P sampler is used.
    LiteRtLmSessionConfig* SessionCfg = litert_lm_session_config_create();
    if (!SessionCfg)
    {
        OutError = TEXT("litert_lm_session_config_create returned NULL");
        return false;
    }
    litert_lm_session_config_set_apply_prompt_template(SessionCfg, false);
    if (MaxNewTokens > 0)
    {
        litert_lm_session_config_set_max_output_tokens(SessionCfg, MaxNewTokens);
    }

    LiteRtLmSession* Session = litert_lm_engine_create_session(Engine, SessionCfg);
    if (!Session)
    {
        OutError = TEXT("litert_lm_engine_create_session returned NULL");
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Engine] %s"), *OutError);
        litert_lm_session_config_delete(SessionCfg);
        return false;
    }

    auto CleanupSession = [&]()
    {
        if (Session)    { litert_lm_session_delete(Session); Session = nullptr; }
        if (SessionCfg) { litert_lm_session_config_delete(SessionCfg); SessionCfg = nullptr; }
    };

    // Prefill the hand-built prompt as a single text input.
    const FTCHARToUTF8 PromptUtf8(*FullPrompt);
    LiteRtLmInputData Input{};
    Input.type = kLiteRtLmInputDataTypeText;
    Input.data = PromptUtf8.Get();
    Input.size = PromptUtf8.Length();

    const double TPrefill = FPlatformTime::Seconds();
    const int PrefillStatus = litert_lm_session_run_prefill(Session, &Input, 1);
    if (PrefillStatus != 0)
    {
        OutError = FString::Printf(TEXT("run_prefill failed: status=%d"), PrefillStatus);
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Engine] %s"), *OutError);
        CleanupSession();
        return false;
    }
    UE_LOG(LogInoAgents, Verbose,
        TEXT("[NeuTTS][Engine] prefill %d chars OK in %.1f ms"),
        FullPrompt.Len(), (FPlatformTime::Seconds() - TPrefill) * 1000.0);

    if (CancelCheck && CancelCheck())
    {
        litert_lm_session_cancel_process(Session);
        CleanupSession();
        OutError = TEXT("Cancelled");
        return false;
    }

    const double TDecode = FPlatformTime::Seconds();
    LiteRtLmResponses* Responses = litert_lm_session_run_decode(Session);
    if (!Responses)
    {
        OutError = TEXT("run_decode returned NULL");
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Engine] %s"), *OutError);
        CleanupSession();
        return false;
    }

    const int NumCandidates = litert_lm_responses_get_num_candidates(Responses);
    if (NumCandidates > 0)
    {
        const char* RespC = litert_lm_responses_get_response_text_at(Responses, 0);
        if (RespC)
        {
            const FString Resp = UTF8_TO_TCHAR(RespC);
            const FRegexPattern Pat(TEXT("<\\|speech_(\\d+)\\|>"));
            FRegexMatcher M(Pat, Resp);
            while (M.FindNext())
            {
                OutSpeechIds.Add(FCString::Atoi(*M.GetCaptureGroup(1)));
            }
        }
    }
    litert_lm_responses_delete(Responses);
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Engine] decode produced %d speech ids in %.1f ms"),
        OutSpeechIds.Num(), (FPlatformTime::Seconds() - TDecode) * 1000.0);

    CleanupSession();
    return true;
}

bool FInoNeuTTSEngineBackend::Warmup(FString& OutError)
{
    // Minimal prompt + MaxNewTokens=1 to exercise the prefill + first
    // decode kernel without burning seconds on full generation.
    static const FString DummyPrompt =
        TEXT("user: Convert the text to speech:<|TEXT_PROMPT_START|> ")
        TEXT("<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>");
    TArray<int32> Throwaway;
    return RunSynthesis(DummyPrompt, /*MaxNewTokens=*/1, Throwaway, OutError, nullptr);
}
