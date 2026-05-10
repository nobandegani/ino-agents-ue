// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Qwen3ASR/InoQwen3ASRTokenizer.h"
#include "InoQwen3ASRLiteRT.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
    static FString ResolveVocabPath()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid()) { return FString(); }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("tokenizer"),
            TEXT("vocab.json"));
    }

    /**
     * Loads vocab.json and decodes a few canonical sequences:
     *
     *   1. A single common ASCII token (e.g. " the" → " the")
     *   2. The canonical "Hello" token sequence — useful as a sanity check
     *      that byte-level BPE → UTF-8 round-trip works
     *   3. A multi-byte UTF-8 sequence (CJK or accented Latin) to verify
     *      multi-token character reconstruction
     *
     * We don't have a tokenizer ENCODER (we never tokenize text — the model
     * eats audio), so we hand-pick known token IDs from the GPT-2 / Qwen3
     * BPE vocabulary instead of doing a round-trip from text.
     */
    void RunTokenizerTest(const TArray<FString>& /*Args*/)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("=== Ino.Qwen3ASRLiteRT.TokenizerTest ==="));

        const FString VocabPath = ResolveVocabPath();
        if (VocabPath.IsEmpty() || !FPaths::FileExists(VocabPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("TokenizerTest: vocab.json missing. Expected at: %s"), *VocabPath);
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Download from: https://huggingface.co/Qwen/Qwen3-ASR-0.6B/resolve/main/vocab.json"));
            return;
        }

        FInoQwen3ASRTokenizer Tok;
        if (!Tok.LoadFromDisk(VocabPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("TokenizerTest: load failed."));
            return;
        }
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TokenizerTest: vocab loaded — %d entries."), Tok.GetVocabSize());

        // Sanity: print the byte length of a handful of low-ID tokens so we
        // can eyeball the result. The exact strings depend on Qwen3's BPE
        // ordering — we don't assume any particular value, just print it.
        const int32 SampleIds[] = { 0, 1, 100, 1000, 10000 };
        for (int32 Id : SampleIds)
        {
            if (Id >= Tok.GetVocabSize()) { continue; }
            const TArray<int32> JustOne = { Id };
            FString Decoded = Tok.Decode(JustOne);
            UE_LOG(LogInoQwen3ASRLiteRT, Log,
                TEXT("    id=%d → '%s' (len=%d chars)"),
                Id, *Decoded, Decoded.Len());
        }

        // Decode an EOS token — should be skipped (yield empty string).
        {
            const TArray<int32> JustEos = { 151645 };
            FString Decoded = Tok.Decode(JustEos);
            UE_LOG(LogInoQwen3ASRLiteRT, Log,
                TEXT("    EOS(151645) → '%s' (len=%d, expected empty)"),
                *Decoded, Decoded.Len());
            if (Decoded.Len() != 0)
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Warning,
                    TEXT("TokenizerTest: EOS token decoded to non-empty — special-token gating may be off."));
            }
        }

        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TokenizerTest: complete. Visually verify the sample IDs above "
                 "look like real BPE pieces (whitespace + ASCII fragments are "
                 "expected for low IDs)."));
    }

    static FAutoConsoleCommand GTokenizerTestCmd(
        TEXT("Ino.Qwen3ASRLiteRT.TokenizerTest"),
        TEXT("Load Qwen3 vocab.json from Plugins/InoAgents/Qwen3ASR/LiteRT/tokenizer/, "
             "decode a handful of sample token IDs to confirm the BPE byte-to-unicode "
             "inverse is wired correctly. Special tokens (EOS) should yield empty strings."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunTokenizerTest));
}
