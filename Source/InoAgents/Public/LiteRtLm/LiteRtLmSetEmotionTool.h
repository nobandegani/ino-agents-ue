// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "LiteRtLm/LiteRtLmToolBase.h"

#include "LiteRtLmSetEmotionTool.generated.h"

class UInoAgentsLiteRtLmAgentComponent;

/**
 * Built-in tool that lets the model set the agent's emotional state.
 * Auto-registered by UInoAgentsLiteRtLmAgentComponent — users do not
 * need to create or register this tool manually.
 *
 * The model calls set_emotion(emotion: "happy") and the agent
 * component's OnEmotionChanged delegate fires, allowing Blueprint
 * to drive facial animation, particle effects, etc.
 */
UCLASS()
class INOAGENTS_API ULiteRtLmSetEmotionTool : public ULiteRtLmToolBase
{
    GENERATED_BODY()

public:
    ULiteRtLmSetEmotionTool();

    /** Set the agent component this tool controls. Must be called
     *  after construction, before the tool is used. */
    void SetAgent(UInoAgentsLiteRtLmAgentComponent* InAgent);

    virtual FString Execute_Implementation(const FJsonObjectWrapper& Arguments) override;

private:
    TWeakObjectPtr<UInoAgentsLiteRtLmAgentComponent> AgentWeak;
};
