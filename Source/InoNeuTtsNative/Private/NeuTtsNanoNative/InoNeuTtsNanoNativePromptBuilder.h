// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

namespace InoNeuTtsNanoNative
{
    /**
     * Compose the exact NeuTTS Nano chat-template prompt string.
     *
     * Mirrors the Python reference (neuphonic/neutts, neutts/neutts.py ::
     * _apply_chat_template + _infer_ggml) — both ref_phones and input
     * phonemes are concatenated with a single space between them inside
     * a <|TEXT_PROMPT_START|>...<|TEXT_PROMPT_END|> block, then the
     * assistant turn opens with <|SPEECH_GENERATION_START|> followed by
     * the reference voice's FSQ codes rendered as <|speech_N|> tokens.
     * The LM generates more <|speech_N|> continuation tokens until it
     * emits <|SPEECH_GENERATION_END|>.
     *
     * Resulting string shape:
     *
     *   user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} {input_phonemes}<|TEXT_PROMPT_END|>
     *   assistant:<|SPEECH_GENERATION_START|><|speech_N1|><|speech_N2|>...
     *
     * The worker then tokenizes this whole string via llama_tokenize
     * with parse_special=true so the <|...|> control tokens resolve to
     * the proper single-token ids from Qwen2's extended vocab.
     *
     * NOTE: both inputs MUST be already-phonemized (IPA via espeak-ng).
     * The runtime plugin has no phonemizer in v1; ref_phones comes from
     * the encoded voice JSON and input_phonemes comes from the caller.
     */
    FString BuildPrompt(
        const FString&         RefPhones,
        const TArray<int32>&   RefCodes,
        const FString&         InputPhonemes);
}
