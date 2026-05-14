// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSPromptBuilder.h"

namespace InoNeuTTSNative
{

FString BuildSpeechTokensBlock(const TArray<int32>& RefCodes)
{
    // Pre-size: "<|speech_NNNNN|>" is up to 16 chars for the largest
    // FSQ code (65535 has 5 digits). Reserve generously.
    FString Out;
    Out.Reserve(RefCodes.Num() * 16);
    for (int32 Code : RefCodes)
    {
        Out += FString::Printf(TEXT("<|speech_%d|>"), Code);
    }
    return Out;
}

FString BuildSynthesisPrompt(
    const FString& RefPhones,
    const FString& InputPhones,
    const FString& SpeechTokensBlock)
{
    // EXACT strings from build_litertlm.py:
    //   _USER_PREFIX = "user: Convert the text to speech:<|TEXT_PROMPT_START|>"
    //   _USER_SUFFIX = "<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>"
    static const FString Prefix =
        TEXT("user: Convert the text to speech:<|TEXT_PROMPT_START|>");
    static const FString Suffix =
        TEXT("<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>");

    FString Out;
    Out.Reserve(Prefix.Len() + RefPhones.Len() + 1 + InputPhones.Len()
                + Suffix.Len() + SpeechTokensBlock.Len());
    Out += Prefix;
    Out += RefPhones;
    Out += TEXT(' ');         // separator between ref + input phones
    Out += InputPhones;
    Out += Suffix;
    Out += SpeechTokensBlock; // seeds the assistant turn with the voice's codes
    return Out;
}

} // namespace InoNeuTTSNative
