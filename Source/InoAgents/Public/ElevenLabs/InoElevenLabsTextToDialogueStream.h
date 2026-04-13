// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "ElevenLabs/InoElevenLabsTypes.h"

#include "InoElevenLabsTextToDialogueStream.generated.h"

class UInoElevenLabsSubsystem;

/**
 * Latent async action that calls ElevenLabs' POST /v1/text-to-dialogue/stream
 * endpoint and delivers the resulting audio back to Blueprint/C++ in chunks
 * as it arrives.
 *
 * Blueprint usage:
 *   Drag out the "ElevenLabs Stream Text-to-Dialogue" node, feed it an
 *   FInoElevenLabsDialogueRequest, bind OnAudioChunk / OnComplete / OnError,
 *   done. The node is latent - execution continues on the bound pin as
 *   each event fires.
 *
 * C++ usage:
 *     FInoElevenLabsDialogueRequest Req;
 *     Req.Inputs.Add({TEXT("Knock knock"),  TEXT("JBFqnCBsd6RMkjVDRZzb")});
 *     Req.Inputs.Add({TEXT("Who's there?"), TEXT("Aw4FAjKCGjjNkVhN1Xmq")});
 *
 *     UInoElevenLabsTextToDialogueStream* Action =
 *         UInoElevenLabsTextToDialogueStream::StreamTextToDialogue(this, Req);
 *     Action->OnAudioChunk.AddDynamic(this, &UMyClass::HandleChunk);
 *     Action->OnComplete  .AddDynamic(this, &UMyClass::HandleComplete);
 *     Action->OnError     .AddDynamic(this, &UMyClass::HandleError);
 *     Action->Activate();
 *
 * Threading: ElevenLabs bytes arrive via UE's HTTP progress delegate which
 * is guaranteed to fire on the game thread, so delegate targets bound via
 * AddDynamic run on the game thread and can touch UObject state safely.
 *
 * Lifetime: on Activate() the action registers itself with
 * UInoElevenLabsSubsystem, which holds a UPROPERTY strong reference until
 * FinishCleanly() unregisters. No AddToRoot needed. If PIE ends before
 * the stream completes, the subsystem's Deinitialize() calls CancelAll()
 * which fires OnError("cancelled") cleanly before the game instance dies.
 */
UCLASS()
class INOAGENTS_API UInoElevenLabsTextToDialogueStream : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * Create a new stream action. Does NOT dispatch the HTTP request yet -
     * Blueprint's latent-action machinery calls Activate() automatically
     * once all delegate pins are wired; from C++, bind the delegates on
     * the returned object and then call Activate() manually.
     *
     * @param WorldContextObject  Any UObject with a World - used to
     *                            resolve the game instance and, via it,
     *                            the UInoElevenLabsSubsystem that holds the
     *                            cached API key and the live-request
     *                            registry.
     * @param Request             Dialogue inputs + optional per-call
     *                            overrides. Validated in Activate().
     * @param ApiKeyOverride      If non-empty, used instead of the
     *                            subsystem's cached key. Lets callers
     *                            fetch keys from their own secret store
     *                            per call without touching
     *                            Project Settings.
     */
    UFUNCTION(BlueprintCallable,
              meta = (BlueprintInternalUseOnly = "true",
                      WorldContext             = "WorldContextObject",
                      DisplayName              = "ElevenLabs Stream Text-to-Dialogue"),
              Category = "InoAgents|ElevenLabs")
    static UInoElevenLabsTextToDialogueStream* StreamTextToDialogue(
        UObject*                          WorldContextObject,
        const FInoElevenLabsDialogueRequest& Request,
        FString                           ApiKeyOverride);

    /**
     * Fires once for every incremental chunk of audio received from the
     * server. Bytes are the NEW bytes only (not the full accumulated
     * buffer), suitable for appending to a caller-side buffer. May fire
     * many times for a single request.
     */
    UPROPERTY(BlueprintAssignable)
    FOnInoElevenLabsDialogueChunk OnAudioChunk;

    /**
     * Fires exactly once when the stream completes successfully. The
     * FullAudioBytes payload is the complete response, byte-equal to
     * the concatenation of every OnAudioChunk the caller saw. Preserved
     * as a convenience so callers who don't care about true streaming
     * can bind this one event and ignore OnAudioChunk.
     */
    UPROPERTY(BlueprintAssignable)
    FOnInoElevenLabsDialogueComplete OnComplete;

    /**
     * Fires exactly once on any failure path: validation error, missing
     * API key, HTTP non-2xx, network error, or explicit CancelStream().
     * Message is human-readable and safe to surface in UI. After OnError
     * fires the action is finished - no further delegates will dispatch.
     */
    UPROPERTY(BlueprintAssignable)
    FOnInoElevenLabsDialogueError OnError;

    /**
     * Abort the in-flight request. Fires OnError("cancelled") then
     * destroys the action. Safe to call from any handler. No-op if the
     * action has already finished.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|ElevenLabs")
    void CancelStream();

    //~ UBlueprintAsyncActionBase interface
    virtual void Activate() override;
    //~ End UBlueprintAsyncActionBase interface

private:
    void HandleRequestComplete(FHttpRequestPtr  Request,
                               FHttpResponsePtr Response,
                               bool             bSucceeded);

    /** Broadcasts OnError and tears the action down. */
    void EmitErrorAndFinish(const FString& Message);

    /** Unregisters from the subsystem and marks the action ready to destroy. */
    void FinishCleanly();

    /** JSON body builder - also the reuse point for phases 2 and 3. */
    static FString BuildJsonBody(const FInoElevenLabsDialogueRequest& Req,
                                 const FString&                    FallbackModelId);

    /** Maps the enum to ElevenLabs' query-string value. */
    static FString OutputFormatToQueryString(EInoElevenLabsOutputFormat Fmt);

    /** Maps the enum to ElevenLabs' JSON string value. */
    static FString NormalizationToString(EInoElevenLabsTextNormalization N);

    /** Inputs passed to StreamTextToDialogue, captured until Activate(). */
    FInoElevenLabsDialogueRequest PendingRequest;

    /** Per-call override or subsystem cached key. Resolved at Activate(). */
    FString PendingApiKey;

    /** Optional override captured at factory time, applied in Activate(). */
    FString PendingApiKeyOverride;

    /** Weak link to the world context so Activate() can find its subsystem. */
    TWeakObjectPtr<UObject> WorldContextObjectWeak;

    /** Strong ref to the subsystem that owns this action's lifetime. */
    TWeakObjectPtr<UInoElevenLabsSubsystem> SubsystemWeak;

    /** In-flight HTTP request. Null after Finish. */
    FHttpRequestPtr HttpRequest;

    /** Set once any terminal delegate fires so CancelStream is a no-op. */
    bool bFinished = false;
};
