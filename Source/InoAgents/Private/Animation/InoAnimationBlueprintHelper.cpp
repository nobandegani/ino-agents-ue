// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Animation/InoAnimationBlueprintHelper.h"

namespace
{
    /** Compute yaw and pitch (radians) of GazeDir in a local frame. */
    void DecomposeGaze(
        const FVector& GazeDir,
        const FVector& Forward,
        const FVector& Up,
        const FVector& Right,
        float& OutYaw,
        float& OutPitch)
    {
        const float DotForward = FVector::DotProduct(GazeDir, Forward);
        const float DotRight   = FVector::DotProduct(GazeDir, Right);
        const float DotUp      = FVector::DotProduct(GazeDir, Up);

        // Yaw: positive = right, negative = left.
        OutYaw = FMath::Atan2(DotRight, DotForward);

        // Pitch: positive = up, negative = down.
        const float HorizontalLen = FMath::Sqrt(DotForward * DotForward + DotRight * DotRight);
        OutPitch = FMath::Atan2(DotUp, HorizontalLen);
    }

    /** Convert a signed angle (radians) to a [0,1] weight. */
    float AngleToWeight(float AngleRad, float MaxAngleRad)
    {
        return FMath::Clamp(FMath::Abs(AngleRad) / MaxAngleRad, 0.f, 1.f);
    }
}

FInoEyeLookWeights UInoAnimationBlueprintHelper::CalculateEyeLookWeights(
    FVector LookAtTarget,
    FVector LeftEyeWorldPos,
    FVector RightEyeWorldPos,
    FVector HeadForwardVector,
    FVector HeadUpVector,
    float DeltaTime,
    const FInoEyeLookWeights& PreviousWeights,
    float MaxAngleDegrees,
    float InterpSpeed)
{
    MaxAngleDegrees = FMath::Clamp(MaxAngleDegrees, 1.f, 90.f);
    const float MaxAngleRad = FMath::DegreesToRadians(MaxAngleDegrees);

    // Build an orthonormal head-local basis.
    const FVector Forward = HeadForwardVector.GetSafeNormal();
    if (Forward.IsNearlyZero())
    {
        return PreviousWeights;
    }

    FVector Up = HeadUpVector.GetSafeNormal();
    if (Up.IsNearlyZero())
    {
        Up = FVector::UpVector;
    }

    // UE left-handed: Right = Up x Forward, Up = Forward x Right.
    const FVector Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    Up = FVector::CrossProduct(Forward, Right).GetSafeNormal();

    // Compute target yaw/pitch per eye.
    float TargetLYaw = 0.f, TargetLPitch = 0.f;
    float TargetRYaw = 0.f, TargetRPitch = 0.f;

    {
        const FVector GazeDir = (LookAtTarget - LeftEyeWorldPos).GetSafeNormal();
        if (!GazeDir.IsNearlyZero())
        {
            DecomposeGaze(GazeDir, Forward, Up, Right, TargetLYaw, TargetLPitch);
        }
    }
    {
        const FVector GazeDir = (LookAtTarget - RightEyeWorldPos).GetSafeNormal();
        if (!GazeDir.IsNearlyZero())
        {
            DecomposeGaze(GazeDir, Forward, Up, Right, TargetRYaw, TargetRPitch);
        }
    }

    // Interpolate in angle space (smooth, no jitter at direction crossings).
    float SmoothedLYaw   = TargetLYaw;
    float SmoothedLPitch = TargetLPitch;
    float SmoothedRYaw   = TargetRYaw;
    float SmoothedRPitch = TargetRPitch;

    if (InterpSpeed > 0.f && DeltaTime > 0.f)
    {
        const float Alpha = FMath::Clamp(DeltaTime * InterpSpeed, 0.f, 1.f);
        SmoothedLYaw   = FMath::Lerp(PreviousWeights.LeftEyeYaw,   TargetLYaw,   Alpha);
        SmoothedLPitch = FMath::Lerp(PreviousWeights.LeftEyePitch,  TargetLPitch,  Alpha);
        SmoothedRYaw   = FMath::Lerp(PreviousWeights.RightEyeYaw,  TargetRYaw,   Alpha);
        SmoothedRPitch = FMath::Lerp(PreviousWeights.RightEyePitch, TargetRPitch, Alpha);
    }

    // Convert smoothed angles to blend shape weights.
    FInoEyeLookWeights Result;

    // Store angles for next frame's interpolation.
    Result.LeftEyeYaw   = SmoothedLYaw;
    Result.LeftEyePitch = SmoothedLPitch;
    Result.RightEyeYaw  = SmoothedRYaw;
    Result.RightEyePitch = SmoothedRPitch;

    // Left eye weights.
    if (SmoothedLYaw < 0.f)
        Result.EyeLookLeftL  = AngleToWeight(SmoothedLYaw, MaxAngleRad);
    else
        Result.EyeLookRightL = AngleToWeight(SmoothedLYaw, MaxAngleRad);

    if (SmoothedLPitch > 0.f)
        Result.EyeLookUpL   = AngleToWeight(SmoothedLPitch, MaxAngleRad);
    else
        Result.EyeLookDownL = AngleToWeight(SmoothedLPitch, MaxAngleRad);

    // Right eye weights.
    if (SmoothedRYaw < 0.f)
        Result.EyeLookLeftR  = AngleToWeight(SmoothedRYaw, MaxAngleRad);
    else
        Result.EyeLookRightR = AngleToWeight(SmoothedRYaw, MaxAngleRad);

    if (SmoothedRPitch > 0.f)
        Result.EyeLookUpR   = AngleToWeight(SmoothedRPitch, MaxAngleRad);
    else
        Result.EyeLookDownR = AngleToWeight(SmoothedRPitch, MaxAngleRad);

    return Result;
}
