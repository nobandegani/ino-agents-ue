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

    /** Random float in [Min, Max] using a deterministic stream. */
    float RandRange(FRandomStream& Rng, float Min, float Max)
    {
        return Min + Rng.FRand() * (Max - Min);
    }

    /** Pick the next random interval before a blink. */
    float PickNextBlinkInterval(FRandomStream& Rng)
    {
        // Humans blink every ~2–6 seconds on average.
        return RandRange(Rng, 2.0f, 6.0f);
    }

    /** Randomise per-blink durations (seconds). */
    void PickBlinkDurations(FRandomStream& Rng, float& OutClose, float& OutHold, float& OutOpen)
    {
        // Close: fast (~50–100 ms). Hold: brief (~30–70 ms). Open: slower (~100–200 ms).
        OutClose = RandRange(Rng, 0.05f, 0.10f);
        OutHold  = RandRange(Rng, 0.03f, 0.07f);
        OutOpen  = RandRange(Rng, 0.10f, 0.20f);
    }

    /** Smooth ease curve: fast start/end, smooth through 0→1. */
    float EaseInOut(float T)
    {
        // Hermite smoothstep.
        T = FMath::Clamp(T, 0.f, 1.f);
        return T * T * (3.f - 2.f * T);
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
    Result.LeftEyeYaw    = SmoothedLYaw;
    Result.LeftEyePitch  = SmoothedLPitch;
    Result.RightEyeYaw   = SmoothedRYaw;
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

FInoBlinkState UInoAnimationBlueprintHelper::CalculateBlinkWeight(
    float DeltaTime,
    const FInoBlinkState& PreviousState)
{
    FInoBlinkState S = PreviousState;

    // First-frame init: seed the RNG and pick the first blink time.
    if (!S.bSeeded)
    {
        S.Seed = FMath::Rand();
        S.bSeeded = true;
        FRandomStream Rng(S.Seed);
        // Advance the seed so subsequent calls get different values.
        S.Seed = Rng.RandHelper(MAX_int32);
        S.NextBlinkTime = PickNextBlinkInterval(Rng);
        S.Seed = Rng.RandHelper(MAX_int32);
        S.Timer = 0.f;
        S.Phase = 0;
        S.BlinkWeight = 0.f;
        return S;
    }

    FRandomStream Rng(S.Seed);

    if (S.Phase == 0)
    {
        // Idle — waiting for next blink.
        S.Timer += DeltaTime;
        S.BlinkWeight = 0.f;

        if (S.Timer >= S.NextBlinkTime)
        {
            // Start closing.
            S.Phase = 1;
            S.PhaseTimer = 0.f;
            PickBlinkDurations(Rng, S.CloseDuration, S.HoldDuration, S.OpenDuration);
            S.Seed = Rng.RandHelper(MAX_int32);

            // ~20% chance of a double blink.
            if (S.PendingDoubleBlinks == 0 && Rng.FRand() < 0.20f)
            {
                S.PendingDoubleBlinks = 1;
            }
            S.Seed = Rng.RandHelper(MAX_int32);
        }
    }

    if (S.Phase == 1)
    {
        // Closing.
        S.PhaseTimer += DeltaTime;
        const float T = FMath::Clamp(S.PhaseTimer / FMath::Max(S.CloseDuration, 0.001f), 0.f, 1.f);
        S.BlinkWeight = EaseInOut(T);

        if (S.PhaseTimer >= S.CloseDuration)
        {
            S.Phase = 2;
            S.PhaseTimer = 0.f;
            S.BlinkWeight = 1.f;
        }
    }
    else if (S.Phase == 2)
    {
        // Hold closed.
        S.PhaseTimer += DeltaTime;
        S.BlinkWeight = 1.f;

        if (S.PhaseTimer >= S.HoldDuration)
        {
            S.Phase = 3;
            S.PhaseTimer = 0.f;
        }
    }
    else if (S.Phase == 3)
    {
        // Opening.
        S.PhaseTimer += DeltaTime;
        const float T = FMath::Clamp(S.PhaseTimer / FMath::Max(S.OpenDuration, 0.001f), 0.f, 1.f);
        S.BlinkWeight = 1.f - EaseInOut(T);

        if (S.PhaseTimer >= S.OpenDuration)
        {
            // Blink complete.
            S.BlinkWeight = 0.f;

            if (S.PendingDoubleBlinks > 0)
            {
                // Immediately start another blink (double blink).
                S.PendingDoubleBlinks--;
                S.Phase = 1;
                S.PhaseTimer = 0.f;
                PickBlinkDurations(Rng, S.CloseDuration, S.HoldDuration, S.OpenDuration);
                S.Seed = Rng.RandHelper(MAX_int32);
            }
            else
            {
                // Back to idle.
                S.Phase = 0;
                S.Timer = 0.f;
                S.NextBlinkTime = PickNextBlinkInterval(Rng);
                S.Seed = Rng.RandHelper(MAX_int32);
            }
        }
    }

    return S;
}
