// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.LiteRtLm.ToolRegistryTest
// ============================================================================
//
// Registry-only smoke test for the tool subsystem. Does NOT touch the
// model, does NOT create a conversation, does NOT require the model
// file to be present. Exercises:
//
//   1. Find the subsystem on the current game instance (PIE required).
//   2. Construct a ULiteRtLmAddNumbersTool.
//   3. Register it, find it, build tools JSON, execute it, unregister it.
//
// Invoke (from PIE):
//     InoAgents.LiteRtLm.ToolRegistryTest
// ============================================================================

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmAddNumbersTool.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"

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

    AddTool->AddToRoot();

    UE_LOG(LogInoAgents, Log,
           TEXT("ToolRegistryTest: tool ToolName == \"%s\""),
           *AddTool->ToolName.ToString());

    if (AddTool->ToolName != FName(TEXT("add_numbers")))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolRegistryTest: expected ToolName \"add_numbers\", got \"%s\""),
               *AddTool->ToolName.ToString());
        AddTool->RemoveFromRoot();
        return;
    }

    // ---- 2. Register it ----------------------------------------------
    Subsys->RegisterTool(AddTool);

    // ---- 3. Look it up -----------------------------------------------
    ULiteRtLmToolBase* Found = Subsys->FindTool(TEXT("add_numbers"));
    if (Found != AddTool)
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
    const FString ExecResult = AddTool->Execute(TEXT(R"({"a":27,"b":15})"));
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

    ULiteRtLmToolBase* AfterUnregister = Subsys->FindTool(TEXT("add_numbers"));
    if (AfterUnregister != nullptr)
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
    TEXT("Smoke test: registers a ULiteRtLmAddNumbersTool with the "
         "subsystem, verifies RegisterTool / FindTool / "
         "BuildToolsJsonForConversation / Execute / UnregisterTool. "
         "Does NOT load the model. Requires PIE."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmToolRegistryTest));
