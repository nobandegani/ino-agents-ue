// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTool.h"

#include "LiteRtLmAddNumbersTool.generated.h"

/**
 * Reference implementation of ILiteRtLmTool: adds two integers.
 *
 * Parameters:
 *   a (integer) — first addend
 *   b (integer) — second addend
 *
 * Returns a bare JSON number (e.g. "42") so the model sees the sum
 * as a scalar value in the tool_response.value field.
 *
 * This tool exists for two purposes:
 *   1. As the smoke-test fixture for Milestone D.4's
 *      InoAgents.LiteRtLm.ConversationToolTest console command. The
 *      Phase 1 ToolCallTest chose add_numbers for the same reason: a
 *      pure math function has no RLHF baggage so the model reliably
 *      uses the tool result in its final answer. See the comments in
 *      Source/InoAgents/Private/SmokeTests/InoAgentsToolCallTest.cpp
 *      for the full rationale.
 *   2. As a compile-checked example for plugin consumers — both C++
 *      and Blueprint tool authors can cargo-cult this file as a
 *      starting template.
 *
 * The tool is NOT auto-registered with the subsystem. Users who want
 * it must either:
 *   - Construct and register it explicitly (see the D.4 smoke test),
 *     or
 *   - Subclass it in Blueprint to change the schema/description
 *     without touching C++.
 */
UCLASS(BlueprintType)
class INOAGENTS_API ULiteRtLmAddNumbersTool : public UObject, public ILiteRtLmTool
{
    GENERATED_BODY()

public:
    //~ ILiteRtLmTool interface
    virtual FName   GetToolName_Implementation() const override;
    virtual FString GetToolSchemaJson_Implementation() const override;
    virtual FString Execute_Implementation(const FString& ArgumentsJson) override;
    //~ End ILiteRtLmTool interface
};
