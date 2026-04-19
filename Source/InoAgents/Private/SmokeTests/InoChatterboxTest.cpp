// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

#include "InoAgentsLog.h"
#include "InoChatterboxModels.h"
#include "InoChatterboxTokenizer.h"
#include "Onnx/InoOnnxSession.h"
#include "Onnx/InoOnnxTensor.h"
#include "Onnx/InoOnnxTypes.h"

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

    // ========================================================================
    //  Ino.Chatterbox.EmbedTest — token-ID -> fp32 embedding round-trip
    // ========================================================================

    /**
     * `Ino.Chatterbox.EmbedTest [variant] [text...]`
     *
     * Chunk-1 of Phase B3. Exercises JUST the embed_tokens session in
     * isolation:
     *
     *   Text ──(tokenizer)──▶ int64 token IDs
     *        ──(embed_tokens ONNX)──▶ fp32 [1, N, 1024] embeddings
     *
     * We deliberately skip the full FInoChatterboxModels::LoadFromDir
     * here — loading language_model + conditional_decoder on top would
     * add ~11 s per run for data we don't need. The embed_tokens session
     * alone loads in a few ms.
     *
     * Verifies:
     *   - Tensor construction with CreateFromBufferCopy<int64>
     *   - FInoOnnxSession::Run positional plumbing
     *   - Output shape matches [1, N, 1024] and dtype is Float32
     *   - No NaN / Inf in the embedding values (would indicate fp16
     *     overflow during the fused table lookup, rare but possible)
     *
     * Does NOT verify numerical correctness vs a reference — that
     * requires running the same text through the PyTorch pipeline and
     * comparing embeddings, which we'll do end-to-end in Chunk 6
     * (SynthTest) by listening to the output instead.
     */
    void RunEmbedTest(const TArray<FString>& Args)
    {
        const FString Variant = Args.Num() > 0 ? Args[0] : FString(TEXT("fp16"));

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
            Text = TEXT("Hello world");
        }

        const FString Dir            = ResolveChatterboxDir(Variant);
        const FString TokenizerPath  = FPaths::Combine(Dir, TEXT("tokenizer.json"));
        const FString EmbedOnnxPath  = FPaths::Combine(
            Dir, FString::Printf(TEXT("embed_tokens_%s.onnx"), *Variant));

        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.EmbedTest: variant=%s text=\"%s\""), *Variant, *Text);
        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.EmbedTest: dispatched (check log in ~1 s)."));

        // All work off-game-thread — tokenizer is cheap but session
        // Run() can hitch the editor on longer inputs. Logs are still
        // thread-safe so we emit directly from the worker.
        Async(EAsyncExecution::ThreadPool,
              [Variant, Text, TokenizerPath, EmbedOnnxPath]()
        {
            // 1) Tokenize.
            FString TkErr;
            TUniquePtr<FInoChatterboxTokenizer> Tk =
                FInoChatterboxTokenizer::LoadFromJson(TokenizerPath, &TkErr);
            if (!Tk.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED tokenizer load: %s"), *TkErr);
                return;
            }

            // bAddSpecialTokens=true is MANDATORY here — not optional.
            //
            // The embed_tokens.onnx graph has an internal routing Where
            // whose Equal-condition depends on the last two positions of
            // input_ids being the <|endoftext|> markers (50256) that the
            // tokenizer's post-processor appends via the template
            //     A <|endoftext|> <|endoftext|>.
            // When the last two tokens ARE 50256, Where routes a safe
            // constant into /speech_emb/Gather. When they're raw text
            // IDs (because we passed bAddSpecialTokens=false), Where
            // routes the raw text IDs into /speech_emb/Gather, which
            // fails bounds since the speech embedding table only has
            // 6563 entries.
            //
            // Diagnosed by running the official Python reference
            // (Plugins/InoAgents/Chatterbox/scripts/diagnose-embed-tokens.py)
            // against our exact .onnx file on latest ORT. Python call
            // tokenizer("Hello world") returns [15496, 995, 50256, 50256]
            // (HF AutoTokenizer defaults to add_special_tokens=True).
            // Feeding those four IDs to embed_tokens succeeds and returns
            // shape [1, 4, 1024]. Our earlier test passed only
            // [15496, 995] — two positions, no EOT trailers — so the
            // safe-index substitution never fired and the bounds error
            // tripped on our text IDs.
            const TArray<int64> Ids = Tk->Encode(Text, /*bAddSpecialTokens=*/true);
            if (Ids.Num() == 0)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED — tokenizer produced 0 tokens"));
                return;
            }

            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.EmbedTest: tokenized to %d IDs"), Ids.Num());
            {
                FString IdList;
                for (int32 i = 0; i < Ids.Num() && i < 16; ++i)
                {
                    IdList += FString::Printf(TEXT("%s%lld"), i == 0 ? TEXT("") : TEXT(", "), Ids[i]);
                }
                if (Ids.Num() > 16) IdList += TEXT(", ...");
                UE_LOG(LogInoAgents, Log, TEXT("  IDs: [%s]"), *IdList);
            }

            // 2) Load the embed_tokens session only.
            //
            // Default session options (CPU provider, full graph opts).
            // We originally set GraphOptimization=Disabled on a false lead
            // — that didn't affect the failure, which turned out to be
            // the missing trailing EOT pair in our input (see comment
            // below on bAddSpecialTokens=true). Reverted to defaults.
            FInoOnnxSessionOptions Opts;
            const double LoadT0 = FPlatformTime::Seconds();
            FString SessErr;
            TUniquePtr<FInoOnnxSession> Sess =
                FInoOnnxSession::Create(EmbedOnnxPath, Opts, &SessErr);
            const double LoadMs = (FPlatformTime::Seconds() - LoadT0) * 1000.0;
            if (!Sess.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED session load (%.1f ms): %s"),
                       LoadMs, *SessErr);
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.EmbedTest: loaded embed_tokens in %.1f ms"), LoadMs);

            // 3) Build input tensor: int64 [1, N]. Batch dim = 1.
            const TArray<int64> InShape = { 1, (int64)Ids.Num() };
            FInoOnnxTensor InputIds = FInoOnnxTensor::CreateFromBufferCopy<int64>(
                InShape, MakeArrayView(Ids));
            if (!InputIds.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED to build int64 input tensor"));
                return;
            }

            // 4) Run. Move the input into a staging TArray so we can
            //    hand a TArrayView to Run. Tensor is move-only.
            TArray<FInoOnnxTensor> Inputs;
            Inputs.Reserve(1);
            Inputs.Add(MoveTemp(InputIds));

            TArray<FInoOnnxTensor> Outputs;
            FString RunErr;
            const double RunT0 = FPlatformTime::Seconds();
            const bool bRan = Sess->Run(Inputs, Outputs, &RunErr);
            const double RunMs = (FPlatformTime::Seconds() - RunT0) * 1000.0;
            if (!bRan)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED Run (%.1f ms): %s"),
                       RunMs, *RunErr);
                return;
            }
            if (Outputs.Num() != 1)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED — expected 1 output, got %d"),
                       Outputs.Num());
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.EmbedTest: embed_tokens.Run() took %.2f ms"), RunMs);

            // 5) Validate output shape + dtype.
            const FInoOnnxTensor& Embeds = Outputs[0];
            const TArray<int64>& OutShape = Embeds.GetShape();
            const EInoOnnxDtype OutDtype  = Embeds.GetDtype();

            {
                FString ShapeStr;
                for (int32 i = 0; i < OutShape.Num(); ++i)
                {
                    ShapeStr += FString::Printf(TEXT("%s%lld"), i == 0 ? TEXT("") : TEXT(", "), OutShape[i]);
                }
                UE_LOG(LogInoAgents, Log,
                       TEXT("Ino.Chatterbox.EmbedTest: output shape=[%s] dtype=%d"),
                       *ShapeStr, (int32)OutDtype);
            }

            const bool bShapeOk = (OutShape.Num() == 3
                && OutShape[0] == 1
                && OutShape[1] == (int64)Ids.Num()
                && OutShape[2] == 1024);
            if (!bShapeOk)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED — expected shape [1, %d, 1024]"),
                       Ids.Num());
                return;
            }
            if (OutDtype != EInoOnnxDtype::Float32)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED — expected Float32 output (dtype=%d got %d)"),
                       (int32)EInoOnnxDtype::Float32, (int32)OutDtype);
                return;
            }

            // 6) Sanity-scan the raw data. We want: no NaN, no Inf, values
            //    in a plausible embedding range (typically |x| < 10).
            const float* Data = Embeds.GetData<float>();
            if (Data == nullptr)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedTest: FAILED — GetData<float>() returned null"));
                return;
            }

            const int64 Total = Embeds.GetElementCount();
            int64 NanCount = 0;
            int64 InfCount = 0;
            float MinV =  FLT_MAX;
            float MaxV = -FLT_MAX;
            double Sum  = 0.0;
            double SumAbs = 0.0;
            for (int64 i = 0; i < Total; ++i)
            {
                const float v = Data[i];
                if (FMath::IsNaN(v))  { ++NanCount; continue; }
                if (!FMath::IsFinite(v)) { ++InfCount; continue; }
                if (v < MinV) MinV = v;
                if (v > MaxV) MaxV = v;
                Sum    += v;
                SumAbs += FMath::Abs(v);
            }
            const double Mean    = (Total > 0) ? Sum / (double)Total : 0.0;
            const double MeanAbs = (Total > 0) ? SumAbs / (double)Total : 0.0;

            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.EmbedTest: stats — count=%lld min=%.4f max=%.4f mean=%.4f mean|x|=%.4f nan=%lld inf=%lld"),
                   Total, MinV, MaxV, Mean, MeanAbs, NanCount, InfCount);

            // First 8 dims of token[0]'s embedding — human-readable spot-check.
            {
                FString Preview;
                for (int32 i = 0; i < 8 && i < OutShape[2]; ++i)
                {
                    Preview += FString::Printf(TEXT("%s%+.4f"),
                                               i == 0 ? TEXT("") : TEXT(", "), Data[i]);
                }
                UE_LOG(LogInoAgents, Log,
                       TEXT("Ino.Chatterbox.EmbedTest: token[0] first 8 dims: [%s]"), *Preview);
            }

            if (NanCount > 0 || InfCount > 0)
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("Ino.Chatterbox.EmbedTest: WARN — non-finite values present (NaN=%lld, Inf=%lld)"),
                       NanCount, InfCount);
            }

            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.EmbedTest: PASS (variant=%s, %d IDs -> [1, %d, 1024] fp32, run=%.2f ms)"),
                   *Variant, Ids.Num(), Ids.Num(), RunMs);
        });
    }

    FAutoConsoleCommand GEmbedTestCmd(
        TEXT("Ino.Chatterbox.EmbedTest"),
        TEXT("Run the Chatterbox embed_tokens session on a tokenized input ")
        TEXT("and dump the fp32 embedding stats. Args: [variant] [text...]. ")
        TEXT("Variant defaults to fp16; text defaults to \"Hello world\"."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunEmbedTest));

    // ========================================================================
    //  Ino.Chatterbox.EmbedRawTest — diagnostic: embed raw integer IDs
    // ========================================================================
    //
    // This is a debugging tool, not part of the normal pipeline. It lets
    // us probe the embed_tokens.onnx graph with arbitrary integer IDs
    // (no tokenizer in the loop) to figure out what ID ranges the graph
    // actually accepts. Background: the first attempt to run
    // Ino.Chatterbox.EmbedTest with canonical GPT-2 text IDs
    // (15496="Hello", 995=" world") failed with:
    //   "Gather node '/speech_emb/Gather': idx=15496 out of bounds
    //    [-6563,6562]"
    // which suggests the graph's speech-embedding path is wired
    // unconditionally — text IDs either need routing we're missing, or
    // the graph is speech-only in practice. This command helps tell
    // which.
    //
    // Usage: Ino.Chatterbox.EmbedRawTest <variant> <id1> [id2] [id3] ...
    //        Ino.Chatterbox.EmbedRawTest fp16 0          # id 0 — safe for any table
    //        Ino.Chatterbox.EmbedRawTest fp16 6561       # START_SPEECH_TOKEN
    //        Ino.Chatterbox.EmbedRawTest fp16 6562       # STOP_SPEECH_TOKEN
    //        Ino.Chatterbox.EmbedRawTest fp16 15496      # text "Hello"
    //        Ino.Chatterbox.EmbedRawTest fp16 6561 4299  # START then SILENCE

    void RunEmbedRawTest(const TArray<FString>& Args)
    {
        if (Args.Num() < 2)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("Ino.Chatterbox.EmbedRawTest: usage: <variant> <id1> [id2] [id3] ..."));
            return;
        }
        const FString Variant = Args[0];

        TArray<int64> Ids;
        Ids.Reserve(Args.Num() - 1);
        for (int32 i = 1; i < Args.Num(); ++i)
        {
            // FCString::Atoi64 returns 0 on parse failure; accept that —
            // 0 is a legal ID and the caller explicitly chose these.
            Ids.Add((int64)FCString::Atoi64(*Args[i]));
        }

        const FString Dir = ResolveChatterboxDir(Variant);
        const FString EmbedOnnxPath = FPaths::Combine(
            Dir, FString::Printf(TEXT("embed_tokens_%s.onnx"), *Variant));

        FString IdStr;
        for (int32 i = 0; i < Ids.Num(); ++i)
        {
            IdStr += FString::Printf(TEXT("%s%lld"), i == 0 ? TEXT("") : TEXT(", "), Ids[i]);
        }
        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.EmbedRawTest: variant=%s ids=[%s]"), *Variant, *IdStr);
        UE_LOG(LogInoAgents, Log, TEXT("Ino.Chatterbox.EmbedRawTest: dispatched."));

        Async(EAsyncExecution::ThreadPool,
              [Variant, Ids, EmbedOnnxPath]()
        {
            FInoOnnxSessionOptions Opts;  // defaults

            FString SessErr;
            TUniquePtr<FInoOnnxSession> Sess =
                FInoOnnxSession::Create(EmbedOnnxPath, Opts, &SessErr);
            if (!Sess.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedRawTest: FAILED session load: %s"), *SessErr);
                return;
            }

            const TArray<int64> InShape = { 1, (int64)Ids.Num() };
            FInoOnnxTensor InputIds = FInoOnnxTensor::CreateFromBufferCopy<int64>(
                InShape, MakeArrayView(Ids));
            if (!InputIds.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedRawTest: FAILED to build int64 input tensor"));
                return;
            }

            TArray<FInoOnnxTensor> Inputs;
            Inputs.Add(MoveTemp(InputIds));

            TArray<FInoOnnxTensor> Outputs;
            FString RunErr;
            const bool bRan = Sess->Run(Inputs, Outputs, &RunErr);
            if (!bRan)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedRawTest: FAILED Run: %s"), *RunErr);
                return;
            }
            if (Outputs.Num() != 1 || !Outputs[0].IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.EmbedRawTest: FAILED — no output tensor"));
                return;
            }

            const FInoOnnxTensor& Embeds = Outputs[0];
            const TArray<int64>& OutShape = Embeds.GetShape();
            FString ShapeStr;
            for (int32 i = 0; i < OutShape.Num(); ++i)
            {
                ShapeStr += FString::Printf(TEXT("%s%lld"),
                                            i == 0 ? TEXT("") : TEXT(", "), OutShape[i]);
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.EmbedRawTest: Run OK — output shape=[%s] dtype=%d"),
                   *ShapeStr, (int32)Embeds.GetDtype());

            if (Embeds.GetDtype() == EInoOnnxDtype::Float32)
            {
                const float* Data = Embeds.GetData<float>();
                if (Data != nullptr && OutShape.Num() == 3 && OutShape[2] > 0)
                {
                    // First 8 dims of token[0]'s vector.
                    FString Preview;
                    for (int64 i = 0; i < 8 && i < OutShape[2]; ++i)
                    {
                        Preview += FString::Printf(TEXT("%s%+.4f"),
                                                   i == 0 ? TEXT("") : TEXT(", "), Data[i]);
                    }
                    UE_LOG(LogInoAgents, Log,
                           TEXT("  token[0] first 8 dims: [%s]"), *Preview);

                    // Also dump first 8 dims of each subsequent token for quick
                    // side-by-side comparison when the caller passes multiple IDs.
                    for (int64 Tok = 1; Tok < OutShape[1] && Tok < 4; ++Tok)
                    {
                        const float* TokData = Data + (Tok * OutShape[2]);
                        FString P;
                        for (int64 d = 0; d < 8 && d < OutShape[2]; ++d)
                        {
                            P += FString::Printf(TEXT("%s%+.4f"),
                                                 d == 0 ? TEXT("") : TEXT(", "), TokData[d]);
                        }
                        UE_LOG(LogInoAgents, Log,
                               TEXT("  token[%lld] first 8 dims: [%s]"), Tok, *P);
                    }
                }
            }
            UE_LOG(LogInoAgents, Log, TEXT("Ino.Chatterbox.EmbedRawTest: PASS"));
        });
    }

    FAutoConsoleCommand GEmbedRawTestCmd(
        TEXT("Ino.Chatterbox.EmbedRawTest"),
        TEXT("Diagnostic: feed raw integer IDs to embed_tokens to probe which ID ")
        TEXT("ranges the graph accepts. Args: <variant> <id1> [id2] [id3] ..."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunEmbedRawTest));

    // ========================================================================
    //  Ino.Chatterbox.ARStepTest — one forward pass through language_model
    // ========================================================================
    //
    // Chunk-2 of Phase B3. Builds the full language_model input bundle
    // for step 0 of an AR generation — text embeddings from chunk 1,
    // plus empty KV cache, attention_mask, and position_ids — and runs
    // one forward pass. Does NOT loop, does NOT sample repeatedly, does
    // NOT apply speaker conditioning (those are chunks 4–6).
    //
    // Proves:
    //   - fp16 tensor construction via Create(Float16, shape) (zero-init
    //     is what we need for empty past_key_values at step 0)
    //   - positional Run() plumbing scales to 51-input / 49-output
    //   - language_model.onnx actually executes end-to-end on fp16 KV
    //   - output logits and fp16 present-KV tensors come back with the
    //     expected shapes, dtypes, no NaN/Inf
    //
    // Does NOT prove:
    //   - output is musically meaningful — we skip the speech_encoder
    //     cond_emb prepend that real inference needs. The argmax logged
    //     here is only for sanity ("did we get finite logits out"); it's
    //     not expected to be a valid speech token until chunk 5 adds
    //     voice conditioning.
    //
    // Reference call pattern (verbatim from the HF Python reference,
    // translated to our positional Run API):
    //
    //   inputs_embeds = embed_tokens.run({input_ids})[0]          # [1,S,1024] fp32
    //   batch_size, seq_len, _ = inputs_embeds.shape
    //   past_kv = { f"past_key_values.{l}.{kv}":
    //                 zeros([batch_size, 16, 0, 64], dtype=fp16)
    //               for l in range(24) for kv in ("key", "value") }
    //   attention_mask = ones([batch_size, seq_len], int64)
    //   position_ids   = arange(seq_len, int64).reshape(1, -1)
    //   logits, *present_kv = language_model.run(dict(
    //       inputs_embeds=inputs_embeds,
    //       attention_mask=attention_mask,
    //       position_ids=position_ids,
    //       **past_kv))

    // These are Chatterbox Turbo's fp16 variant. Verified against the
    // LoadModelsTest output (51 inputs, 49 outputs).
    static constexpr int32 kChatterboxLMNumLayers       = 24;
    static constexpr int32 kChatterboxLMNumKVHeads      = 16;
    static constexpr int32 kChatterboxLMHeadDim         = 64;
    static constexpr int32 kChatterboxLMHiddenDim       = 1024;
    static constexpr int32 kChatterboxLMSpeechVocabSize = 6563;

    // Expected input-order. We validate against the live model's
    // declared names after load; a mismatch means upstream re-ordered
    // the graph and this code needs updating.
    static const TArray<FString>& ExpectedLMInputNames()
    {
        static const TArray<FString> Names = []()
        {
            TArray<FString> N;
            N.Reserve(3 + 2 * kChatterboxLMNumLayers);
            N.Add(TEXT("inputs_embeds"));
            N.Add(TEXT("attention_mask"));
            N.Add(TEXT("position_ids"));
            for (int32 L = 0; L < kChatterboxLMNumLayers; ++L)
            {
                N.Add(FString::Printf(TEXT("past_key_values.%d.key"), L));
                N.Add(FString::Printf(TEXT("past_key_values.%d.value"), L));
            }
            return N;
        }();
        return Names;
    }

    void RunARStepTest(const TArray<FString>& Args)
    {
        const FString Variant = Args.Num() > 0 ? Args[0] : FString(TEXT("fp16"));

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
            Text = TEXT("Hello world");
        }

        const FString Dir           = ResolveChatterboxDir(Variant);
        const FString TokenizerPath = FPaths::Combine(Dir, TEXT("tokenizer.json"));
        const FString EmbedOnnxPath = FPaths::Combine(
            Dir, FString::Printf(TEXT("embed_tokens_%s.onnx"), *Variant));
        const FString LMOnnxPath    = FPaths::Combine(
            Dir, FString::Printf(TEXT("language_model_%s.onnx"), *Variant));

        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.ARStepTest: variant=%s text=\"%s\""), *Variant, *Text);
        UE_LOG(LogInoAgents, Log,
               TEXT("Ino.Chatterbox.ARStepTest: dispatched (check log in ~5-10 s; LM is 635 MB)."));

        Async(EAsyncExecution::ThreadPool,
              [Variant, Text, TokenizerPath, EmbedOnnxPath, LMOnnxPath]()
        {
            // ---- 1) Tokenize (with post-processor EOT trailers) ----
            FString TkErr;
            TUniquePtr<FInoChatterboxTokenizer> Tk =
                FInoChatterboxTokenizer::LoadFromJson(TokenizerPath, &TkErr);
            if (!Tk.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED tokenizer load: %s"), *TkErr);
                return;
            }
            const TArray<int64> Ids = Tk->Encode(Text, /*bAddSpecialTokens=*/true);
            if (Ids.Num() < 2)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — need >=2 tokens, got %d"),
                       Ids.Num());
                return;
            }
            const int64 SeqLen = (int64)Ids.Num();
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: tokenized to %lld IDs"), SeqLen);

            // ---- 2) embed_tokens → inputs_embeds fp32 [1, S, 1024] ----
            FInoOnnxSessionOptions Opts;  // defaults
            FString EmbedErr;
            const double EmbedLoadT0 = FPlatformTime::Seconds();
            TUniquePtr<FInoOnnxSession> EmbedSess =
                FInoOnnxSession::Create(EmbedOnnxPath, Opts, &EmbedErr);
            const double EmbedLoadMs = (FPlatformTime::Seconds() - EmbedLoadT0) * 1000.0;
            if (!EmbedSess.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED embed_tokens load (%.1f ms): %s"),
                       EmbedLoadMs, *EmbedErr);
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: embed_tokens loaded in %.1f ms"), EmbedLoadMs);

            FInoOnnxTensor EmbedInput = FInoOnnxTensor::CreateFromBufferCopy<int64>(
                { 1, SeqLen }, MakeArrayView(Ids));
            if (!EmbedInput.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED to build embed input"));
                return;
            }

            TArray<FInoOnnxTensor> EmbedInputs;
            EmbedInputs.Add(MoveTemp(EmbedInput));
            TArray<FInoOnnxTensor> EmbedOutputs;
            FString EmbedRunErr;
            if (!EmbedSess->Run(EmbedInputs, EmbedOutputs, &EmbedRunErr))
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED embed_tokens Run: %s"),
                       *EmbedRunErr);
                return;
            }
            if (EmbedOutputs.Num() != 1 || !EmbedOutputs[0].IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — embed_tokens output missing"));
                return;
            }

            FInoOnnxTensor InputsEmbeds = MoveTemp(EmbedOutputs[0]);
            const TArray<int64>& EmbedShape = InputsEmbeds.GetShape();
            if (EmbedShape.Num() != 3 || EmbedShape[0] != 1 || EmbedShape[1] != SeqLen
                || EmbedShape[2] != kChatterboxLMHiddenDim
                || InputsEmbeds.GetDtype() != EInoOnnxDtype::Float32)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — embed output wrong shape/dtype"));
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: inputs_embeds fp32 [1, %lld, %d] ready"),
                   SeqLen, kChatterboxLMHiddenDim);

            // ---- 3) Load language_model and verify input signature ----
            const double LMLoadT0 = FPlatformTime::Seconds();
            FString LMErr;
            TUniquePtr<FInoOnnxSession> LMSess =
                FInoOnnxSession::Create(LMOnnxPath, Opts, &LMErr);
            const double LMLoadMs = (FPlatformTime::Seconds() - LMLoadT0) * 1000.0;
            if (!LMSess.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED language_model load (%.1f ms): %s"),
                       LMLoadMs, *LMErr);
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: language_model loaded in %.1f ms"), LMLoadMs);

            const TArray<FString>& Expected = ExpectedLMInputNames();
            if (LMSess->GetInputCount() != Expected.Num())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — LM has %d inputs, expected %d"),
                       LMSess->GetInputCount(), Expected.Num());
                return;
            }
            for (int32 i = 0; i < Expected.Num(); ++i)
            {
                const FString Actual = LMSess->GetInputName(i);
                if (Actual != Expected[i])
                {
                    UE_LOG(LogInoAgents, Error,
                           TEXT("Ino.Chatterbox.ARStepTest: FAILED — LM input[%d] = '%s', expected '%s'"),
                           i, *Actual, *Expected[i]);
                    return;
                }
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: LM input signature matches (%d inputs)"),
                   Expected.Num());

            // ---- 4) Build the 51-input bundle ----
            TArray<FInoOnnxTensor> LMInputs;
            LMInputs.Reserve(Expected.Num());

            // [0] inputs_embeds — already have it
            LMInputs.Add(MoveTemp(InputsEmbeds));

            // [1] attention_mask — int64 [1, S] filled with 1
            FInoOnnxTensor AttnMask = FInoOnnxTensor::Create(
                EInoOnnxDtype::Int64, { 1, SeqLen });
            if (!AttnMask.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED to alloc attention_mask"));
                return;
            }
            if (int64* MaskData = AttnMask.GetMutableData<int64>())
            {
                for (int64 i = 0; i < SeqLen; ++i) { MaskData[i] = 1; }
            }
            LMInputs.Add(MoveTemp(AttnMask));

            // [2] position_ids — int64 [1, S] = [0, 1, ..., S-1]
            FInoOnnxTensor PosIds = FInoOnnxTensor::Create(
                EInoOnnxDtype::Int64, { 1, SeqLen });
            if (!PosIds.IsValid())
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED to alloc position_ids"));
                return;
            }
            if (int64* PosData = PosIds.GetMutableData<int64>())
            {
                for (int64 i = 0; i < SeqLen; ++i) { PosData[i] = i; }
            }
            LMInputs.Add(MoveTemp(PosIds));

            // [3..50] past_key_values.L.key / .L.value — fp16 [1, 16, 0, 64]
            //
            // Zero-sized past-sequence dim means "no cache yet" at step 0.
            // Create() zero-initializes, matching Python's np.zeros(...,
            // dtype=fp16). 24 layers × 2 (key, value) = 48 tensors, so
            // total LM input count is 3 + 48 = 51.
            const TArray<int64> KVShape = { 1, kChatterboxLMNumKVHeads, 0, kChatterboxLMHeadDim };
            for (int32 L = 0; L < kChatterboxLMNumLayers; ++L)
            {
                for (int32 KV = 0; KV < 2; ++KV)
                {
                    FInoOnnxTensor T = FInoOnnxTensor::Create(EInoOnnxDtype::Float16, KVShape);
                    if (!T.IsValid())
                    {
                        UE_LOG(LogInoAgents, Error,
                               TEXT("Ino.Chatterbox.ARStepTest: FAILED to alloc past_key_values.%d.%s"),
                               L, KV == 0 ? TEXT("key") : TEXT("value"));
                        return;
                    }
                    LMInputs.Add(MoveTemp(T));
                }
            }

            check(LMInputs.Num() == Expected.Num());

            // ---- 5) Run language_model ----
            const double LMRunT0 = FPlatformTime::Seconds();
            TArray<FInoOnnxTensor> LMOutputs;
            FString LMRunErr;
            const bool bLMOk = LMSess->Run(LMInputs, LMOutputs, &LMRunErr);
            const double LMRunMs = (FPlatformTime::Seconds() - LMRunT0) * 1000.0;
            if (!bLMOk)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED LM Run (%.1f ms): %s"),
                       LMRunMs, *LMRunErr);
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: language_model.Run() took %.1f ms"), LMRunMs);

            const int32 ExpectedOutputs = 1 + 2 * kChatterboxLMNumLayers;  // logits + 48 present
            if (LMOutputs.Num() != ExpectedOutputs)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — LM returned %d outputs, expected %d"),
                       LMOutputs.Num(), ExpectedOutputs);
                return;
            }

            // ---- 6) Validate logits shape + dtype ----
            const FInoOnnxTensor& Logits = LMOutputs[0];
            const TArray<int64>& LogitShape = Logits.GetShape();
            if (LogitShape.Num() != 3 || LogitShape[0] != 1 || LogitShape[1] != SeqLen
                || LogitShape[2] != kChatterboxLMSpeechVocabSize
                || Logits.GetDtype() != EInoOnnxDtype::Float32)
            {
                FString ShapeStr;
                for (int32 i = 0; i < LogitShape.Num(); ++i)
                {
                    ShapeStr += FString::Printf(TEXT("%s%lld"),
                                                i == 0 ? TEXT("") : TEXT(","), LogitShape[i]);
                }
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — logits shape=[%s] dtype=%d (expected [1,%lld,%d] fp32)"),
                       *ShapeStr, (int32)Logits.GetDtype(), SeqLen, kChatterboxLMSpeechVocabSize);
                return;
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: logits shape [1, %lld, %d] fp32 ✓"),
                   SeqLen, kChatterboxLMSpeechVocabSize);

            // ---- 7) Validate first present KV tensor shape + dtype ----
            {
                const FInoOnnxTensor& Present0Key = LMOutputs[1];
                const TArray<int64>& PShape = Present0Key.GetShape();
                if (PShape.Num() != 4 || PShape[0] != 1
                    || PShape[1] != kChatterboxLMNumKVHeads
                    || PShape[2] != SeqLen
                    || PShape[3] != kChatterboxLMHeadDim
                    || Present0Key.GetDtype() != EInoOnnxDtype::Float16)
                {
                    FString ShapeStr;
                    for (int32 i = 0; i < PShape.Num(); ++i)
                    {
                        ShapeStr += FString::Printf(TEXT("%s%lld"),
                                                    i == 0 ? TEXT("") : TEXT(","), PShape[i]);
                    }
                    UE_LOG(LogInoAgents, Error,
                           TEXT("Ino.Chatterbox.ARStepTest: FAILED — present.0.key shape=[%s] dtype=%d"),
                           *ShapeStr, (int32)Present0Key.GetDtype());
                    return;
                }
                UE_LOG(LogInoAgents, Log,
                       TEXT("Ino.Chatterbox.ARStepTest: present.0.key shape [1,%d,%lld,%d] fp16 ✓"),
                       kChatterboxLMNumKVHeads, SeqLen, kChatterboxLMHeadDim);
            }

            // ---- 8) Scan logits for NaN/Inf + argmax of last-position logits ----
            const float* LogitData = Logits.GetData<float>();
            if (LogitData == nullptr)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — Logits.GetData<float>() returned null"));
                return;
            }

            const int64 Total = Logits.GetElementCount();
            int64 NanCount = 0, InfCount = 0;
            float MinV =  FLT_MAX, MaxV = -FLT_MAX;
            double Sum = 0.0;
            for (int64 i = 0; i < Total; ++i)
            {
                const float v = LogitData[i];
                if (FMath::IsNaN(v))      { ++NanCount; continue; }
                if (!FMath::IsFinite(v))  { ++InfCount; continue; }
                if (v < MinV) MinV = v;
                if (v > MaxV) MaxV = v;
                Sum += v;
            }
            const double Mean = (Total > 0) ? Sum / (double)Total : 0.0;
            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: logits stats — count=%lld min=%.3f max=%.3f mean=%.3f nan=%lld inf=%lld"),
                   Total, MinV, MaxV, Mean, NanCount, InfCount);

            if (NanCount > 0 || InfCount > 0)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Ino.Chatterbox.ARStepTest: FAILED — non-finite values in logits"));
                return;
            }

            // Last-position logits: logits[0, S-1, :] — this is the slice
            // an AR sampler would use to pick the next speech token.
            const int64 LastTokenOffset = (SeqLen - 1) * kChatterboxLMSpeechVocabSize;
            const float* LastLogits = LogitData + LastTokenOffset;

            // Top-5 via partial selection.
            constexpr int32 TopK = 5;
            TArray<int32> TopIdx;
            TopIdx.Reserve(TopK);
            TArray<float> TopVal;
            TopVal.Reserve(TopK);
            for (int32 k = 0; k < TopK; ++k)
            {
                float Best = -FLT_MAX;
                int32 BestIdx = -1;
                for (int32 i = 0; i < kChatterboxLMSpeechVocabSize; ++i)
                {
                    if (TopIdx.Contains(i)) continue;
                    if (LastLogits[i] > Best)
                    {
                        Best = LastLogits[i];
                        BestIdx = i;
                    }
                }
                if (BestIdx < 0) break;
                TopIdx.Add(BestIdx);
                TopVal.Add(Best);
            }

            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: last-token top-%d (no voice conditioning, so probably not a sensible token):"),
                   TopK);
            for (int32 k = 0; k < TopIdx.Num(); ++k)
            {
                FString Tag;
                if (TopIdx[k] == 6561) Tag = TEXT(" <-- START_SPEECH_TOKEN");
                else if (TopIdx[k] == 6562) Tag = TEXT(" <-- STOP_SPEECH_TOKEN");
                else if (TopIdx[k] == 4299) Tag = TEXT(" <-- SILENCE_TOKEN");
                UE_LOG(LogInoAgents, Log,
                       TEXT("   [%d] id=%5d  logit=%+.4f%s"),
                       k, TopIdx[k], TopVal[k], *Tag);
            }

            UE_LOG(LogInoAgents, Log,
                   TEXT("Ino.Chatterbox.ARStepTest: PASS (variant=%s, %lld IDs, embed=%.1f ms, LM-load=%.1f ms, LM-run=%.1f ms)"),
                   *Variant, SeqLen, EmbedLoadMs, LMLoadMs, LMRunMs);
        });
    }

    FAutoConsoleCommand GARStepTestCmd(
        TEXT("Ino.Chatterbox.ARStepTest"),
        TEXT("Run one forward pass through Chatterbox's language_model. ")
        TEXT("Tokenizes text, embeds via embed_tokens, builds empty KV cache + ")
        TEXT("attention_mask + position_ids, runs language_model once, logs the ")
        TEXT("logits shape + top-5 last-token prediction. Args: [variant] [text...]"),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunARStepTest));
}
