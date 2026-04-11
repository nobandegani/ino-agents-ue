// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "ElevenLabs/ElevenLabsTypes.h"

#include "ElevenLabsTextToDialogueStream.generated.h"

class UElevenLabsSubsystem;

/**
 * Latent async action that calls ElevenLabs' POST /v1/text-to-dialogue/stream
 * endpoint and delivers the resulting audio back to Blueprint/C++ in chunks
 * as it arrives.
 *
 * Blueprint usage:
 *   Drag out the "ElevenLabs Stream Text-to-Dialogue" node, feed it an
 *   FElevenLabsDialogueRequest, bind OnAudioChunk / OnComplete / OnError,
 *   done. The node is latent - execution continues on the bound pin as
 *   each event fires.
 *
 * C++ usage:
 *     FElevenLabsDialogueRequest Req;
 *     Req.Inputs.Add({TEXT("Knock knock"),  TEXT("JBFqnCBsd6RMkjVDRZzb")});
 *     Req.Inputs.Add({TEXT("Who's there?"), TEXT("Aw4FAjKCGjjNkVhN1Xmq")});
 *
 *     UElevenLabsTextToDialogueStream* Action =
 *         UElevenLabsTextToDialogueStream::StreamTextToDialogue(this, Req);
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
 * UElevenLabsSubsystem, which holds a UPROPERTY strong reference until
 * FinishCleanly() unregisters. No AddToRoot needed. If PIE ends before
 * the stream completes, the subsystem's Deinitialize() calls CancelAll()
 * which fires OnError("cancelled") cleanly before the game instance dies.
 */
UCLASS()
class INOAGENTS_API UElevenLabsTextToDialogueStream : public UBlueprintAsyncActionBase
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
     *                            the UElevenLabsSubsystem that holds the
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
    static UElevenLabsTextToDialogueStream* StreamTextToDialogue(
        UObject*                          WorldContextObject,
        const FElevenLabsDialogueRequest& Request,
        FString                           ApiKeyOverride);

    /**
     * Fires once for every incremental chunk of audio received from the
     * server. Bytes are the NEW bytes only (not the full accumulated
     * buffer), suitable for appending to a caller-side buffer. May fire
     * many times for a single request.
     */
    UPROPERTY(BlueprintAssignable)
    FOnElevenLabsDialogueChunk OnAudioChunk;

    /**
     * Fires exactly once when the stream completes successfully. The
     * FullAudioBytes payload is the complete response, byte-equal to
     * the concatenation of every OnAudioChunk the caller saw. Preserved
     * as a convenience so callers who don't care about true streaming
     * can bind this one event and ignore OnAudioChunk.
     */
    UPROPERTY(BlueprintAssignable)
    FOnElevenLabsDialogueComplete OnComplete;

    /**
     * Fires exactly once on any failure path: validation error, missing
     * API key, HTTP non-2xx, network error, or explicit CancelStream().
     * Message is human-readable and safe to surface in UI. After OnError
     * fires the action is finished - no further delegates will dispatch.
     */
    UPROPERTY(BlueprintAssignable)
    FOnElevenLabsDialogueError OnError;

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
    // UE 5.4+ uses the 64-bit progress delegate; the old int32 version is
    // deprecated. The byte counters are passed as uint64 but we clamp and
    // cast internally since our slicing arithmetic uses int32.
    void HandleRequestProgress(FHttpRequestPtr  Request,
                               uint64           BytesSent,
                               uint64           BytesReceived);

    void HandleRequestComplete(FHttpRequestPtr  Request,
                               FHttpResponsePtr Response,
                               bool             bSucceeded);

    /** Broadcasts OnError and tears the action down. */
    void EmitErrorAndFinish(const FString& Message);

    /** Unregisters from the subsystem and marks the action ready to destroy. */
    void FinishCleanly();

    /** JSON body builder - also the reuse point for phases 2 and 3. */
    static FString BuildJsonBody(const FElevenLabsDialogueRequest& Req,
                                 const FString&                    FallbackModelId);

    /** Maps the enum to ElevenLabs' query-string value. */
    static FString OutputFormatToQueryString(EElevenLabsOutputFormat Fmt);

    /** Maps the enum to ElevenLabs' JSON string value. */
    static FString NormalizationToString(EElevenLabsTextNormalization N);

    /** Inputs passed to StreamTextToDialogue, captured until Activate(). */
    FElevenLabsDialogueRequest PendingRequest;

    /** Per-call override or subsystem cached key. Resolved at Activate(). */
    FString PendingApiKey;

    /** Optional override captured at factory time, applied in Activate(). */
    FString PendingApiKeyOverride;

    /** Weak link to the world context so Activate() can find its subsystem. */
    TWeakObjectPtr<UObject> WorldContextObjectWeak;

    /** Strong ref to the subsystem that owns this action's lifetime. */
    TWeakObjectPtr<UElevenLabsSubsystem> SubsystemWeak;

    /** In-flight HTTP request. Null after Finish. */
    FHttpRequestPtr HttpRequest;

    /** Bytes-read high-water mark for incremental chunk slicing. */
    int32 LastReadOffset = 0;

    /** Set once any terminal delegate fires so CancelStream is a no-op. */
    bool bFinished = false;
};
