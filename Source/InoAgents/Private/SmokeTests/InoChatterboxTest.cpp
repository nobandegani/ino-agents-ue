// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

#include "InoAgentsLog.h"
#include "InoChatterboxModels.h"
#include "InoChatterboxTokenizer.h"

/**
 * Phase-B smoke-test console commands for the Chatterbox TTS integration.
 *
 * Right now there's only one command: LoadModelsTest. It loads the three
 * staged ORT sessions and dumps their I/O metadata to the log. We use
 * the output to design the tokenizer (Phase B2) and runners (Phase B3)
 * with correct input/output names, shapes, and dtypes — no guessing.
 *
 * All commands expect models to be staged under
 *   <Project>/Saved/PersistentDownloadDir/InoAgents/Models/Chatterbox/<variant>/
 * which is populated either by Chatterbox/scripts/setup-chatterbox.ps1
 * (dev-time) or by UInoChatterboxSubsystem::LoadModelsAsync at runtime
 * (Phase D).
 */

namespace
{
    /** Resolve the staged Chatterbox model directory for a given variant. */
    FString ResolveChatterboxDir(const FString& Variant)
    {
        return FPaths::Combine(
            FPaths::ProjectPersistentDownloadDir(),
            TEXT("InoAgents"),
            TEXT("Models"),
            TEXT("Chatterbox"),
            Variant);
    }

    /**
     * Background-thread variant of model loading. Model load does CPU-heavy
     * graph optimization inside ORT, 1-5 seconds on first run, and we don't
     * want to hitch the editor's main thread. AsyncTask dispatches back to
     * the game thread for final logging so the output order stays readable.
     */
    void LoadAndLogMetadataAsync(const FString& Variant)
    {
        const FString Dir = ResolveChatterboxDir(Variant);

        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.LoadModelsTest: variant=%s dir=%s"),
               *Variant, *Dir);

        Async(EAsyncExecution::ThreadPool, [Variant, Dir]()
        {
            const double TStart = FPlatformTime::Seconds();

            FString Error;
            TUniquePtr<FInoChatterboxModels> Models =
                FInoChatterboxModels::LoadFromDir(Dir, Variant, &Error);

            const double ElapsedMs = (FPlatformTime::Seconds() - TStart) * 1000.0;

            if (!Models.IsValid())
            {
                // FInoChatterboxModels::LoadFromDir already logged the specific error.
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.LoadModelsTest: FAILED (%.1f ms). Error: %s"),
                       ElapsedMs, *Error);
                return;
            }

            // Dump all metadata while we still own the sessions. LogMetadata
            // is thread-safe (read-only access to cached metadata arrays),
            // but we produce a LOT of log lines — dispatch to the game
            // thread so they don't interleave with other background
            // log traffic.
            TSharedPtr<FInoChatterboxModels, ESPMode::ThreadSafe> Shared(Models.Release());
            AsyncTask(ENamedThreads::GameThread, [Shared, Variant, ElapsedMs]()
            {
                Shared->LogMetadata();
                UE_LOG(LogInoAgents, Log,
                       TEXT("Ino.Chatterbox.LoadModelsTest: PASS (variant=%s, load=%.1f ms)"),
                       *Variant, ElapsedMs);
                // Shared falls out of scope here; sessions are released.
            });
        });
    }

    /**
     * `Ino.Chatterbox.LoadModelsTest [variant]`
     *
     * Loads the three staged ORT sessions for the given variant (default
     * "fp16") and logs their full I/O metadata. Use the output to:
     *   - Confirm setup-chatterbox.ps1 staged the correct files
     *   - Design the tokenizer token-space (output logit count)
     *   - Design the AR loop I/O (KV-cache shapes, position IDs, etc.)
     *   - Design the decoder I/O (speech token count, mel/PCM output)
     */
    void RunLoadModelsTest(const TArray<FString>& Args)
    {
        const FString Variant = Args.Num() > 0 ? Args[0] : FString(TEXT("fp16"));
        LoadAndLogMetadataAsync(Variant);
        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.LoadModelsTest: dispatched (check log in ~1-5 s)."));
    }

    FAutoConsoleCommand GLoadModelsTestCmd(
        TEXT("Ino.Chatterbox.LoadModelsTest"),
        TEXT("Load the three Chatterbox Turbo ORT sessions and dump their I/O metadata. ")
        TEXT("Argument: variant name (fp16 default; fp32 | fp16 | q4 | q4f16 | quantized)."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadModelsTest));

    // ========================================================================
    //  Ino.Chatterbox.TokenizerTest — GPT-2 BPE tokenizer round-trip
    // ========================================================================

    /**
     * `Ino.Chatterbox.TokenizerTest [variant] [text...]`
     *
     * Loads the HuggingFace tokenizer.json for the requested variant and
     * exercises Encode / Decode end-to-end. Logs:
     *   - Summary (vocab size, merges, special tokens)
     *   - Input text verbatim
     *   - Encoded token IDs with their sub-string representations
     *   - Decoded text
     *   - Round-trip verdict (PASS if input == decoded after stripping
     *     the post-processor's trailing `<|endoftext|>` x2)
     *
     * Default input exercises every feature we care about:
     *   - Plain text   ("Hello, world!")
     *   - Contractions ("don't")
     *   - Special tokens ([laugh])
     *   - Mixed case + punctuation
     *
     * All work runs on the game thread — the tokenizer is pure CPU string
     * processing, no async needed (unlike the model load which must
     * dispatch to a thread pool to avoid editor hitches).
     */
    void RunTokenizerTest(const TArray<FString>& Args)
    {
        const FString Variant = Args.Num() > 0 ? Args[0] : FString(TEXT("fp16"));

        // Args[1..] joined back into a single text string so the user can
        // type `Ino.Chatterbox.TokenizerTest fp16 Hello world [laugh]`
        // without quoting. Falls back to a canonical smoke-test input.
        FString Text;
        if (Args.Num() > 1)
        {
            for (int32 i = 1; i < Args.Num(); ++i)
            {
                if (!Text.IsEmpty()) { Text += TEXT(" "); }
                Text += Args[i];
            }
        }
        else
        {
            Text = TEXT("Hello, world! I don't know if this is a [laugh] test?");
        }

        const FString Dir = ResolveChatterboxDir(Variant);
        const FString TokenizerJsonPath = FPaths::Combine(Dir, TEXT("tokenizer.json"));

        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.TokenizerTest: variant=%s json=%s"),
               *Variant, *TokenizerJsonPath);

        FString Error;
        TUniquePtr<FInoChatterboxTokenizer> Tk =
            FInoChatterboxTokenizer::LoadFromJson(TokenizerJsonPath, &Error);
        if (!Tk.IsValid())
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("Ino.Chatterbox.TokenizerTest: FAILED to load tokenizer: %s"), *Error);
            return;
        }

        Tk->LogSummary();

        // Encode with the full template-processor terminator so we exercise
        // the post-processor too.
        const TArray<int64> Ids = Tk->Encode(Text, /*bAddSpecialTokens=*/true);

        UE_LOG(LogInoAgents, Log, TEXT("Input:  \"%s\""), *Text);
        UE_LOG(LogInoAgents, Log, TEXT("Encoded %d IDs:"), Ids.Num());

        // Per-ID detail. Each row: "   [i] id=12345 -> \"Hello\"" so a human
        // can eyeball that the BPE pieces look right. Decode one ID at a
        // time via a single-element TArrayView — same path Decode uses,
        // so we're not duplicating logic.
        for (int32 i = 0; i < Ids.Num(); ++i)
        {
            const int64 Id = Ids[i];
            const FString Piece = Tk->Decode(MakeArrayView(&Id, 1));
            UE_LOG(LogInoAgents, Log,
                   TEXT("   [%3d] id=%5lld -> \"%s\""), i, Id, *Piece);
        }

        // Full round-trip.
        const FString Decoded = Tk->Decode(MakeArrayView(Ids.GetData(), Ids.Num()));
        UE_LOG(LogInoAgents, Log, TEXT("Decoded: \"%s\""), *Decoded);

        // Round-trip check. The post-processor appends two `<|endoftext|>`
        // markers, which decode to the literal string. Strip those from
        // the tail before comparing.
        const FString EotStr(TEXT("<|endoftext|>"));
        FString DecodedStripped = Decoded;
        if (DecodedStripped.EndsWith(EotStr))
        {
            DecodedStripped = DecodedStripped.LeftChop(EotStr.Len());
        }
        if (DecodedStripped.EndsWith(EotStr))
        {
            DecodedStripped = DecodedStripped.LeftChop(EotStr.Len());
        }

        const bool bRoundTripMatches = (DecodedStripped == Text);
        if (bRoundTripMatches)
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.TokenizerTest: PASS (round-trip matches, %d tokens for %d chars)"),
                   Ids.Num(), Text.Len());
        }
        else
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Ino.Chatterbox.TokenizerTest: round-trip MISMATCH."));
            UE_LOG(LogInoAgents, Warning, TEXT("  Expected: \"%s\""), *Text);
            UE_LOG(LogInoAgents, Warning, TEXT("  Got:      \"%s\""), *DecodedStripped);
        }
    }

    FAutoConsoleCommand GTokenizerTestCmd(
        TEXT("Ino.Chatterbox.TokenizerTest"),
        TEXT("Load the Chatterbox tokenizer.json and round-trip an input string. ")
        TEXT("Args: [variant] [text...]. Variant defaults to fp16. ")
        TEXT("Text defaults to a built-in sample covering specials + contractions."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunTokenizerTest));
}
