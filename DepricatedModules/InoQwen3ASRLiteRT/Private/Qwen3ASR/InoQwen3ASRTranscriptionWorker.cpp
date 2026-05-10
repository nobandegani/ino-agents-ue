// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRTranscriptionWorker.h"

#include "InoQwen3ASRConstants.h"
#include "InoQwen3ASRLiteRT.h"
#include "InoQwen3ASRRunner.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"

namespace
{
    /**
     * Parse the auto-detected language name out of the first few generated
     * tokens — they come back as the BPE pieces of " English" / " Chinese"
     * / etc., concatenated. Stops at the first special-token boundary.
     */
    static FString ExtractDetectedLanguage(
        TArrayView<const int32> Generated,
        const FInoQwen3ASRRunner& Runner)
    {
        if (Generated.Num() < 2) { return FString(); }
        if (Generated[0] != 11528 /* "language" */) { return FString(); }
        TArray<int32> LangSlice;
        for (int32 i = 1; i < Generated.Num(); ++i)
        {
            if (Generated[i] >= InoQwen3ASR::kPadTokenId) { break; }
            LangSlice.Add(Generated[i]);
        }
        if (LangSlice.Num() == 0) { return FString(); }
        FString Raw = Runner.GetTokenizer().Decode(LangSlice);
        return Raw.TrimStartAndEnd();
    }

    /**
     * Marshal an OnComplete invocation to the game thread, gated on the
     * caller-provided Owner UObject still being alive. If Owner has been
     * GC'd in the meantime we silently skip — same pattern as InoChatterboxNative.
     */
    static void DispatchCompleteOnGameThread(
        TWeakObjectPtr<UObject> Owner,
        FOnInoQwen3ASRTranscribeComplete Delegate,
        bool bSuccess,
        FInoQwen3ASRTranscribeResult Result,
        FString ErrorMessage)
    {
        AsyncTask(ENamedThreads::GameThread,
            [Owner, Delegate, bSuccess, Result = MoveTemp(Result), ErrorMessage = MoveTemp(ErrorMessage)]() mutable
            {
                if (!Owner.IsValid()) { return; }
                Delegate.ExecuteIfBound(bSuccess, Result, ErrorMessage);
            });
    }
}

FInoQwen3ASRTranscriptionWorker::FInoQwen3ASRTranscriptionWorker(FInoQwen3ASRRunner* InRunner)
    : Runner(InRunner)
{
    check(Runner);
    WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);
    Thread = FRunnableThread::Create(this, TEXT("InoQwen3ASRTranscription"),
                                      0, TPri_Normal);
}

FInoQwen3ASRTranscriptionWorker::~FInoQwen3ASRTranscriptionWorker()
{
    if (Thread)
    {
        Stop();
        // Stop() sets bShouldExit + signals; Kill(true) waits for the thread to exit.
        Thread->Kill(/*bShouldWait=*/true);
        delete Thread;
        Thread = nullptr;
    }
    if (WakeEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
        WakeEvent = nullptr;
    }
}

void FInoQwen3ASRTranscriptionWorker::EnqueueRequest(
    TArray<float>&& AudioSamples,
    TWeakObjectPtr<UObject> Owner,
    FOnInoQwen3ASRTranscribeComplete OnComplete)
{
    TUniquePtr<FRequest> Req = MakeUnique<FRequest>();
    Req->AudioSamples = MoveTemp(AudioSamples);
    Req->Owner = Owner;
    Req->OnComplete = OnComplete;

    Queue.Enqueue(MoveTemp(Req));
    QueueDepth.fetch_add(1);
    if (WakeEvent) { WakeEvent->Trigger(); }
}

void FInoQwen3ASRTranscriptionWorker::CancelPending()
{
    DrainQueueAsCancelled();
}

int32 FInoQwen3ASRTranscriptionWorker::GetQueueDepthApprox() const
{
    return QueueDepth.load();
}

void FInoQwen3ASRTranscriptionWorker::Stop()
{
    bShouldExit.store(true);
    if (WakeEvent) { WakeEvent->Trigger(); }
}

uint32 FInoQwen3ASRTranscriptionWorker::Run()
{
    while (!bShouldExit.load())
    {
        // Block until either a request arrives or Stop() is called.
        if (WakeEvent) { WakeEvent->Wait(); }

        // Drain everything queued so far. New requests enqueued mid-loop
        // will trigger WakeEvent again and we'll catch them on the next
        // outer iteration.
        TUniquePtr<FRequest> Req;
        while (!bShouldExit.load() && Queue.Dequeue(Req))
        {
            QueueDepth.fetch_sub(1);
            if (Req)
            {
                ProcessRequest(*Req);
                Req.Reset();
            }
        }
    }

    // Worker is exiting (subsystem deinitialize). Cancel anything still pending.
    DrainQueueAsCancelled();
    return 0;
}

void FInoQwen3ASRTranscriptionWorker::ProcessRequest(FRequest& Req)
{
    if (!Runner || !Runner->IsReady())
    {
        DispatchCompleteOnGameThread(Req.Owner, Req.OnComplete, false,
            FInoQwen3ASRTranscribeResult{},
            TEXT("Runner not ready (model or tokenizer not loaded)."));
        return;
    }

    FString Text;
    TArray<int32> RawIds;
    FInoQwen3ASRTranscribeStats Stats;
    const bool bOk = Runner->Transcribe(Req.AudioSamples, Text, &RawIds, &Stats);

    FInoQwen3ASRTranscribeResult Result;
    if (bOk)
    {
        Result.Text = MoveTemp(Text);
        Result.DetectedLanguage = ExtractDetectedLanguage(RawIds, *Runner);
        Result.NumGeneratedTokens = Stats.NumGeneratedTokens;
        Result.MelSeconds    = static_cast<float>(Stats.MelSeconds);
        Result.EncodeSeconds = static_cast<float>(Stats.EncodeSeconds);
        Result.DecodeSeconds = static_cast<float>(Stats.DecodeSeconds);
        Result.TotalSeconds  = static_cast<float>(Stats.TotalSeconds);
        Result.RawTokenIds   = MoveTemp(RawIds);
    }

    DispatchCompleteOnGameThread(
        Req.Owner, Req.OnComplete, bOk, MoveTemp(Result),
        bOk ? FString() : FString(TEXT("Transcribe() failed; check log for details.")));
}

void FInoQwen3ASRTranscriptionWorker::DrainQueueAsCancelled()
{
    TUniquePtr<FRequest> Req;
    while (Queue.Dequeue(Req))
    {
        QueueDepth.fetch_sub(1);
        if (Req)
        {
            DispatchCompleteOnGameThread(
                Req->Owner, Req->OnComplete, false,
                FInoQwen3ASRTranscribeResult{},
                TEXT("Cancelled"));
        }
    }
}
