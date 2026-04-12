// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmSetEmotionTool.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmAgentComponent.h"

#include "Dom/JsonObject.h"

namespace
{
    /** Map a lowercase emotion string to the enum value. Returns true
     *  if the string matched, false otherwise. */
    bool ParseEmotion(const FString& InStr, EInoAgentsEmotion& OutEmotion)
    {
        const FString Lower = InStr.ToLower().TrimStartAndEnd();

        if (Lower == TEXT("neutral"))   { OutEmotion = EInoAgentsEmotion::Neutral;   return true; }
        if (Lower == TEXT("happy"))     { OutEmotion = EInoAgentsEmotion::Happy;     return true; }
        if (Lower == TEXT("sad"))       { OutEmotion = EInoAgentsEmotion::Sad;       return true; }
        if (Lower == TEXT("disgust"))   { OutEmotion = EInoAgentsEmotion::Disgust;   return true; }
        if (Lower == TEXT("anger"))     { OutEmotion = EInoAgentsEmotion::Anger;     return true; }
        if (Lower == TEXT("surprise"))  { OutEmotion = EInoAgentsEmotion::Surprise;  return true; }
        if (Lower == TEXT("fear"))      { OutEmotion = EInoAgentsEmotion::Fear;      return true; }
        if (Lower == TEXT("confident")) { OutEmotion = EInoAgentsEmotion::Confident; return true; }
        if (Lower == TEXT("excited"))   { OutEmotion = EInoAgentsEmotion::Excited;   return true; }
        if (Lower == TEXT("bored"))     { OutEmotion = EInoAgentsEmotion::Bored;     return true; }
        if (Lower == TEXT("playful"))   { OutEmotion = EInoAgentsEmotion::Playful;   return true; }
        if (Lower == TEXT("confused"))  { OutEmotion = EInoAgentsEmotion::Confused;  return true; }

        return false;
    }
}

ULiteRtLmSetEmotionTool::ULiteRtLmSetEmotionTool()
{
    ToolName = TEXT("set_emotion");
    Description = TEXT("Set the character's emotional expression. You MUST call this "
                       "tool whenever your emotional state changes during the conversation. "
                       "Call it BEFORE your text response so the emotion is visible while "
                       "you speak.");

    FLiteRtLmToolParameter EmotionParam;
    EmotionParam.Name = TEXT("emotion");
    EmotionParam.Type = TEXT("string");
    EmotionParam.Description = TEXT("One of: neutral, happy, sad, disgust, anger, "
                                    "surprise, fear, confident, excited, bored, "
                                    "playful, confused");
    EmotionParam.bRequired = true;
    Parameters.Add(EmotionParam);
}

void ULiteRtLmSetEmotionTool::SetAgent(UInoAgentsLiteRtLmAgentComponent* InAgent)
{
    AgentWeak = InAgent;
}

FString ULiteRtLmSetEmotionTool::Execute_Implementation(const FJsonObjectWrapper& Arguments)
{
    if (!Arguments.JsonObject.IsValid())
    {
        return FString(TEXT("\"ERROR: missing arguments\""));
    }

    FString EmotionStr;
    if (!Arguments.JsonObject->TryGetStringField(TEXT("emotion"), EmotionStr))
    {
        return FString(TEXT("\"ERROR: missing 'emotion' parameter\""));
    }

    // Gemma 4's FC format wraps string values in escape tags like
    // <|"|>happy<|"|> or <ctrl46>happy<ctrl46>. Strip them.
    EmotionStr.ReplaceInline(TEXT("<|\"|>"), TEXT(""));
    EmotionStr.ReplaceInline(TEXT("<ctrl46>"), TEXT(""));

    EInoAgentsEmotion NewEmotion;
    if (!ParseEmotion(EmotionStr, NewEmotion))
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("set_emotion: unrecognized emotion \"%s\", defaulting to Neutral"),
               *EmotionStr);
        NewEmotion = EInoAgentsEmotion::Neutral;
    }

    UInoAgentsLiteRtLmAgentComponent* Agent = AgentWeak.Get();
    if (Agent == nullptr)
    {
        return FString(TEXT("\"ERROR: agent component no longer exists\""));
    }

    Agent->SetEmotion(NewEmotion);

    UE_LOG(LogInoAgents, Log,
           TEXT("set_emotion: %s"), *EmotionStr);

    return FString::Printf(TEXT("\"%s\""), *EmotionStr);
}
