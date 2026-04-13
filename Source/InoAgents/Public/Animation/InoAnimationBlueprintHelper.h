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
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoEyeLookWeights
{
    GENERATED_BODY()

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
     * world-space look-at target.
     *
     * For each eye the function computes a local gaze direction
     * relative to the head's orientation, decomposes it into yaw
     * (left/right) and pitch (up/down), and maps the angles to
     * [0, 1] weights using MaxAngleDegrees as the full-deflection
     * angle.
     *
     * Because each eye has its own world position, vergence
     * (convergence on near targets) is handled naturally.
     *
     * @param LookAtTarget       World-space point the eyes should look at.
     * @param LeftEyeWorldPos    World position of the left eye socket.
     * @param RightEyeWorldPos   World position of the right eye socket.
     * @param HeadForwardVector  Head's forward direction (neutral gaze).
     *                           Does NOT need to be normalized — the
     *                           function normalizes internally.
     * @param HeadUpVector       Head's up direction. Does NOT need to
     *                           be normalized.
     * @param MaxAngleDegrees    Eye rotation range in degrees. Gaze
     *                           deflection at this angle produces
     *                           weight = 1. Typical human range is
     *                           ~35 degrees. Clamped to [1, 90].
     * @return                   Eight blend shape weights, all in [0, 1].
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Animation",
              meta = (DisplayName = "Calculate Eye Look Weights"))
    static FInoEyeLookWeights CalculateEyeLookWeights(
        FVector LookAtTarget,
        FVector LeftEyeWorldPos,
        FVector RightEyeWorldPos,
        FVector HeadForwardVector,
        FVector HeadUpVector,
        float MaxAngleDegrees = 35.f);
};
