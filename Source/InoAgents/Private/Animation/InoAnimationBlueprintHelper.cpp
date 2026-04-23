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

    /**
     * Pick the next inter-blink interval.
     *
     * Two-component pseudo-log-normal approximation:
     *   - With probability Cfg.LongPauseChance (default 10%), sample an
     *     extended "stare" interval in [MaxInterval, MaxInterval *
     *     LongPauseMaxMultiplier]. Mimics the heavy right tail of real
     *     human blink intervals (focused / thinking periods).
     *   - Otherwise, sample uniform in [MinInterval, MaxInterval].
     *     This is the typical distraction-free range.
     *
     * Finally, scale by a speaking-rate multiplier so active speech
     * compresses intervals (real humans blink ~60% more often while
     * talking). At SpeakingIntensity=1.0 the returned interval is 60%
     * of the sampled value.
     */
    float PickNextBlinkInterval(
        FRandomStream&           Rng,
        const FInoBlinkConfig&   Cfg,
        float                    SpeakingIntensity)
    {
        const float LongPauseChance = FMath::Clamp(Cfg.LongPauseChance, 0.f, 1.f);
        const float LongPauseMul    = FMath::Max(Cfg.LongPauseMaxMultiplier, 1.f);

        float Interval;
        if (LongPauseChance > 0.f && Rng.FRand() < LongPauseChance)
        {
            // Extended stare: above the typical-range ceiling up to N×.
            const float StareMin = Cfg.MaxInterval;
            const float StareMax = Cfg.MaxInterval * LongPauseMul;
            Interval = RandRange(Rng, StareMin, StareMax);
        }
        else
        {
            Interval = RandRange(Rng, Cfg.MinInterval, Cfg.MaxInterval);
        }

        // Speaking compresses intervals. At SpeakingIntensity=1.0 the
        // multiplier is 0.6 — matches the ~60% increase in blink rate
        // observed during active speech in psychophysical studies.
        const float Speak = FMath::Clamp(SpeakingIntensity, 0.f, 1.f);
        const float SpeakMul = 1.0f - 0.4f * Speak;

        return Interval * SpeakMul;
    }

    /** Randomise per-blink durations (seconds). */
    void PickBlinkDurations(FRandomStream& Rng, const FInoBlinkConfig& Cfg,
                            float& OutClose, float& OutHold, float& OutOpen)
    {
        OutClose = RandRange(Rng, Cfg.MinCloseDuration, Cfg.MaxCloseDuration);
        OutHold  = RandRange(Rng, Cfg.MinHoldDuration,  Cfg.MaxHoldDuration);
        OutOpen  = RandRange(Rng, Cfg.MinOpenDuration,   Cfg.MaxOpenDuration);
    }

    /**
     * Cubic ease-in for the eyelid-close phase. Slow start, accelerating
     * finish — matches gravity-assisted eyelid descent. Real closes are
     * almost a "snap"; pure linear or ease-in-out looks too mechanical.
     */
    float EaseInCubic(float T)
    {
        T = FMath::Clamp(T, 0.f, 1.f);
        return T * T * T;
    }

    /**
     * Quadratic ease-out for the eyelid-open phase. Fast start, decelerating
     * finish — the eye settles into the open position rather than snapping
     * to it. Slower than the close by design (see interval defaults).
     */
    float EaseOutQuad(float T)
    {
        T = FMath::Clamp(T, 0.f, 1.f);
        const float OneMinus = 1.f - T;
        return 1.f - OneMinus * OneMinus;
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
    const FInoBlinkState& PreviousState,
    const FInoBlinkConfig& Config,
    float SpeakingIntensity)
{
    FInoBlinkState S = PreviousState;

    // First-frame init: seed the RNG and pick the first blink time.
    // Use non-deterministic seed from FMath::Rand() so each character
    // starts blinking on an independent schedule — if we all seeded
    // from 0, every NPC in a crowd would blink in lockstep.
    if (!S.bSeeded)
    {
        S.Seed = FMath::Rand();
        S.bSeeded = true;

        FRandomStream InitRng(S.Seed);
        S.NextBlinkTime = PickNextBlinkInterval(InitRng, Config, SpeakingIntensity);
        S.Seed = InitRng.RandHelper(MAX_int32);

        S.Timer       = 0.f;
        S.Phase       = 0;
        S.PhaseTimer  = 0.f;
        S.BlinkWeight = 0.f;
        return S;
    }

    // Don't process zero or negative time — paused game, bad frame, etc.
    if (DeltaTime <= 0.f)
    {
        return S;
    }

    FRandomStream Rng(S.Seed);

    // -------------------------------------------------------------------
    //  State machine: drain DeltaTime across as many phase transitions
    //  as needed in this single tick. This is what makes the blink
    //  framerate-independent even at 30 fps where a whole hold phase
    //  (30–70 ms) can complete inside a 33 ms frame.
    //
    //  The loop body is one phase-advance step: it consumes AT MOST the
    //  time needed to finish the current phase, then the loop reconsiders.
    //  A generous safety bound stops any theoretical infinite loop if all
    //  durations collapse to zero at runtime.
    // -------------------------------------------------------------------
    float Remaining = DeltaTime;
    int32 SafetyBound = 16;  // max phase transitions per frame

    while (Remaining > 0.f && SafetyBound-- > 0)
    {
        if (S.Phase == 0)
        {
            // Idle — wait out the interval, then start a close.
            const float TimeLeft = S.NextBlinkTime - S.Timer;
            if (Remaining < TimeLeft)
            {
                // Still idle at end of frame.
                S.Timer += Remaining;
                Remaining = 0.f;
            }
            else
            {
                // Interval elapsed this frame — carry the overshoot
                // into the close phase so short intervals at low fps
                // don't get clipped.
                Remaining -= TimeLeft;
                S.Timer   = S.NextBlinkTime;

                // Pick per-blink durations.
                PickBlinkDurations(Rng, Config,
                                   S.CloseDuration, S.HoldDuration, S.OpenDuration);
                S.Seed = Rng.RandHelper(MAX_int32);

                // Maybe queue a double blink. Only rolls if we're not
                // already servicing one — avoids triple/quadruple chains.
                if (S.PendingDoubleBlinks == 0
                    && Config.DoubleBlinkChance > 0.f
                    && Rng.FRand() < Config.DoubleBlinkChance)
                {
                    S.PendingDoubleBlinks = 1;
                }
                S.Seed = Rng.RandHelper(MAX_int32);

                S.Phase      = 1;
                S.PhaseTimer = 0.f;
            }
        }
        else if (S.Phase == 1)
        {
            // Closing — eyelid descends. Use EaseInCubic externally.
            const float TimeLeft = S.CloseDuration - S.PhaseTimer;
            if (Remaining < TimeLeft)
            {
                S.PhaseTimer += Remaining;
                Remaining = 0.f;
            }
            else
            {
                Remaining    -= TimeLeft;
                S.Phase       = 2;
                S.PhaseTimer  = 0.f;
            }
        }
        else if (S.Phase == 2)
        {
            // Holding closed.
            const float TimeLeft = S.HoldDuration - S.PhaseTimer;
            if (Remaining < TimeLeft)
            {
                S.PhaseTimer += Remaining;
                Remaining = 0.f;
            }
            else
            {
                Remaining    -= TimeLeft;
                S.Phase       = 3;
                S.PhaseTimer  = 0.f;
            }
        }
        else  // S.Phase == 3 (opening)
        {
            const float TimeLeft = S.OpenDuration - S.PhaseTimer;
            if (Remaining < TimeLeft)
            {
                S.PhaseTimer += Remaining;
                Remaining = 0.f;
            }
            else
            {
                Remaining -= TimeLeft;

                if (S.PendingDoubleBlinks > 0)
                {
                    // Chain immediately into a second blink. Re-roll
                    // per-blink durations so the double blink isn't
                    // a bit-exact copy of the first.
                    S.PendingDoubleBlinks--;
                    PickBlinkDurations(Rng, Config,
                                       S.CloseDuration, S.HoldDuration, S.OpenDuration);
                    S.Seed = Rng.RandHelper(MAX_int32);

                    S.Phase      = 1;
                    S.PhaseTimer = 0.f;
                }
                else
                {
                    // Back to idle; pick the NEXT blink interval using
                    // the current SpeakingIntensity so starting/stopping
                    // to talk takes effect at the cleanest possible point
                    // (between blinks, not mid-eyelid-motion).
                    S.NextBlinkTime = PickNextBlinkInterval(Rng, Config, SpeakingIntensity);
                    S.Seed = Rng.RandHelper(MAX_int32);

                    S.Phase      = 0;
                    S.Timer      = 0.f;
                    S.PhaseTimer = 0.f;
                }
            }
        }
    }

    // Persist the RNG state for next call.
    S.Seed = Rng.RandHelper(MAX_int32);

    // -------------------------------------------------------------------
    //  Compute BlinkWeight from the final phase state. Single source of
    //  truth — no setting it inside the loop above means we can't drift
    //  between "phase advanced" and "weight updated".
    //
    //  Close uses EaseInCubic (snap-like), open uses EaseOutQuad
    //  (muscle-driven settle). Hold is hardcoded 1.0. Idle is 0.0.
    // -------------------------------------------------------------------
    switch (S.Phase)
    {
        case 0:
            S.BlinkWeight = 0.f;
            break;

        case 1:
        {
            const float T = (S.CloseDuration > 0.001f)
                ? (S.PhaseTimer / S.CloseDuration)
                : 1.f;
            S.BlinkWeight = EaseInCubic(T);
            break;
        }

        case 2:
            S.BlinkWeight = 1.f;
            break;

        case 3:
        {
            const float T = (S.OpenDuration > 0.001f)
                ? (S.PhaseTimer / S.OpenDuration)
                : 1.f;
            S.BlinkWeight = 1.f - EaseOutQuad(T);
            break;
        }

        default:
            // Shouldn't happen, but defense in depth.
            S.BlinkWeight = 0.f;
            S.Phase       = 0;
            break;
    }

    return S;
}
