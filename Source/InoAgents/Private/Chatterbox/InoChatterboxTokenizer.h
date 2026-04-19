// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Crc.h"   // FCrc::StrCrc32 for TCaseSensitiveStringKeyFuncs below

/**
 * FInoChatterboxTokenizer — GPT-2 byte-level BPE tokenizer for Chatterbox
 * Turbo, plus Chatterbox-specific special-token handling.
 *
 * Reads a HuggingFace tokenizer.json and turns text into the int64 token
 * IDs the `embed_tokens` ONNX model expects. No ORT involvement — pure
 * CPU string processing, works on any thread.
 *
 * What this implements:
 *
 *   1. Special-token pre-pass (atomic matching).
 *      Chatterbox's 20 paralinguistic tags (IDs 50256-50275) plus the
 *      EOS marker `<|endoftext|>`. Matched greedily before any
 *      regex/BPE passes so a user-supplied string containing literal
 *      "[laugh]" resolves to the single token ID 50275 rather than
 *      five BPE sub-tokens.
 *
 *   2. GPT-2 pre-tokenizer (ByteLevel + regex split).
 *      The standard `\p{L}+|\p{N}+|...` regex splits text into word-ish
 *      chunks; each chunk is then byte-level encoded — every UTF-8 byte
 *      is mapped to one of 256 unique printable Unicode chars via
 *      GPT-2's canonical byte↔char table. Spaces become `Ġ`, other
 *      whitespace becomes other visible glyphs, so the whole text
 *      becomes a pure "visible" string the BPE can operate on.
 *
 *   3. BPE merging.
 *      For each pre-token, repeatedly find and apply the lowest-rank
 *      merge pair from the 50,000-entry merge table until no more
 *      merges apply. Classic BPE algorithm.
 *
 *   4. Post-processor (template wrapping).
 *      The tokenizer.json declares a TemplateProcessing step that
 *      prepends nothing and appends `<|endoftext|> <|endoftext|>`
 *      (two copies — marking bos/eos boundary for the LM). We
 *      replicate that when `bAddSpecialTokens=true` in Encode().
 *
 * Decode() reverses the process: concat the tokens, byte-level decode
 * to recover the original UTF-8, which we then present as FString.
 *
 * Threading / ownership:
 *   Move-only; held behind TUniquePtr. LoadFromJson does a one-time
 *   dict-build (O(vocab + merges) in memory) that takes ~50 ms for
 *   Chatterbox's ~50 k vocab / 50 k merges. Encode/Decode are const
 *   and thread-safe for concurrent calls — the tokenizer is read-only
 *   after construction.
 *
 * Design principles (relative to other GPT-2 tokenizers):
 *   - Pure UE types at the boundary: FString / TArray<int64>. Internal
 *     byte plumbing goes through FTCHARToUTF8 and local TArray<FString>
 *     buffers. No std::string in public API.
 *   - Exception-free. Errors go through the same OutError + log pattern
 *     as the rest of the plugin (see FInoOnnxSession).
 *   - Zero ORT dependency. This file is the one place in the plugin
 *     where we own the tokenization; if we later add another TTS
 *     runtime that also uses GPT-2 BPE, we can reuse this class
 *     directly.
 */
class FInoChatterboxTokenizer
{
public:
    /**
     * Load a tokenizer from a HuggingFace tokenizer.json file on disk.
     *
     * Expected layout (what Chatterbox's tokenizer.json actually is):
     *   { "model": { "type": "BPE",
     *                "vocab": { "<str>": <int>, ... },
     *                "merges": [ "<a> <b>", ... ] },
     *     "added_tokens": [ { "id": ..., "content": "[laugh]", ... } ],
     *     "pre_tokenizer": { "type": "ByteLevel", ... },
     *     "post_processor": { "type": "TemplateProcessing", ... },
     *     "decoder": { "type": "ByteLevel", ... } }
     *
     * Returns TUniquePtr on success, nullptr + OutError on failure.
     * Specifically errors on:
     *   - file not found / unreadable
     *   - JSON parse failure
     *   - missing required fields
     *   - unsupported tokenizer types (we only implement BPE + ByteLevel)
     */
    static TUniquePtr<FInoChatterboxTokenizer> LoadFromJson(
        const FString& TokenizerJsonPath,
        FString* OutError = nullptr);

    ~FInoChatterboxTokenizer() = default;
    FInoChatterboxTokenizer(const FInoChatterboxTokenizer&) = delete;
    FInoChatterboxTokenizer& operator=(const FInoChatterboxTokenizer&) = delete;

    // ========================================================================
    //  Core API
    // ========================================================================

    /**
     * Encode a text string into token IDs.
     *
     * Pipeline:
     *   1. Split input into segments at special-token boundaries
     *      (so literal "[laugh]" becomes one token, not five).
     *   2. For each non-special segment:
     *      - Apply GPT-2 pre-tokenizer regex to split into pre-tokens.
     *      - Byte-level encode each pre-token.
     *      - Apply BPE merges to get final sub-tokens.
     *      - Look up sub-tokens in the vocab.
     *   3. If bAddSpecialTokens, append the template-processor's
     *      `<|endoftext|> <|endoftext|>` terminator.
     *
     * Returns an empty array on error (logs via LogInoAgents). The
     * typical "failure" here is a string containing characters the
     * tokenizer's vocab can't represent — very unlikely for English
     * text but possible for adversarial inputs.
     */
    TArray<int64> Encode(const FString& Text, bool bAddSpecialTokens = true) const;

    /**
     * Reverse Encode. Concatenates the vocab strings for each ID,
     * then byte-level decodes the result back to UTF-8.
     *
     * Handles:
     *   - Special tokens (renders as their literal content, e.g. "[laugh]")
     *   - Invalid IDs (emits "<unk:N>" placeholder and continues)
     *
     * Primarily useful for round-trip smoke tests. In production we
     * don't decode — the LM emits speech tokens, not text tokens.
     */
    FString Decode(TArrayView<const int64> Ids) const;

    // ========================================================================
    //  Special-token lookups
    // ========================================================================

    /** `<|endoftext|>` — the GPT-2 BOS/EOS marker. ID 50256. */
    int64 GetEndOfTextId() const { return EndOfTextId; }

    /**
     * Resolve a special-token content string to its ID. Examples:
     *   GetSpecialTokenId(TEXT("[laugh]"))        -> 50275
     *   GetSpecialTokenId(TEXT("<|endoftext|>"))  -> 50256
     *   GetSpecialTokenId(TEXT("not-a-special"))  -> -1
     *
     * Case-sensitive; must match exactly. The full list is
     * documented in the "added_tokens" section of tokenizer.json —
     * see the smoke test for a canonical dump.
     */
    int64 GetSpecialTokenId(const FString& Content) const;

    /** Number of entries in the base BPE vocab (not counting added
     *  tokens). For Chatterbox this is 50,257. Useful as an offset
     *  when building the combined text+speech embedding-vocab map. */
    int32 GetBaseVocabSize() const { return VocabIdToString.Num(); }

    /** Total number of tokens the tokenizer knows, including added
     *  special tokens. For Chatterbox this is 50,276 (50,257 + 20). */
    int32 GetTotalVocabSize() const;

    /** List of every special-token content string, in ID order. */
    const TArray<FString>& GetSpecialTokenContents() const { return SpecialTokenContents; }

    // ========================================================================
    //  Introspection for smoke tests + debugging
    // ========================================================================

    /** Dump a summary to LogInoAgents at Log level: vocab size, merge
     *  count, list of special tokens. Useful after LoadFromJson to
     *  confirm we parsed the tokenizer correctly. */
    void LogSummary() const;

private:
    FInoChatterboxTokenizer() = default;

    // --- BPE merge pipeline internals (implementation in .cpp) ---
    TArray<FString> BpeEncode(const FString& PreToken) const;
    TArray<int64>   EncodeNonSpecialSegment(const FString& Segment) const;

    // -------------------------------------------------------------------
    // Case-sensitive FString key-funcs for TMap / TSet.
    //
    // UE's FString is case-INSENSITIVE in TMap by default: both
    // FString::operator== and GetTypeHash(FString) fold case (to stay
    // consistent with FName / SearchCase::IgnoreCase defaults). GPT-2
    // vocab has thousands of cased pairs like {"Hello":15496,
    // "hello":31373} / {" world":995, " WORLD":29564}; with the default
    // key-funcs these collide and the second insert silently overwrites
    // the first, corrupting the vocab lookup table. Same problem hits
    // the BPE merge table (e.g. "H ello" vs "h ello" both as merge
    // keys). We need true case-sensitive map semantics here.
    //
    // BaseKeyFuncs' first template arg is the element type
    // (TPair<K,V> for TMap). Second is the lookup key. Third says
    // whether to allow duplicate keys (false = set/map semantics).
    template<typename ValueType>
    struct TCaseSensitiveStringKeyFuncs
        : BaseKeyFuncs<TPair<FString, ValueType>, FString, /*bInAllowDuplicateKeys=*/false>
    {
        static FORCEINLINE const FString& GetSetKey(const TPair<FString, ValueType>& Element)
        {
            return Element.Key;
        }
        static FORCEINLINE bool Matches(const FString& A, const FString& B)
        {
            return A.Equals(B, ESearchCase::CaseSensitive);
        }
        static FORCEINLINE uint32 GetKeyHash(const FString& Key)
        {
            // FCrc::StrCrc32 walks the string byte-by-byte without
            // case-folding — exactly the case-sensitive hash we want.
            return FCrc::StrCrc32(*Key);
        }
    };

    template<typename ValueType>
    using TCaseSensitiveStringMap =
        TMap<FString, ValueType, FDefaultSetAllocator, TCaseSensitiveStringKeyFuncs<ValueType>>;

    // Vocab maps.
    TCaseSensitiveStringMap<int64> VocabStringToId;   // "Hello" -> 15496, "hello" -> 31373
    TArray<FString>                VocabIdToString;   // reverse: 15496 -> "Hello"

    // BPE merges as (a, b) pairs with their priority rank (lower = higher priority).
    // Key: "<a> <b>" concatenated with a space between (HF format).
    TCaseSensitiveStringMap<int32> MergeRanks;

    // Special tokens (both <|endoftext|> and paralinguistic tags).
    // Sorted by content-length descending for greedy longest-match.
    TArray<FString>                SpecialTokenContents;
    TCaseSensitiveStringMap<int64> SpecialTokenIds;   // "[laugh]" -> 50275

    int64 EndOfTextId = 50256;

    // GPT-2 byte-level encoding tables. Lazily populated by InitByteTables()
    // which LoadFromJson calls once.
    TArray<TCHAR> ByteToChar;    // 256 entries, byte 0 -> ByteToChar[0], etc.
    TMap<TCHAR, uint8> CharToByte;  // reverse
};
