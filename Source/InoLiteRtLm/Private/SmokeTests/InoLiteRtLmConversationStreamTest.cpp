// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LiteRtLm.ConversationStreamTest (milestone D.3)
// ============================================================================
//
// End-to-end smoke test of the UInoLiteRtLmConversation streaming path:
//
//   1. Grab the subsystem from the current game instance. If no model
//      is loaded, kick off LoadModelAsync with an inline config and
//      wait for the async delegate. If already loaded, reuse it.
//   2. Create a conversation via Subsystem::CreateConversation.
//   3. Bind OnToken + OnComplete + OnError to the observer.
//   4. Call Conversation::SendMessageAsync with a prompt that tends
//      to produce a multi-chunk response ("Write a short haiku...").
//   5. For each chunk, log the chunk index, elapsed time since stream
//      start, and the chunk text.
//   6. On OnComplete, log the total chunk count, elapsed wall time,
//      effective chunks/sec, and the full accumulated text. Verify
//      that the locally-accumulated text matches the FullText
//      delivered to OnComplete (if it doesn't, that's a worker bug).
//   7. On OnError, log the error.
//
// Runs non-blocking: the console command returns immediately, the
// editor stays responsive, tokens trickle into the Output Log as the
// model generates them, and the final broadcast releases the observer.
//
// Invoke:
//     Ino.LiteRtLm.ConversationStreamTest
//     Ino.LiteRtLm.ConversationStreamTest Write a haiku about Unreal Engine
// ============================================================================

#include "InoLiteRtLmConversationStreamTest.h"

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
// FInoLiteRtLmModelConfig struct is in InoLiteRtLmTypes.h (included via subsystem header)
#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

void UInoLiteRtLmConversationStreamTestObserver::HandleModelLoaded(
    bool bSuccess, FString ErrorMessage)
{
    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationStreamTest: model load FAILED: %s"),
               *ErrorMessage);
        Finish();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: model loaded, creating conversation"));

    if (Subsystem == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationStreamTest: subsystem reference is null"));
        Finish();
        return;
    }

    Conversation = Subsystem->CreateConversation();
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationStreamTest: CreateConversation returned null"));
        Finish();
        return;
    }

    // Bind the delegates BEFORE sending. OnToken in particular is the
    // key difference from the D.2 test — that test only bound
    // OnComplete/OnError.
    Conversation->OnToken.AddDynamic(
        this, &UInoLiteRtLmConversationStreamTestObserver::HandleToken);
    Conversation->OnComplete.AddDynamic(
        this, &UInoLiteRtLmConversationStreamTestObserver::HandleConversationComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoLiteRtLmConversationStreamTestObserver::HandleConversationError);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: sending prompt: \"%s\""),
           *Prompt);

    // Reset stream-start timestamp as close as possible to the actual
    // SendMessageAsync call — there may be a few milliseconds between
    // "test started" (StartTime) and "stream started" (FirstTokenTime
    // is the real per-chunk baseline, populated inside HandleToken).
    Conversation->SendMessageAsync(Prompt);
}

void UInoLiteRtLmConversationStreamTestObserver::HandleToken(FString RawText, FString CleanText)
{
    const double Now = FPlatformTime::Seconds();

    if (NumTokens == 0)
    {
        FirstTokenTime = Now;
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationStreamTest: first token at +%.2f s from send"),
               Now - StartTime);
    }

    NumTokens += 1;
    AccumulatedText += RawText;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: token %3d (+%.3f s) \"%s\""),
           NumTokens, Now - FirstTokenTime, *RawText);
}

void UInoLiteRtLmConversationStreamTestObserver::HandleConversationComplete(
    FString FullText)
{
    const double Now       = FPlatformTime::Seconds();
    const double TotalSec  = Now - StartTime;
    const double StreamSec = (FirstTokenTime > 0.0) ? Now - FirstTokenTime : 0.0;
    const double ChunksPerSec = (StreamSec > 0.0 && NumTokens > 0)
        ? (static_cast<double>(NumTokens) / StreamSec)
        : 0.0;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: COMPLETE — %d chunks, stream=%.2f s, "
                "total=%.2f s, ~%.1f chunks/sec"),
           NumTokens, StreamSec, TotalSec, ChunksPerSec);

    // Cross-check: the FullText delivered to OnComplete should match
    // our locally-accumulated string. If it doesn't, that means the
    // worker is accumulating differently than the sum of what it
    // dispatches, which would be a bug in the worker's happens-before
    // edge between OnStreamChunk writes and ProcessMessage reads.
    if (FullText != AccumulatedText)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationStreamTest: MISMATCH — FullText from OnComplete "
                    "does not match locally accumulated tokens!"));
        UE_LOG(LogInoAgents, Error,
               TEXT("  OnComplete FullText:  \"%s\""), *FullText);
        UE_LOG(LogInoAgents, Error,
               TEXT("  Local Accumulated:    \"%s\""), *AccumulatedText);
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationStreamTest: full text (matches local accumulation): \"%s\""),
               *FullText);
    }

    Finish();
}

void UInoLiteRtLmConversationStreamTestObserver::HandleConversationError(
    FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("ConversationStreamTest: FAILED after %.2f s (%d chunks received): %s"),
           Elapsed, NumTokens, *ErrorMessage);

    Finish();
}

void UInoLiteRtLmConversationStreamTestObserver::Finish()
{
    // Explicitly shut down the conversation to deterministically
    // release the worker thread and native LiteRT-LM resources.
    // Same rationale as the D.2 test's Finish: LiteRT-LM's engine
    // rejects creating a second native conversation while the first
    // is still alive, and we want back-to-back smoke tests in the
    // same PIE session to work. Shutdown is safe to call from inside
    // the OnComplete broadcast because it only resets the Worker
    // TUniquePtr — it does not touch the delegate invocation list
    // that the broadcast is still walking, and it does not invoke
    // parallel GC (unlike CollectGarbage, which races with the
    // still-in-flight delegate write access).
    if (Conversation)
    {
        Conversation->Shutdown();
    }
    Conversation = nullptr;
    Config = FInoLiteRtLmModelConfig();
    Subsystem    = nullptr;

    RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("ConversationStreamTest: DONE"));
}

static void RunLiteRtLmConversationStreamTest(const TArray<FString>& Args)
{
    UInoLiteRtLmSubsystem* Subsys = InoSmokeTest::FindGameInstanceSubsystem<UInoLiteRtLmSubsystem>();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationStreamTest: could not find a UInoLiteRtLmSubsystem. "
                    "Start PIE first — UInoLiteRtLmSubsystem is a UGameInstanceSubsystem "
                    "and only exists while a game instance is active."));
        return;
    }

    UInoLiteRtLmConversationStreamTestObserver* Observer =
        NewObject<UInoLiteRtLmConversationStreamTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Subsystem = Subsys;
    Observer->Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("Write a short haiku about a cat in a castle."));
    Observer->AddToRoot();

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationStreamTest: model already loaded, skipping load step"));
        Observer->HandleModelLoaded(true, FString());
        return;
    }

    // Build an inline config for the load. Same as the D.1 / D.2 tests.
    FInoLiteRtLmModelConfig Config;
    Config.ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config.Backend       = EInoLiteRtLmBackend::Cpu;
    Config.SystemMessage = TEXT("You are a helpful assistant. Answer concisely.");
    Observer->Config = Config;

    FOnInoLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(
        Observer,
        &UInoLiteRtLmConversationStreamTestObserver::HandleModelLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: starting — loading model first (non-blocking)"));

    Subsys->LoadModelAsync(Config, FOnInoModelDownloadProgress(), OnLoaded);
}

static FAutoConsoleCommand GLiteRtLmConversationStreamTestCommand(
    TEXT("Ino.LiteRtLm.ConversationStreamTest"),
    TEXT("Milestone D.3 smoke test: loads the model (if not already loaded), "
         "creates a UInoLiteRtLmConversation, binds OnToken + OnComplete + "
         "OnError delegates, sends one user message via SendMessageAsync "
         "(which now uses litert_lm_conversation_send_message_stream under "
         "the hood), logs each chunk as it arrives, then the full assistant "
         "text on completion. Non-blocking — editor stays responsive. "
         "Optional custom prompt: 'Ino.LiteRtLm.ConversationStreamTest "
         "Write a haiku about UE'."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmConversationStreamTest));
