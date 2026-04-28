// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNanoSynthesisWorker.h"

#include "InoAgentsLog.h"
#include "InoLlama.h"                       // FLlamaCppApi + GetApi (pulls llama.h, from sibling InoLlama plugin)
#include "InoNeuTtsNanoPromptBuilder.h"
#include "InoNeuTtsNanoRunner.h"
#include "InoNeuTtsNanoVoiceRegistry.h"
#include "NeuTtsNano/InoNeuTtsNanoSubsystem.h"
#include "Onnx/InoOnnxSession.h"
#include "Onnx/InoOnnxTensor.h"
#include "Onnx/InoOnnxTypes.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Internationalization/Regex.h"
#include "Math/UnrealMathUtility.h"

// Cancel-check cadence inside the AR loop. 256 iterations at typical
// CPU throughput (20-40 tok/s) means ~6-13 s of worst-case latency
// between Cancel() and the worker acknowledging — responsive enough
// for UI purposes without costing meaningful throughput.
static constexpr int32 kCancelCheckEvery = 256;

// Hard cap on prompt+generation token count — guards against runaway
// sampling on a vocab drift (if the stop token somehow never fires).
static constexpr int32 kAbsoluteMaxTokens = 8192;

// ============================================================================
// Local helpers — regex parse + PCM conversion
// ============================================================================

namespace
{
    /**
     * Extract every <|speech_N|> match from the LM's generated text
     * into a flat TArray<int32> of FSQ speech-token ids.
     *
     * UE's FRegexMatcher is fine for this — ~1-2000 matches on a short
     * utterance, runs once post-generation. Not a hot path.
     */
    TArray<int32> ParseSpeechTokens(const FString& Text)
    {
        TArray<int32> Result;
        const FRegexPattern Pattern(TEXT("<\\|speech_(\\d+)\\|>"));
        FRegexMatcher Matcher(Pattern, Text);
        while (Matcher.FindNext())
        {
            const FString NumStr = Matcher.GetCaptureGroup(1);
            Result.Add(FCString::Atoi(*NumStr));
        }
        return Result;
    }

    /**
     * Convert NeuCodec's float32 waveform output (range ~[-1, +1]) to
     * int16 little-endian PCM bytes suitable for
     * UStreamingSoundWave::AppendAudioDataFromRAW (RuntimeAudioImporter)
     * or InoAudioFunctionLibrary::SaveInt16PcmAsWav.
     *
     * Matches the output contract Chatterbox uses so both TTS
     * subsystems' outputs are interchangeable at the audio-sink layer.
     */
    TArray<uint8> Float32ToInt16PcmLE(const float* Samples, int64 NumSamples)
    {
        TArray<uint8> Out;
        if (Samples == nullptr || NumSamples <= 0)
        {
            return Out;
        }
        Out.SetNumUninitialized(NumSamples * (int64)sizeof(int16));
        int16* Pcm = reinterpret_cast<int16*>(Out.GetData());
        for (int64 i = 0; i < NumSamples; ++i)
        {
            const float S = FMath::Clamp(Samples[i], -1.0f, 1.0f);
            Pcm[i] = (int16)FMath::RoundToInt(S * 32767.0f);
        }
        return Out;
    }

    /**
     * Tokenize a UTF-8 string via the llama.cpp vtable. Handles the
     * two-pass size-probe dance the C API requires.
     *
     * add_special=true because the Qwen2 chat template expects its
     * BOS/EOS handling. parse_special=true so <|TEXT_PROMPT_START|>,
     * <|speech_N|>, etc. resolve as single special tokens rather than
     * being split into raw UTF-8 bytes.
     */
    TArray<llama_token> TokenizePrompt(
        const InoAgents::LlamaCpp::FLlamaCppApi& Api,
        const struct llama_vocab* Vocab,
        const FString& Prompt)
    {
        const FTCHARToUTF8 Utf8(*Prompt);
        const int32 Utf8Len = Utf8.Length();

        // First call: probe size (negative return = -needed).
        const int32 Probe = Api.llama_tokenize(
            Vocab, Utf8.Get(), Utf8Len,
            /*tokens=*/nullptr, /*n_tokens_max=*/0,
            /*add_special=*/true, /*parse_special=*/true);
        const int32 Needed = Probe < 0 ? -Probe : Probe;
        if (Needed <= 0)
        {
            return TArray<llama_token>();
        }

        TArray<llama_token> Out;
        Out.SetNumUninitialized(Needed);
        const int32 N = Api.llama_tokenize(
            Vocab, Utf8.Get(), Utf8Len,
            Out.GetData(), Needed,
            /*add_special=*/true, /*parse_special=*/true);
        if (N < 0)
        {
            Out.Reset();
            return Out;
        }
        Out.SetNum(N);
        return Out;
    }
} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

FInoNeuTtsNanoSynthesisWorker::FInoNeuTtsNanoSynthesisWorker(
    TWeakObjectPtr<UInoNeuTtsNanoSubsystem> InOwner,
    FInoNeuTtsNanoRunner* InRunner,
    const FInoNeuTtsNanoVoiceRegistry* InVoiceRegistry)
    : WeakSubsystem(InOwner)
    , Runner(InRunner)
    , VoiceRegistry(InVoiceRegistry)
{
    QueueEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);
    Thread = FRunnableThread::Create(
        this,
        TEXT("InoNeuTtsNanoSynthesisWorker"),
        /*StackSize=*/ 0,
        TPri_Normal);

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Worker: ctor — thread created (runner=%s, voices=%s)"),
           Runner ? TEXT("alive") : TEXT("null"),
           VoiceRegistry ? TEXT("alive") : TEXT("null"));
}

FInoNeuTtsNanoSynthesisWorker::~FInoNeuTtsNanoSynthesisWorker()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Worker: dtor — signalling stop + joining thread"));

    bStopRequested  = true;
    bStreamCancelled = true;

    if (QueueEvent != nullptr)
    {
        QueueEvent->Trigger();
    }

    if (Thread != nullptr)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }

    if (QueueEvent != nullptr)
    {
        FPlatformProcess::ReturnSynchEventToPool(QueueEvent);
        QueueEvent = nullptr;
    }

    // Drain any residual queue entries so their captured dynamic
    // delegates get released cleanly. They never fired; the subsystem
    // won't fire them either (we're mid-teardown). Chatterbox's
    // SynthesisWorker takes the same approach.
    int32 Drained = 0;
    FInoNeuTtsNanoPendingSynth Discard;
    while (Queue.Dequeue(Discard)) { ++Drained; }
    if (Drained > 0)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTtsNano: Worker: dtor drained %d undelivered pending synth(es)"),
               Drained);
    }
}

void FInoNeuTtsNanoSynthesisWorker::Stop()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Worker: Stop observed — flagging bStopRequested + bStreamCancelled"));
    bStopRequested  = true;
    bStreamCancelled = true;
    if (QueueEvent != nullptr)
    {
        QueueEvent->Trigger();
    }
}

void FInoNeuTtsNanoSynthesisWorker::SignalCancel()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Worker: SignalCancel observed — bStreamCancelled=true"));
    bStreamCancelled = true;
    // Don't trigger QueueEvent — cancellation means "abandon the
    // in-flight synthesis", not "wake from idle".
}

void FInoNeuTtsNanoSynthesisWorker::Enqueue(FInoNeuTtsNanoPendingSynth Pending)
{
    UE_LOG(LogInoAgents, Verbose,
           TEXT("NeuTtsNano: Worker: Enqueue (voice=%s, phonemes=%d chars, streaming=%s)"),
           *Pending.VoiceName.ToString(),
           Pending.PhonemesText.Len(),
           Pending.bStreamingEnabled ? TEXT("yes") : TEXT("no"));
    Queue.Enqueue(MoveTemp(Pending));
    if (QueueEvent != nullptr)
    {
        QueueEvent->Trigger();
    }
}

// ============================================================================
// Run loop
// ============================================================================

uint32 FInoNeuTtsNanoSynthesisWorker::Run()
{
    UE_LOG(LogInoAgents, Log, TEXT("NeuTtsNano: Worker: Run entry — thread loop starting"));

    while (!bStopRequested.Load())
    {
        FInoNeuTtsNanoPendingSynth Pending;
        if (Queue.Dequeue(Pending))
        {
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("NeuTtsNano: Worker: dequeued synth (voice=%s, streaming=%s) — dispatching to ProcessSynth"),
                   *Pending.VoiceName.ToString(),
                   Pending.bStreamingEnabled ? TEXT("yes") : TEXT("no"));

            // Clear cancel-for-previous-synth flag before starting the
            // next one. Cancel only applies to the in-flight synthesis,
            // not the next queued one.
            bStreamCancelled = false;
            ProcessSynth(Pending);
            continue;
        }

        // Queue empty — block until Enqueue or Stop triggers.
        UE_LOG(LogInoAgents, Verbose,
               TEXT("NeuTtsNano: Worker: queue empty — blocking on QueueEvent"));
        if (QueueEvent != nullptr)
        {
            QueueEvent->Wait();
        }
    }

    UE_LOG(LogInoAgents, Log, TEXT("NeuTtsNano: Worker: Run exit — thread loop terminating"));
    return 0;
}

// ============================================================================
// Game-thread dispatch
// ============================================================================

void FInoNeuTtsNanoSynthesisWorker::DispatchCompleteOnGameThread(
    FOnInoNeuTtsNanoSynthesisComplete OnComplete,
    bool             bSuccess,
    TArray<uint8>    PcmInt16LE,
    FString          ErrorMessage)
{
    // Dynamic delegates must fire on the game thread. Capture
    // everything by value / MoveTemp into the lambda; capture the
    // weak-ptr so we can verify the subsystem is still alive (though
    // strictly we only use it for the IsValid() sanity check — the
    // delegate itself is a standalone FScriptDelegate handle).
    TWeakObjectPtr<UInoNeuTtsNanoSubsystem> WeakSelf = WeakSubsystem;
    AsyncTask(ENamedThreads::GameThread,
        [WeakSelf, OnComplete, bSuccess,
         Pcm = MoveTemp(PcmInt16LE),
         Err = MoveTemp(ErrorMessage)]() mutable
        {
            if (!WeakSelf.IsValid())
            {
                // Subsystem torn down mid-synth — drop the callback.
                // Nothing to notify; the PCM bytes and error string
                // are freed as the lambda goes out of scope.
                return;
            }
            OnComplete.ExecuteIfBound(bSuccess, MoveTemp(Pcm), Err);
        });
}

void FInoNeuTtsNanoSynthesisWorker::DispatchAudioChunkOnGameThread(
    FOnInoNeuTtsNanoAudioChunk OnAudioChunk,
    TArray<uint8>              AudioChunk,
    bool                       bIsFinal,
    int32                      NumSpeechIds)
{
    // Same pattern as DispatchCompleteOnGameThread — dynamic delegate
    // invocation must happen on the game thread. Move the audio bytes
    // into the lambda so we don't pay for a copy.
    TWeakObjectPtr<UInoNeuTtsNanoSubsystem> WeakSelf = WeakSubsystem;
    AsyncTask(ENamedThreads::GameThread,
        [WeakSelf, OnAudioChunk,
         Chunk = MoveTemp(AudioChunk),
         bIsFinal, NumSpeechIds]() mutable
        {
            if (!WeakSelf.IsValid())
            {
                return;
            }
            OnAudioChunk.ExecuteIfBound(Chunk, bIsFinal, NumSpeechIds);
        });
}

// ============================================================================
// Main synthesis pipeline
//
// Mirrors the NeuTTS Nano Python reference (neuphonic/neutts,
// neutts/neutts.py :: infer → _infer_ggml → _decode):
//
//   1. Build prompt string
//   2. Tokenize (parse_special=true so control tokens resolve)
//   3. Clear KV-cache state from any previous synthesis
//   4. Decode prompt into context (processes all prompt tokens)
//   5. Sampler chain: top-k → temperature → dist (seed)
//   6. AR loop: sample next token → append to output text → feed back
//      into context. Stop on stop-token / EOG / MaxNewTokens / cancel.
//   7. Regex-parse <|speech_N|> ids from generated text
//   8. Run NeuCodec ONNX decoder with int32[1,1,N] input
//   9. Float32 → int16 PCM LE conversion
//  10. AsyncTask back to game thread to fire OnComplete
//
// Every failure path fires OnComplete(false, {}, "...") exactly once.
// Happy path fires OnComplete(true, pcm, "") exactly once.
//
// kOutputSampleRate (24000) is used only for diagnostic real-time-factor
// calculations and for fractional-seconds log lines — it's NOT passed
// through to the delegate (the caller reads it from
// UInoNeuTtsNanoSubsystem::GetOutputSampleRate()).
// ============================================================================

void FInoNeuTtsNanoSynthesisWorker::ProcessSynth(FInoNeuTtsNanoPendingSynth& Pending)
{
    constexpr int32 kOutputSampleRate = 24000;
    const double StartTime = FPlatformTime::Seconds();

    auto FailWith = [&](const TCHAR* Fmt) -> void
    {
        const FString Err(Fmt);
        UE_LOG(LogInoAgents, Error, TEXT("NeuTtsNano: Runner: synth FAILED: %s"), *Err);
        DispatchCompleteOnGameThread(
            Pending.OnComplete, false, TArray<uint8>(), Err);
    };
    auto FailWithFmt = [&](const FString& Err) -> void
    {
        UE_LOG(LogInoAgents, Error, TEXT("NeuTtsNano: Runner: synth FAILED: %s"), *Err);
        DispatchCompleteOnGameThread(
            Pending.OnComplete, false, TArray<uint8>(), Err);
    };

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: ProcessSynth begin (voice=%s, phonemes=%d chars, "
                "max_new=%d, streaming=%s, chunk_tokens=%d)"),
           *Pending.VoiceName.ToString(),
           Pending.PhonemesText.Len(),
           Pending.Options.MaxNewTokens,
           Pending.bStreamingEnabled ? TEXT("yes") : TEXT("no"),
           Pending.Options.StreamChunkTokens);

    // -----------------------------------------------------------------
    // Preconditions
    // -----------------------------------------------------------------
    if (Runner == nullptr)
    {
        FailWith(TEXT("Runner is null — model not loaded."));
        return;
    }
    const InoAgents::LlamaCpp::FLlamaCppApi* Api = InoAgents::LlamaCpp::GetApi();
    if (Api == nullptr)
    {
        FailWith(TEXT("llama.cpp runtime is not available."));
        return;
    }
    if (VoiceRegistry == nullptr)
    {
        FailWith(TEXT("VoiceRegistry is null."));
        return;
    }

    // Resolve voice — default to "Default" if caller passed NAME_None.
    const FName VoiceName = Pending.VoiceName.IsNone()
        ? FName(TEXT("Default"))
        : Pending.VoiceName;
    const FInoNeuTtsNanoVoice* Voice = VoiceRegistry->Find(VoiceName);
    if (Voice == nullptr)
    {
        FailWithFmt(FString::Printf(
            TEXT("Voice \"%s\" not registered."), *VoiceName.ToString()));
        return;
    }
    if (Voice->IsPlaceholder())
    {
        FailWith(TEXT("Default voice is a placeholder (empty ref_codes). "
                      "Regenerate via Plugins/InoAgents/NeuTtsNano/scripts/"
                      "encode-default-voice.py — see NeuTtsNano/README.md."));
        return;
    }
    if (Voice->RefPhones.IsEmpty())
    {
        FailWith(TEXT("Voice has empty ref_phones. Re-run "
                      "encode-default-voice.py (the --language=none path "
                      "skips phonemization; default en-us is required for "
                      "prompt construction)."));
        return;
    }
    if (Pending.PhonemesText.IsEmpty())
    {
        FailWith(TEXT("SynthesizeAsync called with empty phonemes. v1 "
                      "requires pre-phonemized input (no runtime text-to-"
                      "phoneme)."));
        return;
    }

    // -----------------------------------------------------------------
    // 1. Build prompt + tokenize
    // -----------------------------------------------------------------
    const double PromptBuildStart = FPlatformTime::Seconds();
    const FString Prompt = InoNeuTtsNano::BuildPrompt(
        Voice->RefPhones, Voice->RefCodes, Pending.PhonemesText);
    const double PromptBuildMs = (FPlatformTime::Seconds() - PromptBuildStart) * 1000.0;

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: prompt built in %.1f ms (%d chars, ref_phones=%d chars, "
                "ref_codes=%d, input_phonemes=%d chars)"),
           PromptBuildMs, Prompt.Len(),
           Voice->RefPhones.Len(), Voice->RefCodes.Num(), Pending.PhonemesText.Len());

    const struct llama_vocab* Vocab = Api->llama_model_get_vocab(Runner->GetModel());
    if (Vocab == nullptr)
    {
        FailWith(TEXT("llama_model_get_vocab returned null."));
        return;
    }

    const double TokenizeStart = FPlatformTime::Seconds();
    TArray<llama_token> PromptTokens = TokenizePrompt(*Api, Vocab, Prompt);
    const double TokenizeMs = (FPlatformTime::Seconds() - TokenizeStart) * 1000.0;
    if (PromptTokens.Num() == 0)
    {
        FailWith(TEXT("llama_tokenize produced zero tokens."));
        return;
    }
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: tokenize done (%d tokens, %.1f ms)"),
           PromptTokens.Num(), TokenizeMs);

    const uint32_t NCtx = Api->llama_n_ctx(Runner->GetContext());
    if ((uint32_t)PromptTokens.Num() >= NCtx)
    {
        FailWithFmt(FString::Printf(
            TEXT("Prompt has %d tokens, exceeds context size %u. "
                 "Reduce ref_codes (shorter reference voice) or raise "
                 "FInoNeuTtsNanoModelConfig::NumContextTokens."),
            PromptTokens.Num(), NCtx));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: synth config — prompt=%d tokens, voice=\"%s\", "
                "ref_codes=%d, target_phonemes=%d chars, max_new=%d, n_ctx=%u"),
           PromptTokens.Num(),
           *Voice->DisplayName,
           Voice->RefCodes.Num(),
           Pending.PhonemesText.Len(),
           Pending.Options.MaxNewTokens,
           NCtx);

    // -----------------------------------------------------------------
    // 2. Clear KV-cache state from any previous synthesis and decode
    //    the prompt.
    // -----------------------------------------------------------------
    if (Api->llama_get_memory != nullptr && Api->llama_memory_clear != nullptr)
    {
        llama_memory_t Mem = Api->llama_get_memory(Runner->GetContext());
        if (Mem != nullptr)
        {
            Api->llama_memory_clear(Mem, /*data=*/false);
        }
    }

    {
        const double PrefillStart = FPlatformTime::Seconds();
        struct llama_batch PromptBatch = Api->llama_batch_get_one(
            PromptTokens.GetData(), PromptTokens.Num());
        const int32 Rc = Api->llama_decode(Runner->GetContext(), PromptBatch);
        const double PrefillMs = (FPlatformTime::Seconds() - PrefillStart) * 1000.0;
        if (Rc != 0)
        {
            FailWithFmt(FString::Printf(
                TEXT("llama_decode on prompt failed (rc=%d)."), Rc));
            return;
        }
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano: Runner: llama_decode prefill complete (ctx=%d tokens, %.1f ms)"),
               PromptTokens.Num(), PrefillMs);
    }

    // -----------------------------------------------------------------
    // 3. Build sampler chain (top-k → top-p → min-p → temp → dist).
    //
    // Exact order matches llama-cpp-python's default pipeline that
    // NeuTTS's Python _infer_ggml inherits from Llama.__call__ defaults:
    //
    //     top_k = 50        (NeuTTS explicit override)
    //     top_p = 0.95      (llama-cpp-python default — implicit)
    //     min_p = 0.05      (llama-cpp-python default — implicit)
    //     temperature = 1.0 (NeuTTS explicit override)
    //     dist (seed)       (sample)
    //
    // min_keep=1 on both top_p and min_p matches llama-cpp-python's
    // Llama._create_completion path (it never leaves a top-p / min-p
    // filter with zero candidates — the minimum-keep floor prevents
    // degenerate sampling when probability mass is extremely
    // concentrated).
    // -----------------------------------------------------------------
    struct llama_sampler* Sampler = nullptr;
    {
        struct llama_sampler_chain_params CP = Api->llama_sampler_chain_default_params();
        Sampler = Api->llama_sampler_chain_init(CP);
        if (Sampler == nullptr)
        {
            FailWith(TEXT("llama_sampler_chain_init returned null."));
            return;
        }
        Api->llama_sampler_chain_add(Sampler,
            Api->llama_sampler_init_top_k(Pending.Options.TopK));
        Api->llama_sampler_chain_add(Sampler,
            Api->llama_sampler_init_top_p(Pending.Options.TopP, /*min_keep=*/ 1));
        Api->llama_sampler_chain_add(Sampler,
            Api->llama_sampler_init_min_p(Pending.Options.MinP, /*min_keep=*/ 1));
        Api->llama_sampler_chain_add(Sampler,
            Api->llama_sampler_init_temp(Pending.Options.Temperature));

        const uint32_t Seed = (Pending.Options.Seed < 0)
            ? (uint32_t)FMath::Rand()
            : (uint32_t)Pending.Options.Seed;
        Api->llama_sampler_chain_add(Sampler,
            Api->llama_sampler_init_dist(Seed));
    }

    // RAII-ish cleanup for Sampler on every exit path.
    auto FreeSampler = [&]() {
        if (Sampler != nullptr)
        {
            Api->llama_sampler_free(Sampler);
            Sampler = nullptr;
        }
    };

    // -----------------------------------------------------------------
    // Streaming state
    //
    // Populated regardless of bStreamingEnabled, but only consumed in
    // the streaming branch. The per-iteration regex cost is tiny (the
    // scan is limited to the newly-appended text via
    // FRegexMatcher::SetLimits) so we don't bother branching it off in
    // one-shot mode — the values stay unused.
    //
    // FullPcm accumulates every delta chunk dispatched via
    // OnAudioChunk. OnComplete's final PcmInt16LE is this exact buffer
    // — see the "OnComplete's full_bytes == concat of emitted chunks"
    // contract documented on FOnInoNeuTtsNanoAudioChunk. This avoids a
    // subtle correctness trap: re-running the decoder on growing
    // prefixes produces slightly different samples near each
    // intermediate chunk boundary (convolutional-codec right-edge
    // effects, since the receptive field of the final conv layer
    // can't see codes that haven't been generated yet), so the concat
    // of emitted deltas differs from a fresh one-shot decode on the
    // full prefix. Using concat-of-chunks as OnComplete's payload
    // makes the two ways a caller might consume the result (chunk-by-
    // chunk vs. all-at-once) agree byte-for-byte.
    // -----------------------------------------------------------------
    static const FRegexPattern SpeechPatternStream(TEXT("<\\|speech_(\\d+)\\|>"));

    TArray<int32> SpeechIds;
    TArray<uint8> FullPcm;             // concat of dispatched chunk deltas
    int32 SpeechScanPos        = 0;    // next char to scan in GeneratedText
    int32 LastEmittedByteCount = 0;    // total PCM bytes dispatched so far
    int32 LastDecodedCodeCount = 0;    // SpeechIds.Num() at most recent decode
    int32 ChunksDispatched     = 0;    // diagnostic counter

    // Decode-on-prefix helper. Runs the NeuCodec decoder on the full
    // current SpeechIds, converts the fresh float32 waveform to int16
    // PCM LE bytes, slices out the delta since the last emission, and
    // marshals it to the game thread via OnAudioChunk. Returns false
    // on failure (in which case FailWith/FailWithFmt has already fired
    // OnComplete(false,…), and the caller should `return;`).
    //
    // Callers: (a) AR loop when SpeechIds grows past chunk threshold;
    // (b) end-of-loop streaming branch to emit the final chunk.
    auto RunIntermediateDecodeEmit = [&](bool bIsFinal) -> bool
    {
        FInoOnnxTensor CodesTensor = FInoOnnxTensor::CreateFromBufferCopy<int32>(
            { 1, 1, (int64)SpeechIds.Num() },
            SpeechIds);
        if (!CodesTensor.IsValid())
        {
            FreeSampler();
            FailWith(TEXT("Stream: failed to allocate codec input tensor."));
            return false;
        }

        FInoOnnxSession* LocalSession = Runner->GetCodecSession();
        if (LocalSession == nullptr)
        {
            FreeSampler();
            FailWith(TEXT("Stream: codec session is null."));
            return false;
        }

        TArray<FInoOnnxTensor> Ins;
        Ins.Add(MoveTemp(CodesTensor));
        TArray<FInoOnnxTensor> Outs;
        FString OrtErr;
        if (!LocalSession->Run(Ins, Outs, &OrtErr))
        {
            FreeSampler();
            FailWithFmt(FString::Printf(
                TEXT("Stream: NeuCodec decoder Run failed (codes=%d): %s"),
                SpeechIds.Num(), *OrtErr));
            return false;
        }
        if (Outs.Num() == 0 || Outs[0].GetDtype() != EInoOnnxDtype::Float32)
        {
            FreeSampler();
            FailWith(TEXT("Stream: decoder returned no/invalid output."));
            return false;
        }
        const float* WavData = Outs[0].GetData<float>();
        const int64  WavNum  = Outs[0].GetElementCount();
        if (WavData == nullptr || WavNum <= 0)
        {
            FreeSampler();
            FailWith(TEXT("Stream: decoder output tensor is empty."));
            return false;
        }

        TArray<uint8> AllBytes = Float32ToInt16PcmLE(WavData, WavNum);
        TArray<uint8> Delta;
        if (AllBytes.Num() > LastEmittedByteCount)
        {
            const int32 DeltaLen = AllBytes.Num() - LastEmittedByteCount;
            Delta.Append(AllBytes.GetData() + LastEmittedByteCount, DeltaLen);
        }

        LastEmittedByteCount = AllBytes.Num();
        LastDecodedCodeCount = SpeechIds.Num();
        ++ChunksDispatched;
        FullPcm.Append(Delta);

        DispatchAudioChunkOnGameThread(
            Pending.OnAudioChunk,
            MoveTemp(Delta),
            bIsFinal,
            SpeechIds.Num());
        return true;
    };

    // -----------------------------------------------------------------
    // 4. AR loop — sample → detokenize → feed back
    // -----------------------------------------------------------------
    const int32 StopTokenId = Runner->GetStopTokenId();
    const int32 MaxNew = FMath::Min(
        Pending.Options.MaxNewTokens,
        (int32)(NCtx - (uint32_t)PromptTokens.Num() - 4));   // 4-token safety margin
    const int32 HardCap = FMath::Min(MaxNew, kAbsoluteMaxTokens);

    FString GeneratedText;
    GeneratedText.Reserve(HardCap * 18);   // ~"<|speech_12345|>" per token

    int32 TokensGenerated = 0;
    int32 CancelCounter   = 0;
    bool  bHitStop        = false;

    char PieceBuf[128];

    const double DecodeStart = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: AR loop begin (hard_cap=%d, stop_token_id=%d)"),
           HardCap, StopTokenId);

    while (TokensGenerated < HardCap)
    {
        // Sample next token.
        llama_token Next = Api->llama_sampler_sample(
            Sampler, Runner->GetContext(), /*idx=*/-1);

        UE_LOG(LogInoAgents, Verbose,
               TEXT("NeuTtsNano: Runner: AR iter %d/%d — sampled token=%d"),
               TokensGenerated + 1, HardCap, (int32)Next);

        // Stop conditions BEFORE accepting (matches upstream convention;
        // an EOG token shouldn't enter the sampler's history).
        if (StopTokenId >= 0 && Next == (llama_token)StopTokenId)
        {
            bHitStop = true;
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("NeuTtsNano: Runner: AR loop hit <|SPEECH_GENERATION_END|> at iter %d"),
                   TokensGenerated + 1);
            break;
        }
        if (Api->llama_vocab_is_eog(Vocab, Next))
        {
            bHitStop = true;
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("NeuTtsNano: Runner: AR loop hit vocab EOG at iter %d (token=%d)"),
                   TokensGenerated + 1, (int32)Next);
            break;
        }

        // Cancel check every N iterations. Cheap atomic read; cadence
        // amortises it to < 0.5% of loop overhead.
        if (++CancelCounter >= kCancelCheckEvery)
        {
            CancelCounter = 0;
            if (bStreamCancelled.Load())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("NeuTtsNano: Runner: AR loop cancelled at iter %d "
                            "(bStreamCancelled observed)"),
                       TokensGenerated + 1);
                FreeSampler();
                FailWith(TEXT("Cancelled by caller."));
                return;
            }
        }

        Api->llama_sampler_accept(Sampler, Next);

        // Detokenize into generated text buffer.
        const int32 PieceLen = Api->llama_token_to_piece(
            Vocab, Next, PieceBuf, (int32)sizeof(PieceBuf) - 1,
            /*lstrip=*/0, /*special=*/true);
        if (PieceLen > 0 && PieceLen < (int32)sizeof(PieceBuf))
        {
            PieceBuf[PieceLen] = '\0';
            GeneratedText.Append(UTF8_TO_TCHAR(PieceBuf));
        }

        ++TokensGenerated;

        // Streaming: scan the newly-appended portion of GeneratedText
        // for <|speech_N|> matches, then emit an intermediate chunk if
        // we've accumulated enough new FSQ codes since the last decode.
        //
        // Constructing a fresh FRegexMatcher each iteration is cheap —
        // SetLimits constrains the match region to [SpeechScanPos,
        // GeneratedText.Len()) so the scanner only processes the newly
        // appended characters. Amortised O(N) across the loop.
        if (Pending.bStreamingEnabled)
        {
            FRegexMatcher SM(SpeechPatternStream, GeneratedText);
            SM.SetLimits(SpeechScanPos, GeneratedText.Len());
            while (SM.FindNext())
            {
                const FString NumStr = SM.GetCaptureGroup(1);
                SpeechIds.Add(FCString::Atoi(*NumStr));
                SpeechScanPos = SM.GetMatchEnding();
            }

            const int32 ChunkSize = Pending.Options.StreamChunkTokens;
            if (ChunkSize > 0
                && SpeechIds.Num() - LastDecodedCodeCount >= ChunkSize)
            {
                UE_LOG(LogInoAgents, Verbose,
                       TEXT("NeuTtsNano: Runner: stream decode trigger at AR iter %d "
                            "(codes=%d, last_decoded=%d, threshold=%d)"),
                       TokensGenerated, SpeechIds.Num(), LastDecodedCodeCount, ChunkSize);
                if (!RunIntermediateDecodeEmit(/*bIsFinal=*/ false))
                {
                    // Error already dispatched; abandon synthesis.
                    return;
                }
            }
        }

        // Feed back into context — one-token batch using a stack
        // variable for the token storage. llama_batch_get_one takes a
        // pointer; the batch struct holds a reference to it, which is
        // valid until llama_decode returns.
        llama_token NextSingleton = Next;
        struct llama_batch StepBatch = Api->llama_batch_get_one(&NextSingleton, 1);
        const int32 DRc = Api->llama_decode(Runner->GetContext(), StepBatch);
        if (DRc != 0)
        {
            FreeSampler();
            FailWithFmt(FString::Printf(
                TEXT("llama_decode (step %d) failed (rc=%d)."),
                TokensGenerated, DRc));
            return;
        }
    }

    FreeSampler();

    const double DecodeMs = (FPlatformTime::Seconds() - DecodeStart) * 1000.0;
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: AR loop complete (%d tokens generated, hit_stop=%s, %.0f ms, %.1f tok/s)"),
           TokensGenerated,
           bHitStop ? TEXT("yes") : TEXT("no"),
           DecodeMs,
           TokensGenerated / FMath::Max(0.001, DecodeMs / 1000.0));

    // -----------------------------------------------------------------
    // 5a. Streaming branch — emit the final chunk (bIsFinal=true) and
    //     dispatch OnComplete with the concatenated PCM.
    //
    // SpeechIds is already populated (incremental regex ran inside the
    // AR loop). If the AR loop produced more codes than the most
    // recent intermediate decode covered, do one last decode to pick
    // up the tail; otherwise emit an empty terminator so the handler's
    // bIsFinal=true contract is upheld.
    // -----------------------------------------------------------------
    if (Pending.bStreamingEnabled)
    {
        if (SpeechIds.Num() == 0)
        {
            FailWithFmt(FString::Printf(
                TEXT("Stream: LM generated %d tokens but zero matched <|speech_N|>. "
                     "Fine-tuned vocab drift or wrong model? First 200 chars: %s"),
                TokensGenerated, *GeneratedText.Left(200)));
            return;
        }

        if (SpeechIds.Num() > LastDecodedCodeCount)
        {
            if (!RunIntermediateDecodeEmit(/*bIsFinal=*/ true))
            {
                return;   // Error already dispatched.
            }
        }
        else
        {
            // No undecoded codes — the last intermediate emission
            // already covered everything. Fire a synthetic empty
            // terminator so the consumer sees bIsFinal=true exactly
            // once (per the delegate's documented contract).
            DispatchAudioChunkOnGameThread(
                Pending.OnAudioChunk,
                TArray<uint8>(),
                /*bIsFinal=*/ true,
                SpeechIds.Num());
            ++ChunksDispatched;
        }

        const double TotalMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
        const int64  NumSamples = (int64)(FullPcm.Num() / (int32)sizeof(int16));
        const double AudioSec = (double)NumSamples / (double)kOutputSampleRate;
        const double RtFactor = AudioSec * 1000.0 / FMath::Max(1.0, TotalMs);
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano: Runner: stream DONE — %d chunks, %lld samples "
                    "(%.2f s audio @ %d Hz), %d bytes total, %.0f ms wall (%.2fx real-time)"),
               ChunksDispatched, NumSamples, AudioSec, kOutputSampleRate,
               FullPcm.Num(), TotalMs, RtFactor);

        DispatchCompleteOnGameThread(
            Pending.OnComplete,
            /*bSuccess=*/ true,
            MoveTemp(FullPcm),
            /*ErrorMessage=*/ FString());
        return;
    }

    // -----------------------------------------------------------------
    // 5b. One-shot branch — regex-parse speech-token ids from the LM
    //     output text (streaming populates SpeechIds incrementally, but
    //     in one-shot mode it's still empty here).
    // -----------------------------------------------------------------
    SpeechIds = ParseSpeechTokens(GeneratedText);
    if (SpeechIds.Num() == 0)
    {
        FailWithFmt(FString::Printf(
            TEXT("LM generated %d tokens but zero matched <|speech_N|>. "
                 "Fine-tuned vocab drift or wrong model? First 200 chars: %s"),
            TokensGenerated,
            *GeneratedText.Left(200)));
        return;
    }
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: parsed %d speech-token ids from LM output"),
           SpeechIds.Num());

    // -----------------------------------------------------------------
    // 6. Build decoder input tensor: int32 [1, 1, N].
    //    (NeuCodec ONNX decoder's expected input shape, per
    //    neuphonic/neucodec-onnx-decoder.)
    // -----------------------------------------------------------------
    FInoOnnxTensor CodesTensor = FInoOnnxTensor::CreateFromBufferCopy<int32>(
        { 1, 1, (int64)SpeechIds.Num() },
        SpeechIds);
    if (!CodesTensor.IsValid())
    {
        FailWith(TEXT("Failed to allocate codec input tensor."));
        return;
    }

    FInoOnnxSession* Session = Runner->GetCodecSession();
    if (Session == nullptr)
    {
        FailWith(TEXT("Codec session is null."));
        return;
    }

    TArray<FInoOnnxTensor> DecoderInputs;
    DecoderInputs.Add(MoveTemp(CodesTensor));

    const double DecoderStart = FPlatformTime::Seconds();
    TArray<FInoOnnxTensor> DecoderOutputs;
    FString OrtErr;
    if (!Session->Run(DecoderInputs, DecoderOutputs, &OrtErr))
    {
        FailWithFmt(FString::Printf(
            TEXT("NeuCodec decoder Run failed: %s"), *OrtErr));
        return;
    }
    const double DecoderMs = (FPlatformTime::Seconds() - DecoderStart) * 1000.0;

    if (DecoderOutputs.Num() == 0)
    {
        FailWith(TEXT("NeuCodec decoder returned zero outputs."));
        return;
    }

    // -----------------------------------------------------------------
    // 7. Extract float32 waveform + convert to int16 PCM LE bytes.
    //    NeuCodec decoder output is float32 shape [1, 1, N_samples].
    // -----------------------------------------------------------------
    const FInoOnnxTensor& Wav = DecoderOutputs[0];
    if (Wav.GetDtype() != EInoOnnxDtype::Float32)
    {
        FailWithFmt(FString::Printf(
            TEXT("Expected Float32 decoder output, got dtype=%d."),
            (int32)Wav.GetDtype()));
        return;
    }
    const float* Samples = Wav.GetData<float>();
    const int64  NumSamples = Wav.GetElementCount();
    if (Samples == nullptr || NumSamples <= 0)
    {
        FailWith(TEXT("Decoder output tensor is empty."));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: codec decode done (%lld samples, %.0f ms)"),
           NumSamples, DecoderMs);

    const double PcmStart = FPlatformTime::Seconds();
    TArray<uint8> Pcm = Float32ToInt16PcmLE(Samples, NumSamples);
    const double PcmMs = (FPlatformTime::Seconds() - PcmStart) * 1000.0;

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: PCM convert done (%d bytes int16 LE, %.1f ms)"),
           Pcm.Num(), PcmMs);

    const double TotalMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
    const double AudioSec = (double)NumSamples / (double)kOutputSampleRate;
    const double RtFactor = AudioSec * 1000.0 / FMath::Max(1.0, TotalMs);
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Runner: synth DONE — %lld samples (%.2f s audio @ %d Hz), "
                "decoder %.0f ms, total %.0f ms (%.2fx real-time)"),
           NumSamples, AudioSec, kOutputSampleRate,
           DecoderMs, TotalMs, RtFactor);

    // -----------------------------------------------------------------
    // 8. Dispatch OnComplete on the game thread.
    // -----------------------------------------------------------------
    DispatchCompleteOnGameThread(
        Pending.OnComplete,
        /*bSuccess=*/true,
        MoveTemp(Pcm),
        /*ErrorMessage=*/FString());
}
