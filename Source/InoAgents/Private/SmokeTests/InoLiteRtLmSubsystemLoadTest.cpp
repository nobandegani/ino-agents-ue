// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LiteRtLm.SubsystemLoadTest (milestone D.1)
// ============================================================================
//
// Exercises the UInoLiteRtLmSubsystem's async model load path end-to-end:
//
//   1. Grab the subsystem from the current game instance.
//   2. Construct a ULiteRtLmModelConfig in C++ (not from a Content Browser
//      asset) pointing at the default Gemma 4 E2B model.
//   3. Call LoadModelAsync with a dynamic-delegate callback that logs
//      success/failure + wall-clock duration.
//   4. Verify IsModelLoaded() returns true in the success callback.
//   5. Call UnloadModel() immediately after verification.
//
// Runs non-blocking: the console command returns immediately and the
// editor stays responsive while the engine loads on a thread-pool worker.
// Log lines appear asynchronously.
//
// Invoke:
//     Ino.LiteRtLm.SubsystemLoadTest
// ============================================================================

#include "InoLiteRtLmSubsystemLoadTest.h"

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"
// FInoLiteRtLmModelConfig struct is in InoLiteRtLmTypes.h (included via subsystem header)
#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

void UInoLiteRtLmSubsystemLoadTestObserver::HandleLoaded(
    bool bSuccess, FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;

    if (bSuccess)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemLoadTest: SUCCESS in %.2f s"), Elapsed);

        const bool bLoaded = Subsystem != nullptr && Subsystem->IsModelLoaded();
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemLoadTest: IsModelLoaded() returned %s after the delegate fired"),
               bLoaded ? TEXT("true") : TEXT("false"));

        if (Subsystem != nullptr)
        {
            Subsystem->UnloadModel();
            UE_LOG(LogInoAgents, Log,
                   TEXT("SubsystemLoadTest: unloaded successfully; IsModelLoaded() now returns %s"),
                   Subsystem->IsModelLoaded() ? TEXT("true") : TEXT("false"));
        }

        UE_LOG(LogInoAgents, Log, TEXT("SubsystemLoadTest: DONE"));
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemLoadTest: FAILED after %.2f s: %s"),
               Elapsed, *ErrorMessage);
    }

    // Release our GC root anchor. After this, we (and our UPROPERTY-held
    // Subsystem + Config references) become eligible for collection.
    RemoveFromRoot();
}

static void RunLiteRtLmSubsystemLoadTest(const TArray<FString>& /*Args*/)
{
    UInoLiteRtLmSubsystem* Subsys = InoSmokeTest::FindLiteRtLmSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemLoadTest: could not find a UInoLiteRtLmSubsystem. "
                    "This usually means there is no active game instance — "
                    "try running the test after entering PIE, or check that "
                    "the plugin module is loaded."));
        return;
    }

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("SubsystemLoadTest: a model is already loaded. Unloading first "
                    "so the test can run from a clean state."));
        Subsys->UnloadModel();
    }

    // Build the config inline. Using NewObject instead of a Content Browser
    // asset makes the test entirely self-contained — there is no asset to
    // create before running.
    FInoLiteRtLmModelConfig Config;
    Config.ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config.Backend       = EInoLiteRtLmBackend::Cpu;
    Config.SystemMessage = TEXT("You are a helpful assistant.");

    // Observer holds the async continuation. Must outlive the delegate
    // callback, so root it.
    auto* Observer = NewObject<UInoLiteRtLmSubsystemLoadTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Subsystem = Subsys;
    Observer->Config    = Config;
    Observer->AddToRoot();

    FOnInoLiteRtLmModelLoaded Delegate;
    Delegate.BindDynamic(Observer, &UInoLiteRtLmSubsystemLoadTestObserver::HandleLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemLoadTest: starting — kicking off LoadModelAsync (non-blocking)"));

    Subsys->LoadModelAsync(Config, Delegate);

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemLoadTest: LoadModelAsync returned synchronously. Editor stays responsive."));
}

static FAutoConsoleCommand GLiteRtLmSubsystemLoadTestCommand(
    TEXT("Ino.LiteRtLm.SubsystemLoadTest"),
    TEXT("Milestone D.1 smoke test: kicks off UInoLiteRtLmSubsystem::LoadModelAsync "
         "with an inline ULiteRtLmModelConfig, logs the result from the dynamic "
         "delegate callback on the game thread, unloads the model, exits. "
         "Non-blocking — the editor stays responsive during the ~0.5-2.5 s load."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmSubsystemLoadTest));
