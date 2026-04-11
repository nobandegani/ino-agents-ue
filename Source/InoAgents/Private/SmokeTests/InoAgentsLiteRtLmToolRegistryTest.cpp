// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.LiteRtLm.ToolRegistryTest (milestone D.4a)
// ============================================================================
//
// Registry-only smoke test for the tool subsystem surface introduced in
// D.4a. Does NOT touch the model, does NOT create a conversation, does
// NOT require the model file to be present. Exercises:
//
//   1. Find the subsystem on the current game instance (PIE required —
//      ULiteRtLmSubsystem is a UGameInstanceSubsystem).
//   2. Construct a ULiteRtLmAddNumbersTool.
//   3. Call Subsystem->RegisterTool with it; verify registration logs
//      successfully.
//   4. Call Subsystem->FindTool("add_numbers") and verify it returns
//      the same object.
//   5. Call Subsystem->BuildToolsJsonForConversation() and log the
//      output — should be a JSON array containing one function schema
//      with function.name == "add_numbers".
//   6. Call the tool's Execute_Implementation directly with
//      {"a":27,"b":15} and verify the result is "42".
//   7. Call Subsystem->UnregisterTool and verify FindTool now returns
//      nullptr.
//
// This is the cheap "does the registry plumbing work" check that
// precedes D.4b's full agent loop. If anything here is broken, D.4b
// is definitely broken, so running this first saves time.
//
// Invoke (from PIE):
//     InoAgents.LiteRtLm.ToolRegistryTest
// ============================================================================

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmAddNumbersTool.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"
#include "LiteRtLm/LiteRtLmTool.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"

namespace
{
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

static void RunLiteRtLmToolRegistryTest(const TArray<FString>& /*Args*/)
{
    ULiteRtLmSubsystem* const Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: could not find a ULiteRtLmSubsystem — "
                    "start PIE first."));
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("ToolRegistryTest: starting"));

    // ---- 1. Construct the tool ---------------------------------------
    ULiteRtLmAddNumbersTool* const AddTool = NewObject<ULiteRtLmAddNumbersTool>();
    if (AddTool == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: failed to NewObject ULiteRtLmAddNumbersTool"));
        return;
    }

    // The tool is not held by a UPROPERTY anywhere yet — AddToRoot so
    // it survives the rest of the test. RemoveFromRoot at the end.
    AddTool->AddToRoot();

    const FName ToolName = ILiteRtLmTool::Execute_GetToolName(AddTool);
    UE_LOG(LogInoAgents, Log,
           TEXT("ToolRegistryTest: tool reports GetToolName() == \"%s\""),
           *ToolName.ToString());

    if (ToolName != FName(TEXT("add_numbers")))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: expected tool name \"add_numbers\", got \"%s\""),
               *ToolName.ToString());
        AddTool->RemoveFromRoot();
        return;
    }

    // ---- 2. Register it ----------------------------------------------
    TScriptInterface<ILiteRtLmTool> ToolInterface(AddTool);
    Subsys->RegisterTool(ToolInterface);

    // ---- 3. Look it up -----------------------------------------------
    TScriptInterface<ILiteRtLmTool> Found = Subsys->FindTool(TEXT("add_numbers"));
    if (Found.GetObject() != AddTool)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: FindTool(\"add_numbers\") did not return the registered tool"));
        Subsys->UnregisterTool(TEXT("add_numbers"));
        AddTool->RemoveFromRoot();
        return;
    }
    UE_LOG(LogInoAgents, Log,
           TEXT("ToolRegistryTest: FindTool(\"add_numbers\") returned the registered tool"));

    // ---- 4. BuildToolsJsonForConversation ----------------------------
    const FString ToolsJson = Subsys->BuildToolsJsonForConversation();
    UE_LOG(LogInoAgents, Log,
           TEXT("ToolRegistryTest: BuildToolsJsonForConversation() -> %s"),
           *ToolsJson);

    if (ToolsJson.IsEmpty() || !ToolsJson.Contains(TEXT("add_numbers")))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: tools_json does not contain \"add_numbers\""));
        Subsys->UnregisterTool(TEXT("add_numbers"));
        AddTool->RemoveFromRoot();
        return;
    }

    // ---- 5. Execute directly -----------------------------------------
    // Go through the BlueprintNativeEvent wrapper (Execute_Execute) to
    // make sure the interface plumbing — and not just the direct C++
    // _Implementation method — is wired up correctly.
    const FString ExecResult = ILiteRtLmTool::Execute_Execute(AddTool,
        TEXT(R"({"a":27,"b":15})"));
    UE_LOG(LogInoAgents, Log,
           TEXT("ToolRegistryTest: Execute({\"a\":27,\"b\":15}) -> \"%s\""),
           *ExecResult);

    if (ExecResult != TEXT("42"))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: expected Execute result \"42\", got \"%s\""),
               *ExecResult);
        Subsys->UnregisterTool(TEXT("add_numbers"));
        AddTool->RemoveFromRoot();
        return;
    }

    // ---- 6. Unregister ------------------------------------------------
    Subsys->UnregisterTool(TEXT("add_numbers"));

    TScriptInterface<ILiteRtLmTool> AfterUnregister = Subsys->FindTool(TEXT("add_numbers"));
    if (AfterUnregister.GetObject() != nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: FindTool after UnregisterTool returned non-null"));
        AddTool->RemoveFromRoot();
        return;
    }
    UE_LOG(LogInoAgents, Log,
           TEXT("ToolRegistryTest: FindTool after UnregisterTool returned null (good)"));

    // ---- Cleanup -----------------------------------------------------
    AddTool->RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("ToolRegistryTest: PASS"));
}

static FAutoConsoleCommand GLiteRtLmToolRegistryTestCommand(
    TEXT("InoAgents.LiteRtLm.ToolRegistryTest"),
    TEXT("Milestone D.4a smoke test: registers a ULiteRtLmAddNumbersTool "
         "with the subsystem, verifies the registry round-trip "
         "(RegisterTool / FindTool / BuildToolsJsonForConversation / "
         "Execute / UnregisterTool), and logs each step. Does NOT load "
         "the model or create a conversation — use "
         "InoAgents.LiteRtLm.ConversationToolTest for the full agent "
         "loop once D.4b is in."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmToolRegistryTest));
