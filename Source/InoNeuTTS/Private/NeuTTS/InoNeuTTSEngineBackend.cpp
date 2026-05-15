// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSEngineBackend.h"

#include "InoNeuTTSCommon.h"        // BackendToLiteRtLmString, ActivationTypeToInt
#include "InoNeuTTSPromptBuilder.h"  // BuildSynthesisPrompt — used by WarmupForVoice

#include "InoAgentsLog.h"

#include "GenericPlatform/GenericPlatformProcess.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"
#include "Misc/ScopeExit.h"

#include "litert/lm/engine.h"

#include <atomic>

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

// =====================================================================
//  Streaming synth
// =====================================================================

namespace
{
    /** Per-stream state shared between the C callback (running on the
     *  LiteRT-LM background thread) and the worker thread waiting for
     *  completion. All access is single-threaded WITHIN the callback
     *  (LM fires callbacks serially), and the worker only READS state
     *  fields AFTER the done-event is signalled — so we don't need a
     *  mutex around them. */
    struct FStreamCtx
    {
        FString UnparsedBuffer;
        TFunction<void(TArrayView<const int32>, bool)> OnTokenChunk;
        FString ErrorMessage;
        FEvent* DoneEvent = nullptr;
        std::atomic<bool> bDone{false};
    };

    /** C-callback bridge — converts the text-stream chunks LiteRT-LM
     *  fires into parsed FSQ ids and forwards them to the typed handler.
     *  Captureless lambda → can be passed as the `LiteRtLmStreamCallback`
     *  function pointer. */
    static void StreamCallback(void* user_data, const char* chunk,
                               bool is_final, const char* error_msg)
    {
        FStreamCtx* C = static_cast<FStreamCtx*>(user_data);

        if (error_msg && error_msg[0])
        {
            C->ErrorMessage = UTF8_TO_TCHAR(error_msg);
        }

        if (chunk && chunk[0])
        {
            C->UnparsedBuffer += UTF8_TO_TCHAR(chunk);

            // Regex over the accumulated buffer; trim the consumed
            // prefix so the buffer stays bounded across long streams.
            TArray<int32> NewIds;
            const FRegexPattern Pat(TEXT("<\\|speech_(\\d+)\\|>"));
            FRegexMatcher M(Pat, C->UnparsedBuffer);
            int32 LastEnd = 0;
            while (M.FindNext())
            {
                NewIds.Add(FCString::Atoi(*M.GetCaptureGroup(1)));
                LastEnd = M.GetMatchEnding();
            }
            if (LastEnd > 0)
            {
                // Drop everything up to (and including) the last full match.
                C->UnparsedBuffer = C->UnparsedBuffer.RightChop(LastEnd);
            }
            if (NewIds.Num() > 0 && C->OnTokenChunk)
            {
                C->OnTokenChunk(MakeArrayView(NewIds), /*bIsFinal=*/false);
            }
        }

        if (is_final && !C->bDone.exchange(true, std::memory_order_acq_rel))
        {
            // Final signal — emit any trailing pseudo-empty chunk so the
            // worker sees bIsFinal=true and can flush its last decode.
            if (C->OnTokenChunk)
            {
                C->OnTokenChunk(TArrayView<const int32>(), /*bIsFinal=*/true);
            }
            if (C->DoneEvent) C->DoneEvent->Trigger();
        }
    }
}

bool FInoNeuTTSEngineBackend::RunStreamingSynthesis(
    const FString& FullPrompt,
    int32 MaxNewTokens,
    TFunction<void(TArrayView<const int32>, bool)> OnTokenChunk,
    FString& OutError,
    TFunction<bool()> CancelCheck)
{
    if (!Engine)
    {
        OutError = TEXT("Engine not initialized");
        return false;
    }
    if (CancelCheck && CancelCheck()) { OutError = TEXT("Cancelled"); return false; }

    // Session — same safe setter combo as RunSynthesis (Phase 0b bisect).
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

    // Prefill — blocking.
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
        TEXT("[NeuTTS][Engine] streaming prefill %d chars OK in %.1f ms"),
        FullPrompt.Len(), (FPlatformTime::Seconds() - TPrefill) * 1000.0);

    if (CancelCheck && CancelCheck())
    {
        litert_lm_session_cancel_process(Session);
        CleanupSession();
        OutError = TEXT("Cancelled");
        return false;
    }

    // Set up streaming context + done event.
    FStreamCtx Ctx;
    Ctx.OnTokenChunk = MoveTemp(OnTokenChunk);
    Ctx.DoneEvent    = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/true);
    ON_SCOPE_EXIT
    {
        if (Ctx.DoneEvent)
        {
            FPlatformProcess::ReturnSynchEventToPool(Ctx.DoneEvent);
            Ctx.DoneEvent = nullptr;
        }
    };

    const double TDecode = FPlatformTime::Seconds();
    const int AsyncStatus = litert_lm_session_run_decode_async(
        Session, &StreamCallback, &Ctx);
    if (AsyncStatus != 0)
    {
        OutError = FString::Printf(TEXT("run_decode_async failed to start: status=%d"), AsyncStatus);
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Engine] %s"), *OutError);
        CleanupSession();
        return false;
    }

    // Wait for done with periodic cancel check.
    bool bCancelled = false;
    while (!Ctx.bDone.load(std::memory_order_acquire))
    {
        // 100ms wait — short enough for snappy cancel, long enough to
        // avoid busy-spinning.
        if (Ctx.DoneEvent->Wait(100))
        {
            break;  // event triggered
        }
        if (!bCancelled && CancelCheck && CancelCheck())
        {
            bCancelled = true;
            UE_LOG(LogInoAgents, Log,
                TEXT("[NeuTTS][Engine] streaming cancelled — signalling session.cancel_process."));
            litert_lm_session_cancel_process(Session);
            // After cancel_process the library should fire a final
            // callback shortly; fall through to the wait loop with a
            // safety timeout below.
            break;
        }
    }

    // If cancelled, wait up to 5 s for the final callback to drain
    // before destroying the session out from under it.
    if (bCancelled)
    {
        const double TCancelWait = FPlatformTime::Seconds();
        while (!Ctx.bDone.load(std::memory_order_acquire))
        {
            if (Ctx.DoneEvent->Wait(100)) break;
            if (FPlatformTime::Seconds() - TCancelWait > 5.0)
            {
                UE_LOG(LogInoAgents, Warning,
                    TEXT("[NeuTTS][Engine] cancel timeout — proceeding with session teardown ")
                    TEXT("without waiting for final callback."));
                break;
            }
        }
    }

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Engine] streaming finished in %.1f ms (cancelled=%d)"),
        (FPlatformTime::Seconds() - TDecode) * 1000.0, bCancelled ? 1 : 0);

    CleanupSession();

    if (bCancelled)
    {
        OutError = TEXT("Cancelled");
        return false;
    }
    if (!Ctx.ErrorMessage.IsEmpty())
    {
        OutError = Ctx.ErrorMessage;
        return false;
    }
    return true;
}

bool FInoNeuTTSEngineBackend::Warmup(FString& OutError)
{
    // Minimal prompt + MaxNewTokens=1 to exercise the prefill + first
    // decode kernel without burning seconds on full generation. The
    // <|SPEECH_GENERATION_START|> is unaccompanied by any speech tokens
    // here — that's an unusual context vs real synth but it's still a
    // valid path for warming the kernels.
    static const FString DummyPrompt =
        TEXT("user: Convert the text to speech:<|TEXT_PROMPT_START|> ")
        TEXT("<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>");
    TArray<int32> Throwaway;
    return RunSynthesis(DummyPrompt, /*MaxNewTokens=*/1, Throwaway, OutError, nullptr);
}

bool FInoNeuTTSEngineBackend::WarmupForVoice(
    const FString& RefPhones,
    const FString& SpeechBlock,
    FString& OutError)
{
    // Real-shape warmup. Build the same prompt structure a real synth
    // uses (prefix + RefPhones + " " + empty-input + suffix +
    // SpeechBlock) and run a single decode step through it. The empty
    // input segment keeps prefill cheap while still exercising the
    // full kernel path actual synth calls take.
    const FString WarmupPrompt = InoNeuTTSNative::BuildSynthesisPrompt(
        RefPhones, /*InputPhones=*/FString(), SpeechBlock);
    TArray<int32> Throwaway;
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Engine] WarmupForVoice — prompt %d chars (RefPhones=%d, SpeechBlock=%d)"),
        WarmupPrompt.Len(), RefPhones.Len(), SpeechBlock.Len());
    return RunSynthesis(WarmupPrompt, /*MaxNewTokens=*/1, Throwaway, OutError, nullptr);
}
