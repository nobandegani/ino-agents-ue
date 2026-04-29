// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNanoPromptBuilder.h"

#include "InoAgentsLog.h"

namespace InoNeuTtsNano
{

FString BuildPrompt(
    const FString&       RefPhones,
    const TArray<int32>& RefCodes,
    const FString&       InputPhonemes)
{
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: PromptBuilder: BuildPrompt entry "
                "(input_phonemes=%d chars, ref_phones=%d chars, ref_codes=%d)"),
           InputPhonemes.Len(), RefPhones.Len(), RefCodes.Num());

    // First: build the reference-codes section. Each code maps to one
    // <|speech_N|> control token — the Qwen2 tokenizer (as extended by
    // Neuphonic) will turn each one into a single token id when
    // parse_special=true. ~650 codes × ~17 chars per code = ~11 KB
    // string, so reserve up-front.
    FString CodesSection;
    CodesSection.Reserve(RefCodes.Num() * 18);
    for (int32 Code : RefCodes)
    {
        CodesSection += FString::Printf(TEXT("<|speech_%d|>"), Code);
    }

    // Assemble the full prompt. The exact fixed text between the
    // control tokens matches the Python reference:
    //   "user: Convert the text to speech:" before <|TEXT_PROMPT_START|>
    //   "\nassistant:" before <|SPEECH_GENERATION_START|>
    // One space between ref_phones and input_phonemes (NeuTTS expects
    // exactly one).
    FString Prompt = FString::Printf(
        TEXT("user: Convert the text to speech:<|TEXT_PROMPT_START|>%s %s<|TEXT_PROMPT_END|>\n")
        TEXT("assistant:<|SPEECH_GENERATION_START|>%s"),
        *RefPhones,
        *InputPhonemes,
        *CodesSection);

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: PromptBuilder: BuildPrompt done (prompt=%d chars, codes_section=%d chars)"),
           Prompt.Len(), CodesSection.Len());
    UE_LOG(LogInoAgents, Verbose,
           TEXT("NeuTtsNano: PromptBuilder: full prompt (first 400 chars): %s"),
           *Prompt.Left(400));

    return Prompt;
}

} // namespace InoNeuTtsNano
