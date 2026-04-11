// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.LiteRtLm.ConversationStreamTest (milestone D.3)
// ============================================================================
//
// End-to-end smoke test of the ULiteRtLmConversation streaming path:
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
//     InoAgents.LiteRtLm.ConversationStreamTest
//     InoAgents.LiteRtLm.ConversationStreamTest Write a haiku about Unreal Engine
// ============================================================================

#include "InoAgentsLiteRtLmConversationStreamTest.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "LiteRtLm/LiteRtLmModelConfig.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/GarbageCollection.h"

namespace
{
    /**
     * Same helper as the D.2 test — find a ULiteRtLmSubsystem on any
     * active game instance. Returns nullptr if none exists (e.g. the
     * user ran the command without being in PIE).
     */
    ULiteRtLmSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (ULiteRtLmSubsystem* Subsys = GI->GetSubsystem<ULiteRtLmSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }
}

void UInoAgentsLiteRtLmConversationStreamTestObserver::HandleModelLoaded(
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
        this, &UInoAgentsLiteRtLmConversationStreamTestObserver::HandleToken);
    Conversation->OnComplete.AddDynamic(
        this, &UInoAgentsLiteRtLmConversationStreamTestObserver::HandleConversationComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoAgentsLiteRtLmConversationStreamTestObserver::HandleConversationError);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: sending prompt: \"%s\""),
           *Prompt);

    // Reset stream-start timestamp as close as possible to the actual
    // SendMessageAsync call — there may be a few milliseconds between
    // "test started" (StartTime) and "stream started" (FirstTokenTime
    // is the real per-chunk baseline, populated inside HandleToken).
    Conversation->SendMessageAsync(Prompt);
}

void UInoAgentsLiteRtLmConversationStreamTestObserver::HandleToken(FString Chunk)
{
    const double Now = FPlatformTime::Seconds();

    // Capture the wall-clock time of the first token, so per-chunk
    // elapsed times are relative to when the model actually started
    // emitting (not when LoadModelAsync was called or when the user
    // ran the console command).
    if (NumTokens == 0)
    {
        FirstTokenTime = Now;
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationStreamTest: first token at +%.2f s from send"),
               Now - StartTime);
    }

    NumTokens += 1;
    AccumulatedText += Chunk;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: token %3d (+%.3f s) \"%s\""),
           NumTokens, Now - FirstTokenTime, *Chunk);
}

void UInoAgentsLiteRtLmConversationStreamTestObserver::HandleConversationComplete(
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

void UInoAgentsLiteRtLmConversationStreamTestObserver::HandleConversationError(
    FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("ConversationStreamTest: FAILED after %.2f s (%d chunks received): %s"),
           Elapsed, NumTokens, *ErrorMessage);

    Finish();
}

void UInoAgentsLiteRtLmConversationStreamTestObserver::Finish()
{
    // Same cleanup rationale as the D.2 observer: release references
    // and let GC destroy the conversation. Do NOT UnloadModel here —
    // that would race with in-flight workers and break subsequent
    // test runs. Leaving the model loaded is the right default.
    Conversation = nullptr;
    Config       = nullptr;
    Subsystem    = nullptr;

    RemoveFromRoot();

    // Force an immediate blocking GC. Same rationale as the D.2
    // test's Finish(): LiteRT-LM's engine rejects creating a second
    // conversation while a prior native LiteRtLmConversation is
    // still alive. Running two smoke tests back-to-back in the same
    // PIE session would otherwise fail because the first test's
    // ULiteRtLmConversation UObject waits for the next natural GC
    // cycle before its worker tears down the native conversation.
    // Test-only hygiene; production code does not need this.
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/ true);

    UE_LOG(LogInoAgents, Log, TEXT("ConversationStreamTest: DONE"));
}

static void RunLiteRtLmConversationStreamTest(const TArray<FString>& Args)
{
    ULiteRtLmSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationStreamTest: could not find a ULiteRtLmSubsystem. "
                    "Start PIE first — ULiteRtLmSubsystem is a UGameInstanceSubsystem "
                    "and only exists while a game instance is active."));
        return;
    }

    UInoAgentsLiteRtLmConversationStreamTestObserver* Observer =
        NewObject<UInoAgentsLiteRtLmConversationStreamTestObserver>();
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
    ULiteRtLmModelConfig* Config = NewObject<ULiteRtLmModelConfig>();
    Config->ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config->Backend       = ELiteRtLmBackend::Cpu;
    Config->SystemMessage = TEXT("You are a helpful assistant. Answer concisely.");
    Observer->Config = Config;

    FOnLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(
        Observer,
        &UInoAgentsLiteRtLmConversationStreamTestObserver::HandleModelLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationStreamTest: starting — loading model first (non-blocking)"));

    Subsys->LoadModelAsync(Config, OnLoaded);
}

static FAutoConsoleCommand GLiteRtLmConversationStreamTestCommand(
    TEXT("InoAgents.LiteRtLm.ConversationStreamTest"),
    TEXT("Milestone D.3 smoke test: loads the model (if not already loaded), "
         "creates a ULiteRtLmConversation, binds OnToken + OnComplete + "
         "OnError delegates, sends one user message via SendMessageAsync "
         "(which now uses litert_lm_conversation_send_message_stream under "
         "the hood), logs each chunk as it arrives, then the full assistant "
         "text on completion. Non-blocking — editor stays responsive. "
         "Optional custom prompt: 'InoAgents.LiteRtLm.ConversationStreamTest "
         "Write a haiku about UE'."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmConversationStreamTest));
