// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

namespace InoNeuTTSNative
{
    /** Build the `<|speech_N1|><|speech_N2|>...<|speech_NK|>` string from
     *  an array of NeuCodec FSQ codes. Each Code is in [0, 65535]
     *  (matches the `<|speech_*|>` vocab range). Cache this once per
     *  voice — for a typical ~650-code reference, it's ~10 KB of string. */
    FString BuildSpeechTokensBlock(const TArray<int32>& RefCodes);

    /**
     * Build the full hand-crafted NeuTTS Nano synthesis prompt:
     *
     *     user: Convert the text to speech:<|TEXT_PROMPT_START|>
     *     {ref_phones} {input_phones}
     *     <|TEXT_PROMPT_END|>
     *     \nassistant:<|SPEECH_GENERATION_START|>
     *     {speech_tokens_block}
     *
     * (Newlines added for readability; the actual string contains only
     * the one literal "\n" before "assistant:".)
     *
     * The prefix + suffix strings MUST match
     * `_USER_PREFIX` / `_USER_SUFFIX` baked into the .litertlm bundle
     * by `Plugins/InoLiteRT/Convert/NeuTTS/scripts/build_litertlm.py`
     * byte-for-byte. If they drift, the model sees a different chat
     * shape than it was trained on.
     *
     * The runner sends this through the LiteRT-LM engine with
     * `apply_prompt_template=false` to bypass the bundle's baked
     * templating — we ARE the template here.
     */
    FString BuildSynthesisPrompt(
        const FString& RefPhones,
        const FString& InputPhones,
        const FString& SpeechTokensBlock);
}
