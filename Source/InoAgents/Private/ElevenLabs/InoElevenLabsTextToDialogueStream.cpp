// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ElevenLabs/InoElevenLabsTextToDialogueStream.h"

#include "ElevenLabs/InoElevenLabsSubsystem.h"
#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "Math/UnrealMathUtility.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    constexpr int32 kMaxInputs = 10;

    /** Convert a JSON tree into a compact (no whitespace) string. */
    FString SerializeJson(const TSharedRef<FJsonObject>& Object)
    {
        FString Out;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Object, Writer);
        return Out;
    }

    /** Parse ElevenLabs' 422 error body and extract the first message. */
    FString ExtractFirstErrorMessage(const FString& BodyJson)
    {
        if (BodyJson.IsEmpty())
        {
            return FString();
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyJson);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return FString();
        }

        // Shape 1: { "detail": [ { "msg": "..." } ] }  (422)
        const TArray<TSharedPtr<FJsonValue>>* DetailArray = nullptr;
        if (Root->TryGetArrayField(TEXT("detail"), DetailArray) && DetailArray && DetailArray->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* FirstObj = nullptr;
            if ((*DetailArray)[0]->TryGetObject(FirstObj) && FirstObj && FirstObj->IsValid())
            {
                FString Msg;
                if ((*FirstObj)->TryGetStringField(TEXT("msg"), Msg))
                {
                    return Msg;
                }
            }
        }

        // Shape 2: { "detail": "error string" }  (some 4xx)
        FString DetailString;
        if (Root->TryGetStringField(TEXT("detail"), DetailString))
        {
            return DetailString;
        }

        // Shape 3: { "error": "..." } or { "message": "..." }
        FString Maybe;
        if (Root->TryGetStringField(TEXT("error"), Maybe))   return Maybe;
        if (Root->TryGetStringField(TEXT("message"), Maybe)) return Maybe;

        return FString();
    }
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

FString UInoElevenLabsTextToDialogueStream::OutputFormatToQueryString(
    EInoElevenLabsOutputFormat Fmt)
{
    switch (Fmt)
    {
        case EInoElevenLabsOutputFormat::Mp3_44100_128: return TEXT("mp3_44100_128");
        case EInoElevenLabsOutputFormat::Mp3_44100_64:  return TEXT("mp3_44100_64");
        case EInoElevenLabsOutputFormat::Mp3_22050_32:  return TEXT("mp3_22050_32");
        case EInoElevenLabsOutputFormat::Pcm_16000:     return TEXT("pcm_16000");
        case EInoElevenLabsOutputFormat::Pcm_24000:     return TEXT("pcm_24000");
        case EInoElevenLabsOutputFormat::Pcm_44100:     return TEXT("pcm_44100");
        case EInoElevenLabsOutputFormat::Ulaw_8000:     return TEXT("ulaw_8000");
        default:                                     return TEXT("mp3_44100_128");
    }
}

FString UInoElevenLabsTextToDialogueStream::NormalizationToString(
    EInoElevenLabsTextNormalization N)
{
    switch (N)
    {
        case EInoElevenLabsTextNormalization::On:   return TEXT("on");
        case EInoElevenLabsTextNormalization::Off:  return TEXT("off");
        case EInoElevenLabsTextNormalization::Auto:
        default:                                 return TEXT("auto");
    }
}

FString UInoElevenLabsTextToDialogueStream::BuildJsonBody(
    const FInoElevenLabsDialogueRequest& Req,
    const FString&                    FallbackModelId)
{
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

    // inputs (required)
    TArray<TSharedPtr<FJsonValue>> InputsArr;
    InputsArr.Reserve(Req.Inputs.Num());
    for (const FInoElevenLabsDialogueInput& In : Req.Inputs)
    {
        const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("text"),     In.Text);
        Obj->SetStringField(TEXT("voice_id"), In.VoiceId);
        InputsArr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    Root->SetArrayField(TEXT("inputs"), InputsArr);

    // model_id: per-call value, or fall back to subsystem default.
    const FString EffectiveModelId = Req.ModelId.IsEmpty() ? FallbackModelId : Req.ModelId;
    if (!EffectiveModelId.IsEmpty())
    {
        Root->SetStringField(TEXT("model_id"), EffectiveModelId);
    }

    // Optional language_code
    if (!Req.LanguageCode.IsEmpty())
    {
        Root->SetStringField(TEXT("language_code"), Req.LanguageCode);
    }

    // settings.stability (always send - it's cheap and avoids surprises
    // from server-side default changes).
    const TSharedRef<FJsonObject> SettingsObj = MakeShared<FJsonObject>();
    SettingsObj->SetNumberField(TEXT("stability"), Req.Stability);
    Root->SetObjectField(TEXT("settings"), SettingsObj);

    // seed (omit when negative = "let the server pick")
    if (Req.Seed >= 0)
    {
        Root->SetNumberField(TEXT("seed"), static_cast<double>(Req.Seed));
    }

    // apply_text_normalization
    Root->SetStringField(TEXT("apply_text_normalization"),
                         NormalizationToString(Req.ApplyTextNormalization));

    return SerializeJson(Root);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

UInoElevenLabsTextToDialogueStream* UInoElevenLabsTextToDialogueStream::StreamTextToDialogue(
    UObject*                          WorldContextObject,
    const FInoElevenLabsDialogueRequest& Request,
    FString                           ApiKeyOverride)
{
    UInoElevenLabsTextToDialogueStream* Action = NewObject<UInoElevenLabsTextToDialogueStream>();
    Action->PendingRequest         = Request;
    Action->PendingApiKeyOverride  = ApiKeyOverride;
    Action->WorldContextObjectWeak = WorldContextObject;

    // Hook into Blueprint's latent-action machinery when called from BP.
    // RegisterWithGameInstance is a no-op when WorldContextObject is null
    // (e.g. from a console command) - the subsystem's LiveActions set
    // handles GC lifetime in that case.
    if (WorldContextObject != nullptr)
    {
        Action->RegisterWithGameInstance(WorldContextObject);
    }

    return Action;
}

// ---------------------------------------------------------------------------
// Activate
// ---------------------------------------------------------------------------

void UInoElevenLabsTextToDialogueStream::Activate()
{
    // 1. Resolve the subsystem via the captured WorldContextObject.
    UInoElevenLabsSubsystem* Subsystem = nullptr;
    if (UObject* Ctx = WorldContextObjectWeak.Get())
    {
        if (UGameInstance* GI = UGameplayStatics::GetGameInstance(Ctx))
        {
            Subsystem = GI->GetSubsystem<UInoElevenLabsSubsystem>();
        }
    }
    if (Subsystem == nullptr)
    {
        EmitErrorAndFinish(
            TEXT("No UInoElevenLabsSubsystem — call from a live game instance (PIE or packaged)"));
        return;
    }
    SubsystemWeak = Subsystem;

    // 2. Resolve API key: override > subsystem cache.
    PendingApiKey = !PendingApiKeyOverride.IsEmpty()
        ? PendingApiKeyOverride
        : Subsystem->GetApiKey();

    if (PendingApiKey.IsEmpty())
    {
        EmitErrorAndFinish(
            TEXT("API key is empty; set it in Project Settings -> Plugins -> "
                 "InoAgents ElevenLabs, or pass one to StreamTextToDialogue"));
        return;
    }

    // 3. Validate the request.
    if (PendingRequest.Inputs.Num() == 0)
    {
        EmitErrorAndFinish(TEXT("Dialogue request has no inputs"));
        return;
    }
    if (PendingRequest.Inputs.Num() > kMaxInputs)
    {
        EmitErrorAndFinish(FString::Printf(
            TEXT("Dialogue request has %d inputs but ElevenLabs allows at most %d"),
            PendingRequest.Inputs.Num(), kMaxInputs));
        return;
    }
    TSet<FString> UniqueVoices;
    for (const FInoElevenLabsDialogueInput& In : PendingRequest.Inputs)
    {
        if (In.Text.IsEmpty())
        {
            EmitErrorAndFinish(TEXT("One of the dialogue inputs has an empty Text field"));
            return;
        }
        if (In.VoiceId.IsEmpty())
        {
            EmitErrorAndFinish(TEXT("One of the dialogue inputs has an empty VoiceId"));
            return;
        }
        UniqueVoices.Add(In.VoiceId);
    }
    if (UniqueVoices.Num() > kMaxInputs)
    {
        EmitErrorAndFinish(FString::Printf(
            TEXT("Dialogue request uses %d unique voice IDs but ElevenLabs allows at most %d"),
            UniqueVoices.Num(), kMaxInputs));
        return;
    }

    // 4. Register with the subsystem so GC keeps us alive and CancelAll
    //    can reach us.
    Subsystem->RegisterLiveAction(this);

    // 5. Build the URL.
    const FString Url = FString::Printf(
        TEXT("%s/v1/text-to-dialogue/stream?output_format=%s"),
        *Subsystem->GetBaseUrl(),
        *OutputFormatToQueryString(PendingRequest.OutputFormat));

    // 6. Build the JSON body.
    const FString Body = BuildJsonBody(PendingRequest, Subsystem->GetDefaultModelId());

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoElevenLabsTextToDialogueStream: POST %s (%d inputs, %d-byte body)"),
           *Url, PendingRequest.Inputs.Num(), Body.Len());

    // 7. Create and configure the request.
    HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetURL(Url);
    HttpRequest->SetVerb(TEXT("POST"));
    HttpRequest->SetHeader(TEXT("xi-api-key"),   PendingApiKey);
    HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    HttpRequest->SetHeader(TEXT("Accept"),       TEXT("*/*"));
    HttpRequest->SetContentAsString(Body);

    // NOTE: we deliberately do NOT use OnRequestProgress64 here.
    // Calling Response->GetContent() mid-stream triggers UE's internal
    // "Payload is incomplete" warning on every progress tick, spamming
    // the log. TTS responses are small (<200 KB) so there's no benefit
    // to incremental chunk delivery — the audio component's pre-buffer
    // handles playback latency regardless. The full buffer is delivered
    // as a single OnAudioChunk + OnComplete at request completion.
    HttpRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoElevenLabsTextToDialogueStream::HandleRequestComplete);

    HttpRequest->ProcessRequest();
}

// ---------------------------------------------------------------------------
// HTTP callbacks
// ---------------------------------------------------------------------------

void UInoElevenLabsTextToDialogueStream::HandleRequestComplete(
    FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
    if (bFinished)
    {
        return;
    }

    // Network / DNS / cancel path: bSucceeded is false.
    if (!bSucceeded || !Response.IsValid())
    {
        EmitErrorAndFinish(TEXT("HTTP request failed (network or connection error)"));
        return;
    }

    const int32 Code = Response->GetResponseCode();
    if (Code < 200 || Code >= 300)
    {
        // Always dump the full response body and the Content-Type header
        // at Warning level so we can diagnose unexpected server replies
        // (CDN HTML pages, SSE frames, JSON errors, etc.). The body is
        // usually under a few hundred bytes on errors so the log noise is
        // acceptable; the signal value is worth it.
        const FString BodyStr     = Response->GetContentAsString();
        const FString ContentType = Response->GetContentType();
        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoElevenLabsTextToDialogueStream: HTTP %d response — "
                    "Content-Type=\"%s\", %d-byte body:\n%s"),
               Code, *ContentType, BodyStr.Len(), *BodyStr);

        // Try to extract a human-readable message from the JSON error body
        // for the OnError delegate. Fall back to the first chunk of the
        // raw body if JSON parsing fails (bumped the truncation threshold
        // from 200 to 500 so typical CDN HTML pages still fit).
        const FString FirstMsg  = ExtractFirstErrorMessage(BodyStr);
        const FString Formatted = FirstMsg.IsEmpty()
            ? FString::Printf(TEXT("HTTP %d: %s"), Code,
                              BodyStr.Len() < 500 ? *BodyStr : TEXT("<truncated>"))
            : FString::Printf(TEXT("HTTP %d: %s"), Code, *FirstMsg);
        EmitErrorAndFinish(Formatted);
        return;
    }

    // Deliver the full buffer as a single OnAudioChunk so callers that
    // bind OnAudioChunk (like the TTS queue) still receive the data.
    // Then fire OnComplete with the same buffer.
    const TArray<uint8>& Full = Response->GetContent();

    if (Full.Num() > 0)
    {
        OnAudioChunk.Broadcast(Full, static_cast<int64>(Full.Num()));
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoElevenLabsTextToDialogueStream: complete, %d bytes"),
           Full.Num());

    OnComplete.Broadcast(Full, PendingRequest.OutputFormat);
    FinishCleanly();
}

// ---------------------------------------------------------------------------
// Cancel / teardown
// ---------------------------------------------------------------------------

void UInoElevenLabsTextToDialogueStream::CancelStream()
{
    if (bFinished)
    {
        return;
    }

    if (HttpRequest.IsValid())
    {
        HttpRequest->CancelRequest();
    }

    EmitErrorAndFinish(TEXT("cancelled"));
}

void UInoElevenLabsTextToDialogueStream::EmitErrorAndFinish(const FString& Message)
{
    if (bFinished)
    {
        return;
    }

    UE_LOG(LogInoAgents, Warning,
           TEXT("UInoElevenLabsTextToDialogueStream: error: %s"), *Message);

    OnError.Broadcast(Message);
    FinishCleanly();
}

void UInoElevenLabsTextToDialogueStream::FinishCleanly()
{
    if (bFinished)
    {
        return;
    }
    bFinished = true;

    // Drop the HTTP delegate so a late callback can't re-enter us.
    if (HttpRequest.IsValid())
    {
        HttpRequest->OnProcessRequestComplete().Unbind();
        HttpRequest.Reset();
    }

    // Drop the subsystem's strong ref so GC can collect us.
    if (UInoElevenLabsSubsystem* Subsystem = SubsystemWeak.Get())
    {
        Subsystem->UnregisterLiveAction(this);
    }

    SetReadyToDestroy();
}
