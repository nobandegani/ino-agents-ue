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
    float MaxAngleDegrees)
{
    FInoEyeLookWeights Result;

    MaxAngleDegrees = FMath::Clamp(MaxAngleDegrees, 1.f, 90.f);
    const float MaxAngleRad = FMath::DegreesToRadians(MaxAngleDegrees);

    // Build an orthonormal head-local basis.
    const FVector Forward = HeadForwardVector.GetSafeNormal();
    if (Forward.IsNearlyZero())
    {
        return Result;
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
                Result.EyeLookLeftL  = YawWeight;
            else
                Result.EyeLookRightL = YawWeight;

            if (Pitch > 0.f)
                Result.EyeLookUpL   = PitchWeight;
            else
                Result.EyeLookDownL = PitchWeight;
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
                Result.EyeLookLeftR  = YawWeight;
            else
                Result.EyeLookRightR = YawWeight;

            if (Pitch > 0.f)
                Result.EyeLookUpR   = PitchWeight;
            else
                Result.EyeLookDownR = PitchWeight;
        }
    }

    return Result;
}
