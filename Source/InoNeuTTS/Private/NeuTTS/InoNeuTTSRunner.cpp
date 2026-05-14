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
    bool bWarmupBackbone,
    bool bWarmupDecoder,
    FString& OutError)
{
    TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> R(new FInoNeuTTSRunner());

    R->Engine = FInoNeuTTSEngineBackend::Create(BackbonePath, OutError);
    if (!R->Engine.IsValid()) return nullptr;

    R->Decoder = FInoNeuTTSDecoderSession::Create(DecoderPath, OutError);
    if (!R->Decoder.IsValid()) return nullptr;

    if (bWarmupBackbone)
    {
        FString WarmupErr;
        if (!R->Engine->Warmup(WarmupErr))
        {
            UE_LOG(LogInoAgents, Warning,
                TEXT("[NeuTTS][Runner] backbone warmup failed (non-fatal): %s"),
                *WarmupErr);
        }
    }
    if (bWarmupDecoder)
    {
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
