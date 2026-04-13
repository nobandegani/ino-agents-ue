// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "LiteRtLm/InoLiteRtLmToolBase.h"

#include "InoLiteRtLmAddNumbersTool.generated.h"

/**
 * Reference implementation of UInoLiteRtLmToolBase: adds two integers.
 *
 * Parameters:
 *   a (integer) — first addend
 *   b (integer) — second addend
 *
 * Returns a bare JSON number (e.g. "42") so the model sees the sum
 * as a scalar value in the tool_response.value field.
 *
 * This tool exists for two purposes:
 *   1. As the smoke-test fixture for the
 *      Ino.LiteRtLm.ConversationToolTest console command.
 *   2. As a compile-checked example for plugin consumers — both C++
 *      and Blueprint tool authors can reference this as a template.
 *
 * The tool is NOT auto-registered with the subsystem. Users who want
 * it must construct and register it explicitly.
 */
UCLASS(BlueprintType)
class INOAGENTS_API UInoLiteRtLmAddNumbersTool : public UInoLiteRtLmToolBase
{
    GENERATED_BODY()

public:
    UInoLiteRtLmAddNumbersTool();

    virtual FString Execute_Implementation(const FJsonObjectWrapper& Arguments) override;
};
