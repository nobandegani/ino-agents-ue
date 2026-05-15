// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSRunner.h"

#include "InoNeuTTSCommon.h"
#include "InoNeuTTSDecoderSession.h"
#include "InoNeuTTSEngineBackend.h"
#include "InoNeuTTSPromptBuilder.h"

#include "InoAgentsLog.h"
#include "InoSpeakNGBPLibrary.h"

TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> FInoNeuTTSRunner::Create(
    const FString& BackbonePath,
    const FString& DecoderPath,
    const FInoNeuTTSConfig& Config,
    FString& OutError)
{
    TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> R(new FInoNeuTTSRunner());

    R->Engine = FInoNeuTTSEngineBackend::Create(
        BackbonePath,
        Config.BackboneBackend,
        Config.ActivationType,
        Config.MaxNumTokens,
        Config.CacheDir,
        Config.PrefillChunkSize,
        OutError);
    if (!R->Engine.IsValid()) return nullptr;

    R->Decoder = FInoNeuTTSDecoderSession::Create(
        DecoderPath, Config.DecoderBackend, OutError);
    if (!R->Decoder.IsValid()) return nullptr;

    // Backbone warmup is DEFERRED until the first successful PrimeVoice.
    // Warming at Create time would require running synth without any
    // reference voice — an unusual context that doesn't match real
    // synth calls. Instead, we stash the flag here and let PrimeVoice
    // fire WarmupForVoice with the just-cached voice's real ref phones
    // + speech-tokens block. Slight one-time latency hit on the first
    // SetActiveVoiceAsync, but the first user-visible synth becomes
    // jitter-free on exactly the kernel path it'll actually take.
    R->bBackboneWarmupPending = Config.bWarmupBackboneOnLoad;

    if (Config.bWarmupDecoderOnLoad)
    {
        // Decoder warmup is voice-agnostic (operates on FSQ codes only),
        // so it stays at Create time. Smallest bucket + all-zero codes.
        FString WarmupErr;
        if (!R->Decoder->Warmup(WarmupErr))
        {
            UE_LOG(LogInoAgents, Warning,
                TEXT("[NeuTTS][Runner] decoder warmup failed (non-fatal): %s"),
                *WarmupErr);
        }
    }

    return R;
}

FInoNeuTTSRunner::~FInoNeuTTSRunner() = default;

bool FInoNeuTTSRunner::PrimeVoice(const FInoNeuTTSVoice& Voice, FString& OutError)
{
    if (!Voice.bIsValid)
    {
        OutError = TEXT("Voice is not valid (missing Name / Language / RefCodes)");
        return false;
    }

    // 1. Resolve ref phonemes — prefer pre-baked RefPhones, else live-phonemize.
    FString ResolvedRefPhones = Voice.RefPhones;
    if (ResolvedRefPhones.IsEmpty())
    {
        if (!UInoSpeakNGBPLibrary::IsAvailable())
        {
            OutError = TEXT("InoSpeakNG not available and voice has no pre-baked RefPhones");
            return false;
        }
        ResolvedRefPhones = UInoSpeakNGBPLibrary::Phonemize(Voice.RefText, Voice.Language);
        if (ResolvedRefPhones.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("InoSpeakNG.Phonemize returned empty for RefText (lang='%s')"),
                *Voice.Language);
            return false;
        }
    }
    CachedRefPhones = InoNeuTTSNative::NormalizePhones(ResolvedRefPhones);

    // 2. Pre-build the "<|speech_N|>...<|speech_NK|>" block. ~16 chars per
    //    code, so a typical ~650-code voice is ~10 KB. Built once per
    //    prime, reused per synth.
    CachedSpeechBlock = InoNeuTTSNative::BuildSpeechTokensBlock(Voice.RefCodes);

    CachedVoiceName = Voice.Name;

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Runner] primed voice '%s' (%d RefCodes, %d ref-phoneme chars, %d block chars)"),
        *CachedVoiceName, Voice.RefCodes.Num(), CachedRefPhones.Len(),
        CachedSpeechBlock.Len());

    // Run deferred backbone warmup on this voice's real ref phones +
    // speech-tokens block, once. Failure is non-fatal — the first user
    // synth will pay the warmup cost itself if this trips a vendor bug.
    // Always clear the flag so we don't retry on subsequent prime calls
    // (a subsequent prime swaps the voice cache; warming each new voice
    // would multiply load latency for no benefit — kernel paths are
    // mostly voice-shape-independent once the first synth has primed
    // them).
    if (bBackboneWarmupPending && Engine.IsValid())
    {
        bBackboneWarmupPending = false;
        FString WarmupErr;
        if (!Engine->WarmupForVoice(CachedRefPhones, CachedSpeechBlock, WarmupErr))
        {
            UE_LOG(LogInoAgents, Warning,
                TEXT("[NeuTTS][Runner] backbone warmup failed (non-fatal): %s"),
                *WarmupErr);
        }
    }

    return true;
}

void FInoNeuTTSRunner::ClearVoiceCache()
{
    CachedVoiceName.Reset();
    CachedRefPhones.Reset();
    CachedSpeechBlock.Reset();
}

bool FInoNeuTTSRunner::HasCachedVoice(const FString& VoiceName) const
{
    return !CachedVoiceName.IsEmpty()
        && CachedVoiceName.Equals(VoiceName, ESearchCase::CaseSensitive);
}
