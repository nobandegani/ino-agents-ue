// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// =====================================================================
// Phase 0b spike — LiteRT-LM custom prompt seeding.
// =====================================================================
//
// Background. NeuTTS's `.litertlm` bundle bakes a chat template that
// wraps every user message as
//
//   "user: Convert the text to speech:<|TEXT_PROMPT_START|>" + <user_text> +
//   "<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>"
//
// To seed the assistant turn with a per-voice `<|speech_N|>` reference
// codes block — which must appear AFTER `<|SPEECH_GENERATION_START|>`
// (= token 128260) and BEFORE the model starts predicting — we need to
// inject custom text that the engine does NOT re-template.
//
// The LiteRT-LM C API exposes
//
//   litert_lm_session_config_set_apply_prompt_template(config, false);
//
// which is supposed to bypass templating entirely. This spike tests
// whether that flag does what its name claims:
//
//   1. Load the .litertlm engine.
//   2. Create a session with `apply_prompt_template = false`.
//   3. Build the FULL prompt by hand (prefix + dummy phonemes + suffix
//      + a small synthetic <|speech_N|> seed block).
//   4. Prefill + decode synchronously with a tiny max_output_tokens.
//   5. Regex-extract <|speech_(\d+)|> from the response text.
//
// Pass criterion: the response contains <|speech_*|> tokens. That
// confirms the model continued the FSQ-token sequence we seeded,
// which means it accepted our custom prompt as-is.
//
// Fail mode 1: response is empty or has no <|speech_*|> ids. The
// model either re-applied the chat template (the flag has no effect)
// or rejected the custom prompt. → Pivot the backbone to raw
// `.tflite` via the bare LiteRT C API (mirrors Phase 0a's mechanics).
//
// Fail mode 2: a function call fails outright. → File an InoLiteRT
// bug; the LM C API has a regression in this build.
//
// Usage:
//   Ino.NeuTTS.BackboneSpikeTest <abs path to neutts_nano_*.litertlm>

#include "InoAgentsLog.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"
#include "Math/UnrealMathUtility.h"

#include "litert/lm/engine.h"

namespace
{

// ---------------------------------------------------------------------
// Build the full handwritten prompt body. The prefix + suffix strings
// here MUST match what `build_litertlm.py` bakes into the .litertlm
// bundle byte-for-byte; if they drift, the model sees a different
// chat shape than it was trained on and we'll get garbage even if the
// no-template flag works.
//
// Source: Plugins/InoLiteRT/Convert/NeuTTS/scripts/build_litertlm.py
//   _USER_PREFIX = "user: Convert the text to speech:<|TEXT_PROMPT_START|>"
//   _USER_SUFFIX = "<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>"
// ---------------------------------------------------------------------
static FString BuildSpikePrompt()
{
    const FString Prefix =
        TEXT("user: Convert the text to speech:<|TEXT_PROMPT_START|>");
    const FString Suffix =
        TEXT("<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>");

    // Dummy phonemes. NeuTTS expects `<ref_phones> <input_phones>`
    // (space-separated). We're not testing audio quality here — any
    // tokenizable IPA-ish text works as long as it's non-empty. Use
    // a short English greeting phonemization so the model sees a
    // realistic-shaped body.
    const FString Phonemes = TEXT("h@l'oU D'e@ h@l'oU");

    // Synthetic <|speech_N|> seed block. In a real synth this would
    // be the encoded reference voice's FSQ codes (~650 tokens).
    // For the spike, 5 arbitrary codes in [0, 65535] is enough to
    // give the AR loop something to continue from.
    FString SeedTokens;
    static constexpr int32 SeedIds[] = {100, 250, 500, 1000, 2000};
    for (int32 Id : SeedIds)
    {
        SeedTokens += FString::Printf(TEXT("<|speech_%d|>"), Id);
    }

    return Prefix + Phonemes + Suffix + SeedTokens;
}

// ---------------------------------------------------------------------
// Main entry.
// ---------------------------------------------------------------------
static void RunBackboneSpikeTest(const TArray<FString>& Args)
{
    if (Args.Num() < 1)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] usage: Ino.NeuTTS.BackboneSpikeTest <abs path to .litertlm>"));
        return;
    }

    const FString ModelPath = Args[0];
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] === BEGIN === model=%s"), *ModelPath);

    // Crank up LM library logging so any internal errors surface.
    litert_lm_set_min_log_level(2 /* INFO */);

    // -----------------------------------------------------------------
    // 1. Build engine settings + create engine.
    // -----------------------------------------------------------------
    const FTCHARToUTF8 PathUtf8(*ModelPath);
    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        PathUtf8.Get(), /*backend_str=*/"cpu",
        /*vision_backend_str=*/nullptr,
        /*audio_backend_str=*/nullptr);
    if (!Settings)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] litert_lm_engine_settings_create returned NULL"));
        return;
    }
    // Cap context at the value the .litertlm was converted for.
    litert_lm_engine_settings_set_max_num_tokens(Settings, 2048);

    const double EngineStart = FPlatformTime::Seconds();
    LiteRtLmEngine* Engine = litert_lm_engine_create(Settings);
    // The engine takes ownership of the settings' contents at create-time;
    // we still delete the settings handle to free the wrapper.
    litert_lm_engine_settings_delete(Settings);
    Settings = nullptr;

    if (!Engine)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] litert_lm_engine_create returned NULL — model load failed."));
        return;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] engine loaded in %.1f ms"),
        (FPlatformTime::Seconds() - EngineStart) * 1000.0);

    // -----------------------------------------------------------------
    // 2. Session config — THE crux of the spike. Disable chat template.
    // -----------------------------------------------------------------
    LiteRtLmSessionConfig* SessionConfig = litert_lm_session_config_create();
    if (!SessionConfig)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] litert_lm_session_config_create returned NULL"));
        litert_lm_engine_delete(Engine);
        return;
    }
    litert_lm_session_config_set_apply_prompt_template(SessionConfig, /*apply=*/false);
    // Cap output to keep the spike fast (~20 tokens of decoding ≈ <1s on CPU).
    litert_lm_session_config_set_max_output_tokens(SessionConfig, 20);

    // Match the sampler baked into the .litertlm (TOP_K, k=50, T=1.0).
    LiteRtLmSamplerParams Sampler{};
    Sampler.type = kLiteRtLmSamplerTypeTopK;
    Sampler.top_k = 50;
    Sampler.top_p = 0.0f;
    Sampler.temperature = 1.0f;
    Sampler.seed = 12345;  // deterministic for the spike
    litert_lm_session_config_set_sampler_params(SessionConfig, &Sampler);

    // -----------------------------------------------------------------
    // 3. Create session.
    // -----------------------------------------------------------------
    LiteRtLmSession* Session = litert_lm_engine_create_session(Engine, SessionConfig);
    if (!Session)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] litert_lm_engine_create_session returned NULL ")
            TEXT("(known Gemma-4-era regression: NeuTTS may need bAttachSessionConfig=false ")
            TEXT("equivalent — flag for follow-up if reproducible)."));
        litert_lm_session_config_delete(SessionConfig);
        litert_lm_engine_delete(Engine);
        return;
    }

    // -----------------------------------------------------------------
    // 4. Build the prompt + prefill.
    // -----------------------------------------------------------------
    const FString Prompt = BuildSpikePrompt();
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] sending prompt (%d chars): %s"),
        Prompt.Len(), *Prompt);

    const FTCHARToUTF8 PromptUtf8(*Prompt);
    LiteRtLmInputData Input{};
    Input.type = kLiteRtLmInputDataTypeText;
    Input.data = PromptUtf8.Get();
    Input.size = PromptUtf8.Length();

    const double PrefillStart = FPlatformTime::Seconds();
    const int PrefillStatus = litert_lm_session_run_prefill(Session, &Input, 1);
    const double PrefillMs = (FPlatformTime::Seconds() - PrefillStart) * 1000.0;
    if (PrefillStatus != 0)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] litert_lm_session_run_prefill failed: status=%d"),
            PrefillStatus);
        litert_lm_session_delete(Session);
        litert_lm_session_config_delete(SessionConfig);
        litert_lm_engine_delete(Engine);
        return;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] prefill OK in %.1f ms"), PrefillMs);

    // -----------------------------------------------------------------
    // 5. Decode (blocking — keep the spike simple; the real path uses
    //    `run_decode_async` for token-stream callbacks).
    // -----------------------------------------------------------------
    const double DecodeStart = FPlatformTime::Seconds();
    LiteRtLmResponses* Responses = litert_lm_session_run_decode(Session);
    const double DecodeMs = (FPlatformTime::Seconds() - DecodeStart) * 1000.0;
    if (!Responses)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] litert_lm_session_run_decode returned NULL"));
        litert_lm_session_delete(Session);
        litert_lm_session_config_delete(SessionConfig);
        litert_lm_engine_delete(Engine);
        return;
    }

    const int NumCandidates = litert_lm_responses_get_num_candidates(Responses);
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] decode OK in %.1f ms — %d candidates"),
        DecodeMs, NumCandidates);

    // -----------------------------------------------------------------
    // 6. Parse the first candidate's text. Extract <|speech_N|> ids.
    // -----------------------------------------------------------------
    bool bAnySpeechToken = false;
    if (NumCandidates > 0)
    {
        const char* ResponseText = litert_lm_responses_get_response_text_at(Responses, 0);
        const FString ResponseStr = ResponseText ? UTF8_TO_TCHAR(ResponseText) : FString();
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike] response (%d chars, full text follows):"),
            ResponseStr.Len());
        UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][BackboneSpike] >>> %s"), *ResponseStr);

        TArray<int32> ParsedSpeechIds;
        const FRegexPattern SpeechPattern(TEXT("<\\|speech_(\\d+)\\|>"));
        FRegexMatcher Matcher(SpeechPattern, ResponseStr);
        while (Matcher.FindNext())
        {
            const FString Captured = Matcher.GetCaptureGroup(1);
            ParsedSpeechIds.Add(FCString::Atoi(*Captured));
        }
        bAnySpeechToken = ParsedSpeechIds.Num() > 0;

        FString IdsPreview;
        for (int32 i = 0; i < FMath::Min(10, ParsedSpeechIds.Num()); ++i)
        {
            if (i > 0) IdsPreview += TEXT(", ");
            IdsPreview += FString::Printf(TEXT("%d"), ParsedSpeechIds[i]);
        }
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike] parsed %d <|speech_N|> ids — first 10: [%s]"),
            ParsedSpeechIds.Num(), *IdsPreview);
    }

    // -----------------------------------------------------------------
    // 7. Verdict.
    // -----------------------------------------------------------------
    if (bAnySpeechToken)
    {
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike] PASS — model continued the seeded <|speech_*|> sequence. ")
            TEXT("LiteRT-LM `apply_prompt_template=false` works for custom seeding. ")
            TEXT("Proceed with the .litertlm path for the backbone."));
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] FAIL — no <|speech_*|> ids in response. ")
            TEXT("Either the no-template flag didn't bypass the chat wrapper, ")
            TEXT("or the model rejected the custom prompt. ")
            TEXT("Investigate before committing to .litertlm; raw .tflite via bare ")
            TEXT("LiteRT C API (mirroring Phase 0a) is the fallback."));
    }

    // -----------------------------------------------------------------
    // 8. Cleanup.
    // -----------------------------------------------------------------
    litert_lm_responses_delete(Responses);
    litert_lm_session_delete(Session);
    litert_lm_session_config_delete(SessionConfig);
    litert_lm_engine_delete(Engine);

    UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][BackboneSpike] === END ==="));
}

static FAutoConsoleCommand GBackboneSpikeCmd(
    TEXT("Ino.NeuTTS.BackboneSpikeTest"),
    TEXT("Phase 0b spike — confirms LiteRT-LM `apply_prompt_template=false` lets us ")
    TEXT("inject a custom prompt with a <|speech_N|> seed block past the chat-template ")
    TEXT("suffix. Args: <abs path to neutts_nano_*.litertlm>"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunBackboneSpikeTest));

} // namespace
