// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/**
 * Qwen3 BPE detokenizer (decode-only).
 *
 * For ASR we only need to convert token IDs → UTF-8 text — the model emits
 * tokens, the user reads strings. We never tokenize text input on this side
 * (text doesn't enter the encoder).
 *
 * Qwen3 uses GPT-2-style byte-level BPE: vocab.json maps token strings (in
 * a special unicode-encoded form where each "character" represents one byte
 * of the original UTF-8) to integer IDs. To recover the original bytes we
 * apply the inverse of GPT-2's bytes-to-unicode permutation, then interpret
 * the resulting byte sequence as UTF-8.
 *
 * Special tokens (e.g. <|im_start|>, <|endoftext|>, <|audio_start|>) live
 * separately in tokenizer_config.json's added_tokens_decoder map. For Phase 2
 * we hardcode the IDs we care about (kEosTokenId*, etc.) and treat everything
 * else as either a real text token or "skip" if it's an added_tokens entry.
 */
class FInoQwen3ASRTokenizer
{
public:
    /**
     * Load vocab.json from disk and build the inverse decode tables.
     * Returns true on success. On failure, IsLoaded() will return false and
     * Decode() will produce empty strings.
     */
    bool LoadFromDisk(const FString& VocabJsonPath);

    bool IsLoaded() const { return bLoaded; }

    /**
     * Convert a sequence of token IDs to UTF-8 text. Special tokens are
     * skipped (left out of the output). Unknown IDs (out of vocab range)
     * are skipped silently.
     */
    FString Decode(TArrayView<const int32> TokenIds) const;

    /** Vocab size loaded (= entries in vocab.json). 0 if not loaded. */
    int32 GetVocabSize() const { return IdToBytes.Num(); }

private:
    bool bLoaded = false;

    /**
     * IdToBytes[id] = the raw UTF-8 byte sequence that this token represents.
     * Built from vocab.json + the inverse-of-GPT2-bytes-to-unicode mapping.
     */
    TArray<TArray<uint8>> IdToBytes;

    /**
     * IsSpecialId[id] = true if this token should be skipped during decode
     * (chat/control tokens like <|im_start|>, <|audio_start|>, etc.). For
     * Phase 2 we mark everything in the >=151643 range as special — that
     * range matches Qwen3's added_tokens block and contains zero text.
     */
    TArray<bool> IsSpecialId;
};
