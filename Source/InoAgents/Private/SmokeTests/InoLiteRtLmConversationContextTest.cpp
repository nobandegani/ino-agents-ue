// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LiteRtLm.ConversationContextTest
// ============================================================================
//
// Smoke test for the per-turn system/user context injection API:
//
//   1. Grab the subsystem from the current game instance. If no model is
//      loaded, kick off LoadModelAsync and wait for the async delegate.
//      If already loaded, reuse it.
//   2. Create a conversation via Subsystem::CreateConversation.
//   3. Inject system context:
//        "location"     = "Dragon's Peak Castle"
//        "time_of_day"  = "midnight"
//   4. Inject user context:
//        "player_name"  = "Sir Lancelot"
//        "player_class" = "knight"
//   5. Bind OnComplete + OnError to the observer.
//   6. SendMessageAsync with: "Greet me by name and mention where I am
//      and what time it is. One sentence."
//   7. On OnComplete, check that the response contains at least one of
//      the injected values (case-insensitive). PASS if it does, FAIL
//      if it doesn't (model didn't see the context).
//
// The test proves that BuildMergedContext produces valid JSON, that
// the worker forwards it as extra_context to the C API, and that the
// model actually uses it to shape its response.
//
// Runs non-blocking. Requires PIE.
//
// Invoke:
//     Ino.LiteRtLm.ConversationContextTest
// ============================================================================

#include "InoLiteRtLmConversationContextTest.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

namespace
{
    UInoLiteRtLmSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (UInoLiteRtLmSubsystem* Subsys = GI->GetSubsystem<UInoLiteRtLmSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }
}

void UInoLiteRtLmConversationContextTestObserver::HandleModelLoaded(
    bool bSuccess, FString ErrorMessage)
{
    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationContextTest: model load FAILED: %s"),
               *ErrorMessage);
        Finish();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: model loaded, creating conversation"));

    if (Subsystem == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationContextTest: subsystem reference is null"));
        Finish();
        return;
    }

    Conversation = Subsystem->CreateConversation();
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationContextTest: CreateConversation returned null"));
        Finish();
        return;
    }

    // ---- Inject system context (game/world state) ----
    Conversation->SetSystemContext(TEXT("location"),    TEXT("Dragon's Peak Castle"));
    Conversation->SetSystemContext(TEXT("time_of_day"), TEXT("midnight"));

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: system context set: "
                "location=\"Dragon's Peak Castle\", time_of_day=\"midnight\""));

    // ---- Inject user context (player state) ----
    Conversation->SetUserContext(TEXT("player_name"),  TEXT("Sir Lancelot"));
    Conversation->SetUserContext(TEXT("player_class"), TEXT("knight"));

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: user context set: "
                "player_name=\"Sir Lancelot\", player_class=\"knight\""));

    // Bind delegates BEFORE sending.
    Conversation->OnComplete.AddDynamic(
        this, &UInoLiteRtLmConversationContextTestObserver::HandleConversationComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoLiteRtLmConversationContextTestObserver::HandleConversationError);

    const FString Prompt =
        TEXT("Greet me by name and mention where I am and what time it is. "
             "One sentence.");

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: sending prompt: \"%s\""),
           *Prompt);

    Conversation->SendMessageAsync(Prompt);
}

void UInoLiteRtLmConversationContextTestObserver::HandleConversationComplete(
    FString FullText)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: response received in %.2f s"),
           Elapsed);
    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: assistant text: \"%s\""),
           *FullText);

    // Check that the response references at least some of the injected
    // context. We do case-insensitive checks because the model may
    // rephrase or change casing.
    const FString Lower = FullText.ToLower();

    const bool bHasName     = Lower.Contains(TEXT("lancelot"));
    const bool bHasLocation = Lower.Contains(TEXT("dragon"))
                           || Lower.Contains(TEXT("castle"));
    const bool bHasTime     = Lower.Contains(TEXT("midnight"));
    const bool bHasClass    = Lower.Contains(TEXT("knight"));

    int32 ContextHits = 0;
    if (bHasName)     { ContextHits++; }
    if (bHasLocation) { ContextHits++; }
    if (bHasTime)     { ContextHits++; }
    if (bHasClass)    { ContextHits++; }

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: context hits: %d/4 "
                "(name=%s, location=%s, time=%s, class=%s)"),
           ContextHits,
           bHasName     ? TEXT("YES") : TEXT("no"),
           bHasLocation ? TEXT("YES") : TEXT("no"),
           bHasTime     ? TEXT("YES") : TEXT("no"),
           bHasClass    ? TEXT("YES") : TEXT("no"));

    // The prompt explicitly asks for name + location + time. We require
    // at least 2 of 4 as a PASS — the model might not mention "knight"
    // since the prompt didn't ask for class, but it should reference
    // name and at least one of location/time.
    if (ContextHits >= 2)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationContextTest: PASS — model used injected context"));
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationContextTest: FAIL — model did not reference "
                    "enough injected context values (%d/4). The extra_context "
                    "may not be reaching the model."),
               ContextHits);
    }

    Finish();
}

void UInoLiteRtLmConversationContextTestObserver::HandleConversationError(
    FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("ConversationContextTest: FAILED after %.2f s: %s"),
           Elapsed, *ErrorMessage);

    Finish();
}

void UInoLiteRtLmConversationContextTestObserver::Finish()
{
    if (Conversation)
    {
        Conversation->Shutdown();
    }
    Conversation = nullptr;
    Config = FInoLiteRtLmModelConfig();
    Subsystem    = nullptr;

    RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("ConversationContextTest: DONE"));
}

static void RunLiteRtLmConversationContextTest(const TArray<FString>& Args)
{
    UInoLiteRtLmSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationContextTest: could not find a UInoLiteRtLmSubsystem. "
                    "Start PIE first — UInoLiteRtLmSubsystem is a UGameInstanceSubsystem "
                    "and only exists while a game instance is active."));
        return;
    }

    UInoLiteRtLmConversationContextTestObserver* Observer =
        NewObject<UInoLiteRtLmConversationContextTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Subsystem = Subsys;
    Observer->AddToRoot();

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationContextTest: model already loaded, skipping load step"));
        Observer->HandleModelLoaded(true, FString());
        return;
    }

    FInoLiteRtLmModelConfig Config;
    Config.ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config.Backend       = EInoLiteRtLmBackend::Cpu;
    Config.SystemMessage = TEXT("You are an NPC in a fantasy RPG. "
                                "Use the provided context to stay in character.");
    Observer->Config = Config;

    FOnInoLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(
        Observer,
        &UInoLiteRtLmConversationContextTestObserver::HandleModelLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationContextTest: starting — loading model first (non-blocking)"));

    Subsys->LoadModelAsync(Config, OnLoaded);
}

static FAutoConsoleCommand GLiteRtLmConversationContextTestCommand(
    TEXT("Ino.LiteRtLm.ConversationContextTest"),
    TEXT("Smoke test for system/user context injection. Sets system context "
         "(location, time_of_day) and user context (player_name, player_class) "
         "on a conversation, sends a prompt that requires the context to answer, "
         "and checks that the model's response references the injected values. "
         "Validates the BuildMergedContext -> extra_context -> model pipeline. "
         "Non-blocking. Requires PIE."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmConversationContextTest));
