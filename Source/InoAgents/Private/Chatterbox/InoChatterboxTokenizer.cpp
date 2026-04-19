// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxTokenizer.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Design note on character width: TCHAR is UTF-16 code-units on Windows
// (and UTF-32 on some non-default UE builds). The GPT-2 byte-level
// encoding table spans codepoints 0x21..0xFF and 0x0100..0x0142 — all
// within the Basic Multilingual Plane (U+0000..U+FFFF), so a single
// TCHAR holds one mapped char everywhere UE targets. We never cross
// into surrogate pairs in the byte-level string domain.

namespace
{
    // ========================================================================
    //  GPT-2 byte-level <-> unicode table construction
    // ========================================================================
    //
    // Canonical GPT-2 mapping. Directly mirrors
    // transformers/models/gpt2/tokenization_gpt2.py bytes_to_unicode():
    //
    //   bs = list(range(ord("!"), ord("~")+1)) +
    //        list(range(ord("¡"), ord("¬")+1)) +
    //        list(range(ord("®"), ord("ÿ")+1))
    //   cs = bs[:]
    //   n = 0
    //   for b in range(256):
    //       if b not in bs:
    //           bs.append(b)
    //           cs.append(256 + n)
    //           n += 1
    //   return dict(zip(bs, (chr(c) for c in cs)))
    //
    // The resulting mapping has every byte 0..255 mapped to exactly one
    // printable-ish Unicode codepoint, with no collisions. Bytes that
    // would otherwise be invisible (control chars, spaces, DEL) get
    // remapped to codepoints in the U+0100..U+0142 range.
    void BuildByteLevelTables(TArray<TCHAR>& OutByteToChar, TMap<TCHAR, uint8>& OutCharToByte)
    {
        OutByteToChar.SetNumZeroed(256);

        // First pass: bytes that map to themselves (33..126, 161..172, 174..255).
        auto IsSelfMapping = [](int32 B) -> bool
        {
            return (B >= 0x21 && B <= 0x7E)
                || (B >= 0xA1 && B <= 0xAC)
                || (B >= 0xAE && B <= 0xFF);
        };

        int32 Overflow = 0;
        for (int32 B = 0; B < 256; ++B)
        {
            if (IsSelfMapping(B))
            {
                OutByteToChar[B] = (TCHAR)B;
            }
            else
            {
                // Assign overflow bytes to codepoints 0x100, 0x101, ...
                OutByteToChar[B] = (TCHAR)(0x100 + Overflow);
                ++Overflow;
            }
        }

        OutCharToByte.Reset();
        for (int32 B = 0; B < 256; ++B)
        {
            OutCharToByte.Add(OutByteToChar[B], (uint8)B);
        }
    }

    // ========================================================================
    //  UTF-8 byte-level string conversion
    // ========================================================================

    /**
     * Convert a UE FString (UTF-16 internally) to the byte-level FString
     * the BPE operates on. Each UTF-8 byte becomes one mapped char.
     */
    FString FStringToByteLevel(const FString& In, const TArray<TCHAR>& ByteToChar)
    {
        const FTCHARToUTF8 Utf8(*In);
        const char* Src = Utf8.Get();
        const int32 Len = Utf8.Length();

        FString Out;
        Out.Reserve(Len);
        for (int32 i = 0; i < Len; ++i)
        {
            const uint8 B = (uint8)Src[i];
            Out.AppendChar(ByteToChar[B]);
        }
        return Out;
    }

    /**
     * Inverse of FStringToByteLevel: map each char back to its byte,
     * reassemble as UTF-8, then interpret as FString.
     */
    FString ByteLevelToFString(const FString& In, const TMap<TCHAR, uint8>& CharToByte)
    {
        TArray<uint8> Bytes;
        Bytes.Reserve(In.Len() + 1);
        for (TCHAR Ch : In)
        {
            const uint8* B = CharToByte.Find(Ch);
            if (B == nullptr)
            {
                // Not a byte-level char. Best effort: replace with '?'.
                Bytes.Add((uint8)'?');
            }
            else
            {
                Bytes.Add(*B);
            }
        }
        Bytes.Add(0); // null terminator for UTF8_TO_TCHAR

        return FString(UTF8_TO_TCHAR((const ANSICHAR*)Bytes.GetData()));
    }

    // ========================================================================
    //  GPT-2 pre-tokenizer (ASCII-first English-pragmatic splitter)
    // ========================================================================
    //
    // The canonical GPT-2 pre-tokenizer regex is:
    //   's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
    //
    // UE's FRegexPattern does not reliably support \p{L} / \p{N} Unicode
    // properties across platforms (it differs between Win MSVC and clang
    // Android). Since Chatterbox Turbo is English-only by design, we
    // implement an ASCII-first equivalent that handles the English case
    // correctly and falls back to byte-level tokenization for any
    // characters outside the ASCII+Latin-1 range. This matches the
    // tokenizer.json pre_tokenizer.type == "ByteLevel" semantics.
    //
    // Categories we split on:
    //   - Contractions     : 's 't 're 've 'm 'll 'd (optional leading space)
    //   - Letter runs      : A-Za-z (+ accented Latin-1 chars as bonus)
    //   - Digit runs       : 0-9
    //   - Punctuation runs : everything non-alnum non-space
    //   - Whitespace runs  : spaces, tabs, newlines
    //
    // The boundary logic matches the regex semantics: each category
    // greedily consumes as many consecutive chars as possible.

    enum class EPreTokenCategory { Letter, Digit, Punct, Whitespace };

    bool IsLetter(TCHAR C)
    {
        // ASCII A-Z, a-z plus Latin-1 supplement letters (0xC0..0xFF excluding
        // math/punctuation codepoints). Pragmatic for the English corpus
        // Chatterbox Turbo targets.
        if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z')) return true;
        if (C >= 0xC0 && C <= 0xFF && C != 0xD7 && C != 0xF7) return true;
        return false;
    }

    bool IsDigit(TCHAR C)
    {
        return C >= '0' && C <= '9';
    }

    bool IsAsciiWhitespace(TCHAR C)
    {
        return C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\v' || C == '\f';
    }

    /** Match a GPT-2 contraction at Position. Returns the matched length
     *  (0 if no match). Contractions are: 's, 't, 're, 've, 'm, 'll, 'd. */
    int32 MatchContraction(const FString& Text, int32 Position)
    {
        if (Position >= Text.Len() || Text[Position] != '\'')
        {
            return 0;
        }
        // Check from longest to shortest to avoid e.g. 'l matching before 'll.
        static const TCHAR* Singles[]     = { TEXT("'s"), TEXT("'t"), TEXT("'m"), TEXT("'d") };
        static const TCHAR* Doubles[]     = { TEXT("'re"), TEXT("'ve"), TEXT("'ll") };

        for (const TCHAR* Cand : Doubles)
        {
            const int32 CandLen = FCString::Strlen(Cand);
            if (Position + CandLen <= Text.Len() &&
                FCString::Strncmp(*Text + Position, Cand, CandLen) == 0)
            {
                return CandLen;
            }
        }
        for (const TCHAR* Cand : Singles)
        {
            const int32 CandLen = FCString::Strlen(Cand);
            if (Position + CandLen <= Text.Len() &&
                FCString::Strncmp(*Text + Position, Cand, CandLen) == 0)
            {
                return CandLen;
            }
        }
        return 0;
    }

    /** Split a text segment into GPT-2 pre-tokens. Preserves leading
     *  spaces as part of the next pre-token (GPT-2 convention: "hello"
     *  -> ["hello"], " world" -> [" world"]). */
    TArray<FString> PreTokenize(const FString& Segment)
    {
        TArray<FString> Out;
        const int32 Len = Segment.Len();
        int32 i = 0;

        while (i < Len)
        {
            // Contraction match (with or without leading space).
            // " 's" or "'s" at the start of a word-ish position.
            {
                const int32 LeadingSpace = (Segment[i] == ' ') ? 1 : 0;
                const int32 ContractionStart = i + LeadingSpace;
                const int32 CLen = MatchContraction(Segment, ContractionStart);
                if (CLen > 0)
                {
                    Out.Add(Segment.Mid(i, LeadingSpace + CLen));
                    i += LeadingSpace + CLen;
                    continue;
                }
            }

            // " word", " 123", " !!!": GPT-2 lets one leading space attach
            // to the next pre-token. Detect the leading space and the
            // category of the following char.
            const bool bHasLeadingSpace = (Segment[i] == ' ');
            int32 Start = i;

            if (bHasLeadingSpace)
            {
                // Check what follows the single leading space.
                if (i + 1 < Len)
                {
                    const TCHAR Next = Segment[i + 1];
                    if (IsLetter(Next))
                    {
                        int32 End = i + 1;
                        while (End < Len && IsLetter(Segment[End])) ++End;
                        Out.Add(Segment.Mid(Start, End - Start));
                        i = End;
                        continue;
                    }
                    if (IsDigit(Next))
                    {
                        int32 End = i + 1;
                        while (End < Len && IsDigit(Segment[End])) ++End;
                        Out.Add(Segment.Mid(Start, End - Start));
                        i = End;
                        continue;
                    }
                    if (!IsAsciiWhitespace(Next))
                    {
                        // Leading space + non-alnum-non-space punctuation run.
                        int32 End = i + 1;
                        while (End < Len
                               && !IsAsciiWhitespace(Segment[End])
                               && !IsLetter(Segment[End])
                               && !IsDigit(Segment[End]))
                        {
                            ++End;
                        }
                        Out.Add(Segment.Mid(Start, End - Start));
                        i = End;
                        continue;
                    }
                }
                // Leading space not followed by a content char -> whitespace run.
            }

            // Pure whitespace run (trailing whitespace, standalone space, newlines).
            if (IsAsciiWhitespace(Segment[i]))
            {
                // \s+(?!\S) vs \s+ — the "not-followed-by-non-whitespace"
                // variant keeps the LAST whitespace attached to the NEXT
                // token. We approximate: collect whitespace until the
                // last char, and if the next char after would be a
                // content char, leave the last space out.
                int32 End = i;
                while (End < Len && IsAsciiWhitespace(Segment[End])) ++End;
                if (End < Len && End - i > 1)
                {
                    // \s+(?!\S) match: keep whitespace[0..End-1), leave the
                    // last space to be picked up as leading-space of next pre-token.
                    Out.Add(Segment.Mid(i, End - 1 - i));
                    i = End - 1;
                }
                else
                {
                    Out.Add(Segment.Mid(i, End - i));
                    i = End;
                }
                continue;
            }

            // Content chars without leading space.
            if (IsLetter(Segment[i]))
            {
                int32 End = i;
                while (End < Len && IsLetter(Segment[End])) ++End;
                Out.Add(Segment.Mid(Start, End - Start));
                i = End;
                continue;
            }
            if (IsDigit(Segment[i]))
            {
                int32 End = i;
                while (End < Len && IsDigit(Segment[End])) ++End;
                Out.Add(Segment.Mid(Start, End - Start));
                i = End;
                continue;
            }

            // Punctuation / other.
            {
                int32 End = i;
                while (End < Len
                       && !IsAsciiWhitespace(Segment[End])
                       && !IsLetter(Segment[End])
                       && !IsDigit(Segment[End]))
                {
                    ++End;
                }
                if (End == i) { ++End; } // defensive: always advance
                Out.Add(Segment.Mid(Start, End - Start));
                i = End;
            }
        }

        return Out;
    }
}

// ============================================================================
//  FInoChatterboxTokenizer — loading
// ============================================================================

TUniquePtr<FInoChatterboxTokenizer> FInoChatterboxTokenizer::LoadFromJson(
    const FString& TokenizerJsonPath,
    FString* OutError)
{
    auto Fail = [OutError](FString Msg)
    {
        if (OutError) *OutError = Msg;
        UE_LOG(LogInoAgents, Error, TEXT("FInoChatterboxTokenizer::LoadFromJson: %s"), *Msg);
        return TUniquePtr<FInoChatterboxTokenizer>(nullptr);
    };

    if (!IFileManager::Get().FileExists(*TokenizerJsonPath))
    {
        return Fail(FString::Printf(TEXT("tokenizer.json not found at %s"), *TokenizerJsonPath));
    }

    // Read + parse.
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *TokenizerJsonPath))
    {
        return Fail(FString::Printf(TEXT("failed to read %s"), *TokenizerJsonPath));
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return Fail(FString::Printf(TEXT("failed to parse JSON in %s"), *TokenizerJsonPath));
    }

    // Model block — must be BPE.
    const TSharedPtr<FJsonObject>* ModelObj = nullptr;
    if (!Root->TryGetObjectField(TEXT("model"), ModelObj))
    {
        return Fail(TEXT("tokenizer.json has no 'model' field"));
    }
    FString ModelType;
    (*ModelObj)->TryGetStringField(TEXT("type"), ModelType);
    if (ModelType != TEXT("BPE"))
    {
        return Fail(FString::Printf(
            TEXT("unsupported tokenizer type '%s'; only BPE is implemented"), *ModelType));
    }

    TUniquePtr<FInoChatterboxTokenizer> Tk(new FInoChatterboxTokenizer());

    // Vocab: { "token_string": id }
    const TSharedPtr<FJsonObject>* VocabObj = nullptr;
    if (!(*ModelObj)->TryGetObjectField(TEXT("vocab"), VocabObj))
    {
        return Fail(TEXT("tokenizer.json model.vocab is missing"));
    }
    Tk->VocabStringToId.Reserve((*VocabObj)->Values.Num());
    Tk->VocabIdToString.SetNum((*VocabObj)->Values.Num());
    int32 MaxVocabId = -1;
    for (const auto& Pair : (*VocabObj)->Values)
    {
        const int64 Id = (int64)Pair.Value->AsNumber();
        Tk->VocabStringToId.Add(Pair.Key, Id);
        if (Id >= Tk->VocabIdToString.Num())
        {
            Tk->VocabIdToString.SetNum((int32)Id + 1);
        }
        Tk->VocabIdToString[(int32)Id] = Pair.Key;
        if ((int32)Id > MaxVocabId) MaxVocabId = (int32)Id;
    }

    // Merges: [ "token_a token_b", ... ] — one per rank.
    const TArray<TSharedPtr<FJsonValue>>* MergesArr = nullptr;
    if (!(*ModelObj)->TryGetArrayField(TEXT("merges"), MergesArr))
    {
        return Fail(TEXT("tokenizer.json model.merges is missing"));
    }
    Tk->MergeRanks.Reserve(MergesArr->Num());
    for (int32 Rank = 0; Rank < MergesArr->Num(); ++Rank)
    {
        const FString Merge = (*MergesArr)[Rank]->AsString();
        Tk->MergeRanks.Add(Merge, Rank);
    }

    // Added tokens (special tokens including paralinguistic).
    const TArray<TSharedPtr<FJsonValue>>* AddedArr = nullptr;
    if (Root->TryGetArrayField(TEXT("added_tokens"), AddedArr))
    {
        for (const TSharedPtr<FJsonValue>& V : *AddedArr)
        {
            const TSharedPtr<FJsonObject> Obj = V->AsObject();
            if (!Obj.IsValid()) continue;
            const int64 Id = (int64)Obj->GetNumberField(TEXT("id"));
            FString Content;
            Obj->TryGetStringField(TEXT("content"), Content);
            if (Content.IsEmpty()) continue;

            Tk->SpecialTokenIds.Add(Content, Id);
            Tk->SpecialTokenContents.Add(Content);
            // Also add to the vocab map so Decode can find special tokens.
            Tk->VocabStringToId.Add(Content, Id);
            if (Id >= Tk->VocabIdToString.Num())
            {
                Tk->VocabIdToString.SetNum((int32)Id + 1);
            }
            Tk->VocabIdToString[(int32)Id] = Content;
            if (Content == TEXT("<|endoftext|>"))
            {
                Tk->EndOfTextId = Id;
            }
        }
    }

    // Sort special tokens by length descending for greedy longest-match.
    Tk->SpecialTokenContents.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });

    // Byte-level tables.
    BuildByteLevelTables(Tk->ByteToChar, Tk->CharToByte);

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox tokenizer: loaded %d vocab entries, %d merges, %d special tokens. EOT=%lld"),
           Tk->VocabIdToString.Num(), Tk->MergeRanks.Num(),
           Tk->SpecialTokenContents.Num(), Tk->EndOfTextId);

    return Tk;
}

// ============================================================================
//  BPE core
// ============================================================================

TArray<FString> FInoChatterboxTokenizer::BpeEncode(const FString& PreToken) const
{
    // Start with each char as its own sub-token.
    TArray<FString> Word;
    Word.Reserve(PreToken.Len());
    for (TCHAR Ch : PreToken)
    {
        Word.Add(FString(1, &Ch));
    }
    if (Word.Num() < 2)
    {
        return Word;
    }

    // Classic BPE: repeatedly find the lowest-rank adjacent pair and merge.
    while (Word.Num() >= 2)
    {
        int32 BestRank = INT32_MAX;
        int32 BestIdx  = -1;
        for (int32 i = 0; i + 1 < Word.Num(); ++i)
        {
            // HuggingFace merge-key format: "<a> <b>".
            FString Key = Word[i] + TEXT(" ") + Word[i + 1];
            const int32* Rank = MergeRanks.Find(Key);
            if (Rank != nullptr && *Rank < BestRank)
            {
                BestRank = *Rank;
                BestIdx  = i;
            }
        }
        if (BestIdx < 0)
        {
            break;  // no more merges apply
        }

        // Merge all occurrences of the best pair in one pass.
        TArray<FString> Merged;
        Merged.Reserve(Word.Num() - 1);
        int32 i = 0;
        while (i < Word.Num())
        {
            if (i + 1 < Word.Num() && Word[i] == Word[BestIdx] && Word[i + 1] == Word[BestIdx + 1])
            {
                Merged.Add(Word[i] + Word[i + 1]);
                i += 2;
            }
            else
            {
                Merged.Add(Word[i]);
                i += 1;
            }
        }
        Word = MoveTemp(Merged);
    }

    return Word;
}

// ============================================================================
//  Encode pipeline
// ============================================================================

TArray<int64> FInoChatterboxTokenizer::EncodeNonSpecialSegment(const FString& Segment) const
{
    TArray<int64> Ids;
    if (Segment.IsEmpty())
    {
        return Ids;
    }

    const TArray<FString> PreTokens = PreTokenize(Segment);
    for (const FString& PreToken : PreTokens)
    {
        // Byte-level encode.
        const FString ByteLevel = FStringToByteLevel(PreToken, ByteToChar);
        // BPE merge.
        const TArray<FString> SubTokens = BpeEncode(ByteLevel);
        // Vocab lookup.
        for (const FString& Sub : SubTokens)
        {
            const int64* Id = VocabStringToId.Find(Sub);
            if (Id != nullptr)
            {
                Ids.Add(*Id);
            }
            else
            {
                // Missing from vocab (should be impossible with byte-level)
                // — log once and skip.
                UE_LOG(LogInoAgents, Warning,
                       TEXT("Chatterbox tokenizer: sub-token %s not in vocab, skipping"), *Sub);
            }
        }
    }
    return Ids;
}

TArray<int64> FInoChatterboxTokenizer::Encode(const FString& Text, bool bAddSpecialTokens) const
{
    TArray<int64> Ids;
    if (Text.IsEmpty() && !bAddSpecialTokens)
    {
        return Ids;
    }

    // Segment at special-token boundaries (longest-match first).
    int32 Pos = 0;
    while (Pos < Text.Len())
    {
        // Try to match a special token at this position.
        int32 MatchedLen = 0;
        int64 MatchedId = -1;
        for (const FString& Content : SpecialTokenContents)
        {
            if (Pos + Content.Len() <= Text.Len()
                && FCString::Strncmp(*Text + Pos, *Content, Content.Len()) == 0)
            {
                MatchedLen = Content.Len();
                MatchedId  = SpecialTokenIds.FindChecked(Content);
                break;
            }
        }

        if (MatchedLen > 0)
        {
            Ids.Add(MatchedId);
            Pos += MatchedLen;
            continue;
        }

        // No special match — consume up to the next special boundary (or end).
        int32 End = Text.Len();
        for (const FString& Content : SpecialTokenContents)
        {
            const int32 Found = Text.Find(Content, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos + 1);
            if (Found >= 0 && Found < End)
            {
                End = Found;
            }
        }
        const FString Segment = Text.Mid(Pos, End - Pos);
        Ids.Append(EncodeNonSpecialSegment(Segment));
        Pos = End;
    }

    // Post-processor: the template is:
    //   A  <|endoftext|>  <|endoftext|>
    // so we append EOT twice after the encoded text when bAddSpecialTokens.
    if (bAddSpecialTokens)
    {
        Ids.Add(EndOfTextId);
        Ids.Add(EndOfTextId);
    }

    return Ids;
}

// ============================================================================
//  Decode
// ============================================================================

FString FInoChatterboxTokenizer::Decode(TArrayView<const int64> Ids) const
{
    FString ByteLevel;
    ByteLevel.Reserve(Ids.Num() * 4);

    for (const int64 Id : Ids)
    {
        if (Id < 0 || Id >= VocabIdToString.Num())
        {
            ByteLevel += FString::Printf(TEXT("<unk:%lld>"), Id);
            continue;
        }
        ByteLevel += VocabIdToString[(int32)Id];
    }

    return ByteLevelToFString(ByteLevel, CharToByte);
}

// ============================================================================
//  Introspection
// ============================================================================

int64 FInoChatterboxTokenizer::GetSpecialTokenId(const FString& Content) const
{
    const int64* Id = SpecialTokenIds.Find(Content);
    return Id ? *Id : INT64_C(-1);
}

int32 FInoChatterboxTokenizer::GetTotalVocabSize() const
{
    return VocabIdToString.Num();
}

void FInoChatterboxTokenizer::LogSummary() const
{
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox tokenizer summary: base_vocab=%d merges=%d specials=%d total=%d eot_id=%lld"),
           GetBaseVocabSize(), MergeRanks.Num(),
           SpecialTokenContents.Num(), GetTotalVocabSize(), EndOfTextId);

    UE_LOG(LogInoAgents, Log, TEXT("Special tokens (longest-first):"));
    for (const FString& Content : SpecialTokenContents)
    {
        const int64 Id = SpecialTokenIds.FindChecked(Content);
        UE_LOG(LogInoAgents, Log, TEXT("  %s -> %lld"), *Content, Id);
    }
}
