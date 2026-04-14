// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "InoAnimationBlueprintHelper.generated.h"

/**
 * Per-eye blend shape weights for gaze direction.
 *
 * Each weight is in [0, 1]. For a given eye only one horizontal
 * (Left / Right) and one vertical (Up / Down) weight will be
 * non-zero at a time — the opposite direction is clamped to 0.
 *
 * The Yaw/Pitch fields carry the smoothed gaze angles (radians)
 * between frames. Wire the whole struct back as PreviousWeights
 * — the angle fields drive the interpolation, the blend shape
 * weights are derived from them each frame.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoEyeLookWeights
{
    GENERATED_BODY()

    // ---- blend shape outputs (read these) ----

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookUpL = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookDownL = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookLeftL = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookRightL = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookUpR = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookDownR = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookLeftR = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float EyeLookRightR = 0.f;

    // ---- interpolation state (feed back as PreviousWeights) ----

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float LeftEyeYaw = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float LeftEyePitch = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float RightEyeYaw = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float RightEyePitch = 0.f;
};

/**
 * Animation helper functions exposed to Blueprint.
 */
UCLASS()
class INOAGENTS_API UInoAnimationBlueprintHelper : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Calculate ARKit-style eye look blend shape weights from a
     * world-space look-at target, with frame-rate-independent smoothing.
     *
     * Interpolation happens in angle space (yaw / pitch per eye),
     * then the smoothed angles are converted to blend shape weights.
     * This avoids the jitter that occurs when lerping opposing
     * weights independently.
     *
     * Pass the previous frame's output as PreviousWeights — it
     * carries the smoothed yaw/pitch state. On the first frame,
     * pass a default-constructed (zeroed) FInoEyeLookWeights.
     *
     * @param LookAtTarget       World-space point the eyes should look at.
     * @param LeftEyeWorldPos    World position of the left eye socket.
     * @param RightEyeWorldPos   World position of the right eye socket.
     * @param HeadForwardVector  Head's forward direction (neutral gaze).
     * @param HeadUpVector       Head's up direction.
     * @param DeltaTime          Frame delta time (seconds).
     * @param PreviousWeights    Output from the previous frame.
     * @param MaxAngleDegrees    Full-deflection angle. Default 35.
     * @param InterpSpeed        Interpolation speed (units/sec). Higher
     *                           = faster tracking. 0 = instant snap.
     *                           Good starting value: 8–15.
     * @return                   Blend shape weights + smoothed angles.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Animation",
              meta = (DisplayName = "Calculate Eye Look Weights"))
    static FInoEyeLookWeights CalculateEyeLookWeights(
        FVector LookAtTarget,
        FVector LeftEyeWorldPos,
        FVector RightEyeWorldPos,
        FVector HeadForwardVector,
        FVector HeadUpVector,
        float DeltaTime,
        const FInoEyeLookWeights& PreviousWeights,
        float MaxAngleDegrees = 35.f,
        float InterpSpeed = 10.f);
};
