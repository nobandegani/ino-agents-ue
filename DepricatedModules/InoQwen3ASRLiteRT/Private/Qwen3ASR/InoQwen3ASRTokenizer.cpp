// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRTokenizer.h"

#include "InoQwen3ASRConstants.h"
#include "InoQwen3ASRLiteRT.h"

#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"

namespace
{
    /**
     * GPT-2 byte-to-unicode permutation, INVERSE direction (codepoint → byte).
     * Reference: https://github.com/openai/gpt-2/blob/master/src/encoder.py#L9
     *
     * Iteration matches the Python reference:
     *   For b in 0..255:
     *     printable bytes (33..126, 161..172, 174..255) → codepoint = b
     *     non-printable bytes (0..32, 127..160, 173)    → codepoint = 256 + idx,
     *       where idx counts non-printables in order seen.
     *
     * Invert that for decoding: given a unicode codepoint we recover the
     * original byte. All codepoints land in the BMP (≤ 0x017F), so UTF-16
     * surrogate handling is not needed.
     */
    static void BuildUnicodeToBytesMap(TMap<uint32, uint8>& OutMap)
    {
        OutMap.Reset();
        OutMap.Reserve(256);
        uint32 NonPrintableIdx = 0;
        for (int32 b = 0; b < 256; ++b)
        {
            const bool bPrintable =
                (b >= 33  && b <= 126) ||
                (b >= 161 && b <= 172) ||
                (b >= 174 && b <= 255);
            const uint32 Cp = bPrintable
                ? static_cast<uint32>(b)
                : (256u + NonPrintableIdx++);
            OutMap.Add(Cp, static_cast<uint8>(b));
        }
        // Sanity: 256 entries, with 68 non-printables mapped to 256..323.
        check(OutMap.Num() == 256);
    }
}

bool FInoQwen3ASRTokenizer::LoadFromDisk(const FString& VocabJsonPath)
{
    bLoaded = false;
    IdToBytes.Reset();
    IsSpecialId.Reset();

    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *VocabJsonPath))
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Tokenizer: failed to read vocab file '%s'."), *VocabJsonPath);
        return false;
    }

    // Build the inverse byte-to-unicode permutation up front.
    TMap<uint32, uint8> UnicodeToBytes;
    BuildUnicodeToBytesMap(UnicodeToBytes);

    // Pre-size to the model's known vocab. We grow if the file declares larger IDs.
    IdToBytes.SetNum(InoQwen3ASR::kVocabSize);
    IsSpecialId.Init(false, InoQwen3ASR::kVocabSize);
    // IDs >= 151643 are Qwen's chat-template / audio control tokens; never emit them.
    for (int32 Id = InoQwen3ASR::kPadTokenId; Id < InoQwen3ASR::kVocabSize; ++Id)
    {
        IsSpecialId[Id] = true;
    }

    // ---------- Stream the JSON in event mode ----------
    // We deliberately avoid FJsonObject / FJsonSerializer::Deserialize: those
    // build a TMap<FString, TSharedPtr<FJsonValue>> internally, and UE's
    // FString TMap hashing routes through FCrc::Strihash_DEPRECATED — which
    // is CASE-INSENSITIVE. Vocab.json keys like "A" and "a" land in the
    // same bucket, and one silently overwrites the other on insert. For
    // Qwen3-ASR's 151,643-entry vocab, this drops ~24,000 distinct tokens.
    //
    // TJsonReader's notification API hands us each (key, value) pair as we
    // encounter it, before any TMap is built. We just consume them directly.
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);

    int32 NumDecoded = 0;
    int32 NumSkippedUnknown = 0;
    int32 MaxIdSeen = -1;
    bool bSawObjectStart = false;

    EJsonNotation Notation = EJsonNotation::Null;
    while (Reader->ReadNext(Notation))
    {
        if (!bSawObjectStart)
        {
            if (Notation != EJsonNotation::ObjectStart)
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Error,
                    TEXT("Tokenizer: vocab.json must be a top-level JSON object."));
                return false;
            }
            bSawObjectStart = true;
            continue;
        }

        if (Notation == EJsonNotation::ObjectEnd)
        {
            break;
        }
        // Inside the object: each entry surfaces as exactly one
        // EJsonNotation::Number event whose Identifier is the key and
        // whose value is GetValueAsNumber(). String values would surface as
        // EJsonNotation::String — vocab.json doesn't have those, but skip
        // them defensively rather than erroring.
        if (Notation != EJsonNotation::Number)
        {
            continue;
        }

        const FString UnicodeKey = Reader->GetIdentifier();
        const int32 Id = static_cast<int32>(Reader->GetValueAsNumber());
        if (Id < 0)
        {
            continue;
        }
        if (Id > MaxIdSeen)
        {
            MaxIdSeen = Id;
            if (Id >= IdToBytes.Num())
            {
                // Grow both tables together. Newly added IsSpecialId slots
                // default to false (real text token), which is correct.
                IdToBytes.SetNum(Id + 1);
                IsSpecialId.SetNum(Id + 1, EAllowShrinking::No);
            }
        }

        TArray<uint8>& Bytes = IdToBytes[Id];
        Bytes.Reset(UnicodeKey.Len());

        bool bAllMapped = true;
        for (int32 i = 0; i < UnicodeKey.Len(); ++i)
        {
            // GPT-2 byte-mapped codepoints all fit in the BMP (≤ U+0143)
            // — no UTF-16 surrogate handling required.
            const uint32 Cp = static_cast<uint32>(UnicodeKey[i]);
            const uint8* MappedByte = UnicodeToBytes.Find(Cp);
            if (!MappedByte)
            {
                bAllMapped = false;
                break;
            }
            Bytes.Add(*MappedByte);
        }

        if (!bAllMapped)
        {
            Bytes.Reset();
            if (Id < IsSpecialId.Num())
            {
                IsSpecialId[Id] = true;
            }
            ++NumSkippedUnknown;
            continue;
        }
        ++NumDecoded;
    }

    if (NumDecoded == 0)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Tokenizer: vocab.json parse produced 0 decoded entries."));
        return false;
    }

    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("Tokenizer: loaded vocab.json — %d text tokens decoded, %d unmappable skipped, max id seen=%d (table size %d)."),
        NumDecoded, NumSkippedUnknown, MaxIdSeen, IdToBytes.Num());

    bLoaded = true;
    return true;
}

FString FInoQwen3ASRTokenizer::Decode(TArrayView<const int32> TokenIds) const
{
    if (!bLoaded || TokenIds.Num() == 0)
    {
        return FString();
    }

    // Concatenate all text-token bytes into a single UTF-8 buffer, then
    // decode at the end. Doing it byte-at-a-time would split multi-byte
    // characters across token boundaries — common with byte-level BPE,
    // where one logical character (e.g. emoji, accented letter) routinely
    // spans 2-4 token bytes.
    TArray<uint8> Utf8Bytes;
    Utf8Bytes.Reserve(TokenIds.Num() * 4);
    for (int32 Id : TokenIds)
    {
        if (Id < 0 || Id >= IdToBytes.Num()) { continue; }
        if (Id < IsSpecialId.Num() && IsSpecialId[Id]) { continue; }
        const TArray<uint8>& B = IdToBytes[Id];
        Utf8Bytes.Append(B);
    }
    if (Utf8Bytes.Num() == 0)
    {
        return FString();
    }

    // Convert the byte buffer (sized, not null-terminated) to TCHAR once.
    const FUTF8ToTCHAR Converter(
        reinterpret_cast<const ANSICHAR*>(Utf8Bytes.GetData()),
        Utf8Bytes.Num());
    return FString(Converter.Length(), Converter.Get());
}
