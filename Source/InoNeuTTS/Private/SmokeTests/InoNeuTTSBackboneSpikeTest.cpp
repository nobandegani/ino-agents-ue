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
// Initial finding (2026-05-11): passing a non-NULL LiteRtLmSessionConfig
// to `litert_lm_engine_create_session` returns NULL. Same shape as the
// Gemma-4-era regression noted in the InoLiteRtLm docs. We need to know
// WHICH setter triggers it, OR if any config attachment fails — both
// answers determine the path forward.
//
// This spike runs a bisect probe over 7 session-config variants and
// reports which (if any) accepts the attach. For the first one that
// succeeds, it also drives a real prefill+decode with our manual prompt
// to verify the template-bypass actually works at runtime.
//
// Pass criteria (cumulative — best case has all four green):
//   1. At least one SessionConfig variant produces a non-NULL session.
//   2. Among those, `set_apply_prompt_template(false)` is settable.
//   3. After bypass, our hand-built prompt (prefix + phonemes + suffix
//      + <|speech_N|> seed block) is fed in without re-templating.
//   4. The decoded response contains `<|speech_*|>` tokens.
//
// If only (1) holds and (2)/(3) fail, the backbone pivots from
// `.litertlm` to raw `.tflite` via the bare LiteRT C API (the path
// validated by Phase 0a). The user can also re-convert the `.litertlm`
// without baking the chat template (set `_USER_PREFIX = ""` /
// `_USER_SUFFIX = ""` in `Convert/NeuTTS/scripts/build_litertlm.py`),
// which sidesteps the SessionConfig requirement entirely.
//
// Usage:
//   Ino.NeuTTS.BackboneSpikeTest <abs path to neutts_nano_*.litertlm>

#include "InoAgentsLog.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"

#include "litert/lm/engine.h"

namespace
{

// ---------------------------------------------------------------------
// Manual prompt — must match `_USER_PREFIX` + `_USER_SUFFIX` strings
// baked by `build_litertlm.py` byte-for-byte (otherwise the model sees
// a different chat shape than it was trained on and we get garbage
// even if `apply_prompt_template=false` works).
// ---------------------------------------------------------------------
static FString BuildManualPrompt()
{
    const FString Prefix =
        TEXT("user: Convert the text to speech:<|TEXT_PROMPT_START|>");
    const FString Suffix =
        TEXT("<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>");

    // Dummy phonemes — short English greeting, just to give the model
    // a realistic-shaped body. Not testing audio quality here.
    const FString Phonemes = TEXT("h@l'oU D'e@ h@l'oU");

    // 5 arbitrary FSQ codes as a seed block. In a real synth this would
    // be the encoded reference voice's ~650 codes.
    FString SeedTokens;
    static constexpr int32 SeedIds[] = {100, 250, 500, 1000, 2000};
    for (int32 Id : SeedIds)
    {
        SeedTokens += FString::Printf(TEXT("<|speech_%d|>"), Id);
    }
    return Prefix + Phonemes + Suffix + SeedTokens;
}

// ---------------------------------------------------------------------
// Probe types — which setter combinations to test on the SessionConfig
// before passing it into `create_session`.
// ---------------------------------------------------------------------
enum class EProbeKind : uint8
{
    NullConfig,                       // pass nullptr — engine default
    EmptyConfig,                      // create but call no setters
    ApplyTemplateOnly,                // only set_apply_prompt_template(false)
    MaxOutputTokensOnly,              // only set_max_output_tokens(20)
    SamplerOnly,                      // only set_sampler_params(top_k=50, T=1.0)
    TemplateAndMaxTokens,             // apply_template + max_tokens (no sampler)
    AllThree,                         // every setter we want to use in prod
};

static const TCHAR* ProbeKindName(EProbeKind Kind)
{
    switch (Kind)
    {
        case EProbeKind::NullConfig:           return TEXT("NULL config");
        case EProbeKind::EmptyConfig:          return TEXT("empty config (no setters)");
        case EProbeKind::ApplyTemplateOnly:    return TEXT("config + set_apply_prompt_template(false)");
        case EProbeKind::MaxOutputTokensOnly:  return TEXT("config + set_max_output_tokens(20)");
        case EProbeKind::SamplerOnly:          return TEXT("config + set_sampler_params(TopK,k=50,T=1.0)");
        case EProbeKind::TemplateAndMaxTokens: return TEXT("config + apply_template + max_tokens");
        case EProbeKind::AllThree:             return TEXT("config + apply_template + max_tokens + sampler");
        default: return TEXT("<unknown>");
    }
}

// Build a SessionConfig for the requested probe. Returns nullptr for
// the NullConfig case (so the caller passes nullptr through).
static LiteRtLmSessionConfig* BuildConfigForProbe(EProbeKind Kind)
{
    if (Kind == EProbeKind::NullConfig)
    {
        return nullptr;
    }

    LiteRtLmSessionConfig* Cfg = litert_lm_session_config_create();
    if (!Cfg)
    {
        return nullptr;
    }

    LiteRtLmSamplerParams Sampler{};
    Sampler.type = kLiteRtLmSamplerTypeTopK;
    Sampler.top_k = 50;
    Sampler.top_p = 0.0f;
    Sampler.temperature = 1.0f;
    Sampler.seed = 12345;

    switch (Kind)
    {
        case EProbeKind::EmptyConfig:
            break;
        case EProbeKind::ApplyTemplateOnly:
            litert_lm_session_config_set_apply_prompt_template(Cfg, false);
            break;
        case EProbeKind::MaxOutputTokensOnly:
            litert_lm_session_config_set_max_output_tokens(Cfg, 20);
            break;
        case EProbeKind::SamplerOnly:
            litert_lm_session_config_set_sampler_params(Cfg, &Sampler);
            break;
        case EProbeKind::TemplateAndMaxTokens:
            litert_lm_session_config_set_apply_prompt_template(Cfg, false);
            litert_lm_session_config_set_max_output_tokens(Cfg, 20);
            break;
        case EProbeKind::AllThree:
            litert_lm_session_config_set_apply_prompt_template(Cfg, false);
            litert_lm_session_config_set_max_output_tokens(Cfg, 20);
            litert_lm_session_config_set_sampler_params(Cfg, &Sampler);
            break;
        default: break;
    }
    return Cfg;
}

// ---------------------------------------------------------------------
// Drive prefill + decode through a given session with our manual prompt,
// extract `<|speech_*|>` ids from the response. Returns true if at least
// one speech token was parsed.
// ---------------------------------------------------------------------
static bool DriveDecodeAndCheckForSpeechTokens(LiteRtLmSession* Session)
{
    const FString Prompt = BuildManualPrompt();
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike]   driving prompt (%d chars): %s"),
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
            TEXT("[NeuTTS][BackboneSpike]   prefill FAILED: status=%d"), PrefillStatus);
        return false;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike]   prefill OK in %.1f ms"), PrefillMs);

    const double DecodeStart = FPlatformTime::Seconds();
    LiteRtLmResponses* Responses = litert_lm_session_run_decode(Session);
    const double DecodeMs = (FPlatformTime::Seconds() - DecodeStart) * 1000.0;
    if (!Responses)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike]   decode FAILED (responses=NULL)"));
        return false;
    }

    const int NumCandidates = litert_lm_responses_get_num_candidates(Responses);
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike]   decode OK in %.1f ms — %d candidates"),
        DecodeMs, NumCandidates);

    bool bAnySpeech = false;
    if (NumCandidates > 0)
    {
        const char* RespC = litert_lm_responses_get_response_text_at(Responses, 0);
        const FString Resp = RespC ? UTF8_TO_TCHAR(RespC) : FString();
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike]   response (%d chars):"), Resp.Len());
        UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][BackboneSpike]   >>> %s"), *Resp);

        TArray<int32> Ids;
        const FRegexPattern Pat(TEXT("<\\|speech_(\\d+)\\|>"));
        FRegexMatcher M(Pat, Resp);
        while (M.FindNext())
        {
            Ids.Add(FCString::Atoi(*M.GetCaptureGroup(1)));
        }
        bAnySpeech = Ids.Num() > 0;

        FString Preview;
        for (int32 i = 0; i < FMath::Min(10, Ids.Num()); ++i)
        {
            if (i > 0) Preview += TEXT(", ");
            Preview += FString::Printf(TEXT("%d"), Ids[i]);
        }
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike]   parsed %d <|speech_N|> ids — first 10: [%s]"),
            Ids.Num(), *Preview);
    }

    litert_lm_responses_delete(Responses);
    return bAnySpeech;
}

// ---------------------------------------------------------------------
// Run one probe: build the config, try to create a session, and
// optionally drive a decode if create succeeded. The decode is only
// attempted on the first successful probe (controlled by the
// `bAlreadyDecodedSomewhere` flag the caller threads through).
// Returns true iff create_session succeeded for this probe.
// ---------------------------------------------------------------------
static bool RunProbe(LiteRtLmEngine* Engine, EProbeKind Kind, bool& bRanDecode,
                     bool& bSpeechSeen)
{
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] probe '%s' — "), ProbeKindName(Kind));

    LiteRtLmSessionConfig* Cfg = BuildConfigForProbe(Kind);
    if (Kind != EProbeKind::NullConfig && !Cfg)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike]   BuildConfigForProbe returned NULL "
                 "(could not even create the empty config)"));
        return false;
    }

    LiteRtLmSession* Session = litert_lm_engine_create_session(Engine, Cfg);
    if (!Session)
    {
        UE_LOG(LogInoAgents, Warning,
            TEXT("[NeuTTS][BackboneSpike]   create_session FAILED (returned NULL)"));
        if (Cfg) litert_lm_session_config_delete(Cfg);
        return false;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike]   create_session OK"));

    // On the FIRST successful probe, also exercise prefill+decode to
    // verify the chosen config produces speech tokens from our hand-
    // built prompt. We only do this once because each decode costs
    // several seconds and the goal is just to know whether the chosen
    // config bypasses templating, not to benchmark.
    if (!bRanDecode)
    {
        bSpeechSeen = DriveDecodeAndCheckForSpeechTokens(Session);
        bRanDecode = true;
    }

    litert_lm_session_delete(Session);
    if (Cfg) litert_lm_session_config_delete(Cfg);
    return true;
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
    litert_lm_set_min_log_level(2 /* INFO */);

    // -----------------------------------------------------------------
    // 1. Load the engine. (Already proven to work in the v1 spike.)
    // -----------------------------------------------------------------
    const FTCHARToUTF8 PathUtf8(*ModelPath);
    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        PathUtf8.Get(), /*backend_str=*/"cpu",
        /*vision_backend_str=*/nullptr, /*audio_backend_str=*/nullptr);
    if (!Settings)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] engine_settings_create returned NULL"));
        return;
    }
    litert_lm_engine_settings_set_max_num_tokens(Settings, 2048);

    const double EngineStart = FPlatformTime::Seconds();
    LiteRtLmEngine* Engine = litert_lm_engine_create(Settings);
    litert_lm_engine_settings_delete(Settings);
    if (!Engine)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] engine_create returned NULL"));
        return;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] engine loaded in %.1f ms"),
        (FPlatformTime::Seconds() - EngineStart) * 1000.0);

    // -----------------------------------------------------------------
    // 2. Run all 7 probes in order and tally which succeeded.
    // -----------------------------------------------------------------
    static constexpr EProbeKind AllProbes[] = {
        EProbeKind::NullConfig,
        EProbeKind::EmptyConfig,
        EProbeKind::ApplyTemplateOnly,
        EProbeKind::MaxOutputTokensOnly,
        EProbeKind::SamplerOnly,
        EProbeKind::TemplateAndMaxTokens,
        EProbeKind::AllThree,
    };

    bool bRanDecode = false;
    bool bSpeechSeen = false;
    TArray<EProbeKind> Passed;
    for (EProbeKind Kind : AllProbes)
    {
        if (RunProbe(Engine, Kind, bRanDecode, bSpeechSeen))
        {
            Passed.Add(Kind);
        }
    }

    // -----------------------------------------------------------------
    // 3. Verdict.
    // -----------------------------------------------------------------
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] ---- SUMMARY ----"));
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] %d / %d probes created a session"),
        Passed.Num(), static_cast<int32>(UE_ARRAY_COUNT(AllProbes)));
    for (EProbeKind K : Passed)
    {
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike]   ✓ %s"), ProbeKindName(K));
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][BackboneSpike] speech-token test: %s"),
        bRanDecode ? (bSpeechSeen ? TEXT("PASS — <|speech_*|> present in response")
                                   : TEXT("FAIL — no <|speech_*|> in response (re-templated or stuck)"))
                   : TEXT("not run (no probe succeeded)"));

    if (Passed.Num() > 0 && bSpeechSeen)
    {
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][BackboneSpike] PATH OK — use the first passing config for production."));
    }
    else if (Passed.Num() > 0 && !bSpeechSeen)
    {
        UE_LOG(LogInoAgents, Warning,
            TEXT("[NeuTTS][BackboneSpike] PARTIAL — session creates but generation didn't ")
            TEXT("produce <|speech_*|> tokens. The chat template is likely being applied ")
            TEXT("despite our config. Two follow-ups: (a) try re-converting the .litertlm ")
            TEXT("with empty _USER_PREFIX / _USER_SUFFIX in build_litertlm.py, or ")
            TEXT("(b) pivot the backbone to raw .tflite via bare LiteRT C API."));
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][BackboneSpike] HARD FAIL — no session-config variant accepted. ")
            TEXT("Pivot the backbone to raw .tflite via bare LiteRT C API ")
            TEXT("(neutts_nano_q8_ekv2048.tflite + signature runners — mirrors test_tts.py)."));
    }

    litert_lm_engine_delete(Engine);
    UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][BackboneSpike] === END ==="));
}

static FAutoConsoleCommand GBackboneSpikeCmd(
    TEXT("Ino.NeuTTS.BackboneSpikeTest"),
    TEXT("Phase 0b spike — bisect probe over LiteRT-LM SessionConfig variants to find ")
    TEXT("which (if any) accepts attachment and bypasses the baked chat template. ")
    TEXT("Args: <abs path to neutts_nano_*.litertlm>"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunBackboneSpikeTest));

} // namespace
