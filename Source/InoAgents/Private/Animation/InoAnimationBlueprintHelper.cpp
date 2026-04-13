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
        // Project gaze onto the head-local axes via dot products.
        // Forward component isn't used directly — yaw and pitch are
        // measured as angles from the forward axis in the horizontal
        // and vertical planes respectively.
        const float DotForward = FVector::DotProduct(GazeDir, Forward);
        const float DotRight   = FVector::DotProduct(GazeDir, Right);
        const float DotUp      = FVector::DotProduct(GazeDir, Up);

        // Yaw: angle in the horizontal plane (Forward, Right).
        // Positive = right, negative = left.
        OutYaw = FMath::Atan2(DotRight, DotForward);

        // Pitch: angle above/below the horizontal plane.
        // Positive = up, negative = down.
        const float HorizontalLen = FMath::Sqrt(DotForward * DotForward + DotRight * DotRight);
        OutPitch = FMath::Atan2(DotUp, HorizontalLen);
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
    FInoEyeLookWeights Target;

    MaxAngleDegrees = FMath::Clamp(MaxAngleDegrees, 1.f, 90.f);
    const float MaxAngleRad = FMath::DegreesToRadians(MaxAngleDegrees);

    // Build an orthonormal head-local basis.
    const FVector Forward = HeadForwardVector.GetSafeNormal();
    if (Forward.IsNearlyZero())
    {
        return Target;
    }

    FVector Up = HeadUpVector.GetSafeNormal();
    if (Up.IsNearlyZero())
    {
        Up = FVector::UpVector;
    }

    // Ensure orthogonality. UE is left-handed (X=Forward, Y=Right, Z=Up),
    // so Right = Up x Forward, and re-derived Up = Forward x Right.
    const FVector Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    Up = FVector::CrossProduct(Forward, Right).GetSafeNormal();

    // --- Left eye ---
    {
        const FVector GazeDir = (LookAtTarget - LeftEyeWorldPos).GetSafeNormal();
        if (!GazeDir.IsNearlyZero())
        {
            float Yaw, Pitch;
            DecomposeGaze(GazeDir, Forward, Up, Right, Yaw, Pitch);

            const float YawWeight   = FMath::Clamp(FMath::Abs(Yaw)   / MaxAngleRad, 0.f, 1.f);
            const float PitchWeight = FMath::Clamp(FMath::Abs(Pitch) / MaxAngleRad, 0.f, 1.f);

            if (Yaw < 0.f)
                Target.EyeLookLeftL  = YawWeight;
            else
                Target.EyeLookRightL = YawWeight;

            if (Pitch > 0.f)
                Target.EyeLookUpL   = PitchWeight;
            else
                Target.EyeLookDownL = PitchWeight;
        }
    }

    // --- Right eye ---
    {
        const FVector GazeDir = (LookAtTarget - RightEyeWorldPos).GetSafeNormal();
        if (!GazeDir.IsNearlyZero())
        {
            float Yaw, Pitch;
            DecomposeGaze(GazeDir, Forward, Up, Right, Yaw, Pitch);

            const float YawWeight   = FMath::Clamp(FMath::Abs(Yaw)   / MaxAngleRad, 0.f, 1.f);
            const float PitchWeight = FMath::Clamp(FMath::Abs(Pitch) / MaxAngleRad, 0.f, 1.f);

            if (Yaw < 0.f)
                Target.EyeLookLeftR  = YawWeight;
            else
                Target.EyeLookRightR = YawWeight;

            if (Pitch > 0.f)
                Target.EyeLookUpR   = PitchWeight;
            else
                Target.EyeLookDownR = PitchWeight;
        }
    }

    // Interpolate from previous weights toward the target for smooth motion.
    if (InterpSpeed <= 0.f || DeltaTime <= 0.f)
    {
        return Target;
    }

    const float Alpha = FMath::Clamp(DeltaTime * InterpSpeed, 0.f, 1.f);

    FInoEyeLookWeights Result;
    Result.EyeLookUpL    = FMath::Lerp(PreviousWeights.EyeLookUpL,    Target.EyeLookUpL,    Alpha);
    Result.EyeLookDownL  = FMath::Lerp(PreviousWeights.EyeLookDownL,  Target.EyeLookDownL,  Alpha);
    Result.EyeLookLeftL  = FMath::Lerp(PreviousWeights.EyeLookLeftL,  Target.EyeLookLeftL,  Alpha);
    Result.EyeLookRightL = FMath::Lerp(PreviousWeights.EyeLookRightL, Target.EyeLookRightL, Alpha);
    Result.EyeLookUpR    = FMath::Lerp(PreviousWeights.EyeLookUpR,    Target.EyeLookUpR,    Alpha);
    Result.EyeLookDownR  = FMath::Lerp(PreviousWeights.EyeLookDownR,  Target.EyeLookDownR,  Alpha);
    Result.EyeLookLeftR  = FMath::Lerp(PreviousWeights.EyeLookLeftR,  Target.EyeLookLeftR,  Alpha);
    Result.EyeLookRightR = FMath::Lerp(PreviousWeights.EyeLookRightR, Target.EyeLookRightR, Alpha);
    return Result;
}
