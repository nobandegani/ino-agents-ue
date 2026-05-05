// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LiteRtLm.ConversationSendTest (milestone D.2)
// ============================================================================
//
// End-to-end smoke test of the UInoLiteRtLmConversation non-streaming send
// path:
//
//   1. Grab the subsystem from the current game instance. If no model is
//      loaded, kick off LoadModelAsync with an inline config and wait for
//      the async delegate. If a model is already loaded (from a previous
//      test run), reuse it and skip the load step.
//   2. Create a conversation via Subsystem::CreateConversation.
//   3. Bind OnComplete + OnError to the observer.
//   4. Call Conversation::SendMessageAsync with a short instruction
//      prompt ("What is 2 plus 2? Answer in one sentence.").
//   5. On OnComplete, log the full assistant text + elapsed time and
//      release the observer so the conversation can be garbage-collected.
//   6. On OnError, log the error and release the observer.
//
// Runs non-blocking: the console command returns immediately, the editor
// stays responsive, and all progress appears asynchronously in the
// Output Log via delegate broadcasts.
//
// Invoke:
//     Ino.LiteRtLm.ConversationSendTest
//     Ino.LiteRtLm.ConversationSendTest Write a haiku about Unreal Engine
// ============================================================================

#include "InoLiteRtLmConversationSendTest.h"

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
// FInoLiteRtLmModelConfig struct is in InoLiteRtLmTypes.h (included via subsystem header)
#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

void UInoLiteRtLmConversationSendTestObserver::HandleModelLoaded(
    bool bSuccess, FString ErrorMessage)
{
    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationSendTest: model load FAILED: %s"),
               *ErrorMessage);
        Finish();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationSendTest: model loaded, creating conversation"));

    if (Subsystem == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationSendTest: subsystem reference is null"));
        Finish();
        return;
    }

    Conversation = Subsystem->CreateConversation();
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationSendTest: CreateConversation returned null"));
        Finish();
        return;
    }

    // Bind the delegates BEFORE sending — otherwise SendMessageAsync
    // might complete (for very short responses) before we've attached
    // the handlers, and we'd miss the broadcast.
    Conversation->OnComplete.AddDynamic(
        this, &UInoLiteRtLmConversationSendTestObserver::HandleConversationComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoLiteRtLmConversationSendTestObserver::HandleConversationError);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationSendTest: sending prompt: \"%s\""),
           *Prompt);

    Conversation->SendMessageAsync(Prompt);
}

void UInoLiteRtLmConversationSendTestObserver::HandleConversationComplete(
    FString FullText)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationSendTest: response received in %.2f s (total test elapsed)"),
           Elapsed);
    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationSendTest: assistant text: \"%s\""),
           *FullText);

    Finish();
}

void UInoLiteRtLmConversationSendTestObserver::HandleConversationError(
    FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("ConversationSendTest: FAILED after %.2f s: %s"),
           Elapsed, *ErrorMessage);

    Finish();
}

void UInoLiteRtLmConversationSendTestObserver::Finish()
{
    // Explicitly shut down the conversation to deterministically
    // release the worker thread and native LiteRT-LM resources. This
    // matters because LiteRT-LM's engine rejects creating a second
    // native conversation while the first is still alive, and we
    // want running two smoke tests back-to-back in the same PIE
    // session to work.
    //
    // Calling Shutdown from inside the OnComplete broadcast (which
    // is how we got here) is safe — Shutdown only resets the worker
    // TUniquePtr, which does not touch the delegate invocation list.
    // CollectGarbage in this same context would be unsafe because
    // parallel GC marks UObject delegates from worker threads while
    // the game thread still holds write access via the in-flight
    // broadcast.
    //
    // We intentionally do NOT call Subsystem->UnloadModel() — leaving
    // the model loaded lets subsequent test runs reuse it (the load
    // is cheap from the OS page cache anyway).
    if (Conversation)
    {
        Conversation->Shutdown();
    }
    Conversation = nullptr;
    Config = FInoLiteRtLmModelConfig();
    Subsystem    = nullptr;

    RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("ConversationSendTest: DONE"));
}

static void RunLiteRtLmConversationSendTest(const TArray<FString>& Args)
{
    UInoLiteRtLmSubsystem* Subsys = InoSmokeTest::FindGameInstanceSubsystem<UInoLiteRtLmSubsystem>();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationSendTest: could not find a UInoLiteRtLmSubsystem. "
                    "Start PIE first — UInoLiteRtLmSubsystem is a UGameInstanceSubsystem "
                    "and only exists while a game instance is active."));
        return;
    }

    // Build the observer first so it is ready to receive callbacks.
    UInoLiteRtLmConversationSendTestObserver* Observer =
        NewObject<UInoLiteRtLmConversationSendTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Subsystem = Subsys;
    Observer->Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("What is 2 plus 2? Answer in one sentence."));
    Observer->AddToRoot();

    if (Subsys->IsModelLoaded())
    {
        // A previous test run (or other game code) already loaded the model.
        // Skip the load step entirely and call HandleModelLoaded directly.
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationSendTest: model already loaded, skipping load step"));
        Observer->HandleModelLoaded(true, FString());
        return;
    }

    // Build an inline config for the load. Same as the D.1 test.
    FInoLiteRtLmModelConfig Config;
    Config.ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config.Backend       = EInoLiteRtLmBackend::Cpu;
    Config.SystemMessage = TEXT("You are a helpful assistant. Answer concisely.");
    Observer->Config = Config;

    FOnInoLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(
        Observer,
        &UInoLiteRtLmConversationSendTestObserver::HandleModelLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationSendTest: starting — loading model first (non-blocking)"));

    Subsys->LoadModelAsync(Config, FInoLiteRtLmDownloadProgressDelegate(), OnLoaded);
}

static FAutoConsoleCommand GLiteRtLmConversationSendTestCommand(
    TEXT("Ino.LiteRtLm.ConversationSendTest"),
    TEXT("Milestone D.2 smoke test: loads the model (if not already loaded), "
         "creates a UInoLiteRtLmConversation via the subsystem factory, binds "
         "OnComplete + OnError delegates, sends one user message via "
         "SendMessageAsync, logs the assistant's full response. Non-blocking "
         "— editor stays responsive. Optional custom prompt: "
         "'Ino.LiteRtLm.ConversationSendTest Write a haiku about UE'."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmConversationSendTest));
