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

// ============================================================================
// CalculateGaze
// ============================================================================
//
// Merged eye + head tracking. Replaces the older separate
// CalculateEyeLookWeights + CalculateHeadLookRotation functions.
//
// Flow:
//   1. First-call init: seed RNG, pick a hold duration.
//   2. Build head-local orthonormal basis (Forward / Right / Up) so we
//      can decompose world-space gaze into local yaw / pitch.
//   3. Compute target yaw/pitch for each eye (using its own world
//      position) and for the head (using HeadWorldLocation). Clamp each
//      to its configured max angle.
//   4. Advance the attention state machine (Tracking / EyeGlance /
//      FullGlance). When the current state's timer expires, roll for
//      transition with the configured probabilities.
//   5. Decide per-frame "goal" angles based on the state. Tracking →
//      aim at target. EyeGlance → eyes to neutral, head to target.
//      FullGlance → both to neutral. Smooth toward the goal at the
//      configured InterpSpeed (eye + head independently).
//   6. Head dead-zone: if the target is within ±DeadZoneDegrees of
//      forward, the head stays put.
//   7. Fill the output struct: eye blend shape weights from smoothed
//      eye angles; head FRotator from smoothed head angles; apply
//      flags from the state + dead-zone; blend weights ramped toward
//      bApply* for crossfade-style callers.
// ============================================================================

namespace
{
    /** Three-state gaze attention machine. */
    enum class EGazeState : uint8
    {
        Tracking   = 0,
        EyeGlance  = 1,
        FullGlance = 2,
    };
}

FInoGazeResult UInoAnimationBlueprintHelper::CalculateGaze(
    FVector                 LookAtTarget,
    FVector                 HeadWorldLocation,
    FVector                 LeftEyeWorldPos,
    FVector                 RightEyeWorldPos,
    FVector                 HeadForwardVector,
    FVector                 HeadUpVector,
    float                   DeltaTime,
    const FInoGazeResult&   PreviousResult,
    const FInoGazeConfig&   Config)
{
    FInoGazeResult R = PreviousResult;

    // ---- First-call init ---------------------------------------------------
    if (!R.bSeeded)
    {
        R.Seed    = FMath::Rand();
        R.bSeeded = true;

        FRandomStream InitRng(R.Seed);
        R.NextGlanceChangeTime = RandRange(InitRng,
            Config.MinHoldDuration, Config.MaxHoldDuration);
        R.Seed = InitRng.RandHelper(MAX_int32);

        R.GlanceState      = static_cast<uint8>(EGazeState::Tracking);
        R.GlanceTimer      = 0.f;
        R.bApplyEyes       = true;
        R.bApplyHead       = true;
        R.EyeBlendWeight   = 1.f;
        R.HeadBlendWeight  = 1.f;
        // Fall through — still compute a rotation this frame.
    }

    // ---- Build head-local orthonormal basis -------------------------------
    const FVector Forward = HeadForwardVector.GetSafeNormal();
    if (Forward.IsNearlyZero())
    {
        // Bad input — return previous frame's state unchanged.
        return R;
    }
    FVector Up = HeadUpVector.GetSafeNormal();
    if (Up.IsNearlyZero())
    {
        Up = FVector::UpVector;
    }
    const FVector Right = FVector::CrossProduct(Up,      Forward).GetSafeNormal();
    Up                  = FVector::CrossProduct(Forward, Right  ).GetSafeNormal();

    // ---- Per-eye target yaw/pitch (radians) -------------------------------
    // Each eye has its own world position, so each has a slightly different
    // gaze direction to the same target (convergence / divergence). At
    // typical distances this is tiny, but doing it per-eye is correct and
    // free.
    const float MaxEyeAngleDeg =
        FMath::Clamp(Config.MaxEyeAngleDegrees, 1.f, 90.f);
    const float MaxEyeAngleRad = FMath::DegreesToRadians(MaxEyeAngleDeg);

    float TargetLYawRad   = 0.f, TargetLPitchRad = 0.f;
    float TargetRYawRad   = 0.f, TargetRPitchRad = 0.f;
    {
        const FVector GazeDirL = (LookAtTarget - LeftEyeWorldPos).GetSafeNormal();
        if (!GazeDirL.IsNearlyZero())
        {
            DecomposeGaze(GazeDirL, Forward, Up, Right,
                          TargetLYawRad, TargetLPitchRad);
        }
        const FVector GazeDirR = (LookAtTarget - RightEyeWorldPos).GetSafeNormal();
        if (!GazeDirR.IsNearlyZero())
        {
            DecomposeGaze(GazeDirR, Forward, Up, Right,
                          TargetRYawRad, TargetRPitchRad);
        }
    }
    TargetLYawRad   = FMath::Clamp(TargetLYawRad,   -MaxEyeAngleRad, MaxEyeAngleRad);
    TargetLPitchRad = FMath::Clamp(TargetLPitchRad, -MaxEyeAngleRad, MaxEyeAngleRad);
    TargetRYawRad   = FMath::Clamp(TargetRYawRad,   -MaxEyeAngleRad, MaxEyeAngleRad);
    TargetRPitchRad = FMath::Clamp(TargetRPitchRad, -MaxEyeAngleRad, MaxEyeAngleRad);

    // ---- Head target yaw/pitch + dead-zone ------------------------------
    const float MaxHeadYawRad =
        FMath::DegreesToRadians(FMath::Max(Config.MaxHeadYawDegrees, 0.f));
    const float MaxHeadPitchRad =
        FMath::DegreesToRadians(FMath::Max(Config.MaxHeadPitchDegrees, 0.f));
    const float DeadZoneRad =
        FMath::DegreesToRadians(FMath::Max(Config.HeadDeadZoneDegrees, 0.f));

    float TargetHeadYawRad   = 0.f;
    float TargetHeadPitchRad = 0.f;
    bool  bValidHeadGaze     = false;
    {
        const FVector GazeDir = (LookAtTarget - HeadWorldLocation).GetSafeNormal();
        if (!GazeDir.IsNearlyZero())
        {
            DecomposeGaze(GazeDir, Forward, Up, Right,
                          TargetHeadYawRad, TargetHeadPitchRad);
            bValidHeadGaze = true;
        }
    }
    TargetHeadYawRad   = FMath::Clamp(TargetHeadYawRad,   -MaxHeadYawRad,   MaxHeadYawRad);
    TargetHeadPitchRad = FMath::Clamp(TargetHeadPitchRad, -MaxHeadPitchRad, MaxHeadPitchRad);

    R.bInHeadDeadZone = bValidHeadGaze
        && FMath::Abs(TargetHeadYawRad)   < DeadZoneRad
        && FMath::Abs(TargetHeadPitchRad) < DeadZoneRad;

    // ---- Attention state machine ----------------------------------------
    FRandomStream Rng(R.Seed);
    if (DeltaTime > 0.f)
    {
        R.GlanceTimer += DeltaTime;
        if (R.GlanceTimer >= R.NextGlanceChangeTime)
        {
            R.GlanceTimer = 0.f;
            const EGazeState Current = static_cast<EGazeState>(R.GlanceState);

            if (Current == EGazeState::Tracking)
            {
                // Roll for transition: eye glance / full disengage / continue.
                const float EyeChance =
                    FMath::Clamp(Config.EyeGlanceChance, 0.f, 1.f);
                const float FullChance =
                    FMath::Clamp(Config.FullGlanceChance, 0.f, 1.f - EyeChance);

                const float Roll = Rng.FRand();
                if (Roll < EyeChance)
                {
                    R.GlanceState = static_cast<uint8>(EGazeState::EyeGlance);
                    R.NextGlanceChangeTime = RandRange(Rng,
                        Config.MinEyeGlanceDuration,
                        Config.MaxEyeGlanceDuration);
                }
                else if (Roll < EyeChance + FullChance)
                {
                    R.GlanceState = static_cast<uint8>(EGazeState::FullGlance);
                    R.NextGlanceChangeTime = RandRange(Rng,
                        Config.MinFullGlanceDuration,
                        Config.MaxFullGlanceDuration);
                }
                else
                {
                    // Roll for continued tracking — just pick another hold.
                    R.NextGlanceChangeTime = RandRange(Rng,
                        Config.MinHoldDuration, Config.MaxHoldDuration);
                }
            }
            else
            {
                // Coming back from any glance state → resume tracking.
                R.GlanceState = static_cast<uint8>(EGazeState::Tracking);
                R.NextGlanceChangeTime = RandRange(Rng,
                    Config.MinHoldDuration, Config.MaxHoldDuration);
            }
        }
    }
    R.Seed = Rng.RandHelper(MAX_int32);

    const EGazeState State = static_cast<EGazeState>(R.GlanceState);
    const bool bHeadTracks = (State != EGazeState::FullGlance);
    const bool bEyesTrack  = (State == EGazeState::Tracking);

    // ---- Smooth per-eye angles toward per-state goal ---------------------
    // When a component isn't tracking, interpolate its smoothed angle
    // toward zero (neutral) so re-engage doesn't snap.
    const float EyeGoalLYaw   = bEyesTrack ? TargetLYawRad   : 0.f;
    const float EyeGoalLPitch = bEyesTrack ? TargetLPitchRad : 0.f;
    const float EyeGoalRYaw   = bEyesTrack ? TargetRYawRad   : 0.f;
    const float EyeGoalRPitch = bEyesTrack ? TargetRPitchRad : 0.f;

    const float PrevLYawRad   = FMath::DegreesToRadians(R.LeftEyeYaw);
    const float PrevLPitchRad = FMath::DegreesToRadians(R.LeftEyePitch);
    const float PrevRYawRad   = FMath::DegreesToRadians(R.RightEyeYaw);
    const float PrevRPitchRad = FMath::DegreesToRadians(R.RightEyePitch);

    float SmoothedLYawRad   = EyeGoalLYaw;
    float SmoothedLPitchRad = EyeGoalLPitch;
    float SmoothedRYawRad   = EyeGoalRYaw;
    float SmoothedRPitchRad = EyeGoalRPitch;
    if (Config.EyeInterpSpeed > 0.f && DeltaTime > 0.f)
    {
        const float Alpha = FMath::Clamp(DeltaTime * Config.EyeInterpSpeed, 0.f, 1.f);
        SmoothedLYawRad   = FMath::Lerp(PrevLYawRad,   EyeGoalLYaw,   Alpha);
        SmoothedLPitchRad = FMath::Lerp(PrevLPitchRad, EyeGoalLPitch, Alpha);
        SmoothedRYawRad   = FMath::Lerp(PrevRYawRad,   EyeGoalRYaw,   Alpha);
        SmoothedRPitchRad = FMath::Lerp(PrevRPitchRad, EyeGoalRPitch, Alpha);
    }

    R.LeftEyeYaw    = FMath::RadiansToDegrees(SmoothedLYawRad);
    R.LeftEyePitch  = FMath::RadiansToDegrees(SmoothedLPitchRad);
    R.RightEyeYaw   = FMath::RadiansToDegrees(SmoothedRYawRad);
    R.RightEyePitch = FMath::RadiansToDegrees(SmoothedRPitchRad);

    // ---- Derive per-eye blend shape weights ------------------------------
    auto ZeroEyeWeights = [](FInoGazeResult& X)
    {
        X.EyeLookLeftL = X.EyeLookRightL = X.EyeLookUpL = X.EyeLookDownL = 0.f;
        X.EyeLookLeftR = X.EyeLookRightR = X.EyeLookUpR = X.EyeLookDownR = 0.f;
    };
    ZeroEyeWeights(R);

    if (SmoothedLYawRad < 0.f)
        R.EyeLookLeftL  = AngleToWeight(SmoothedLYawRad, MaxEyeAngleRad);
    else
        R.EyeLookRightL = AngleToWeight(SmoothedLYawRad, MaxEyeAngleRad);
    if (SmoothedLPitchRad > 0.f)
        R.EyeLookUpL    = AngleToWeight(SmoothedLPitchRad, MaxEyeAngleRad);
    else
        R.EyeLookDownL  = AngleToWeight(SmoothedLPitchRad, MaxEyeAngleRad);

    if (SmoothedRYawRad < 0.f)
        R.EyeLookLeftR  = AngleToWeight(SmoothedRYawRad, MaxEyeAngleRad);
    else
        R.EyeLookRightR = AngleToWeight(SmoothedRYawRad, MaxEyeAngleRad);
    if (SmoothedRPitchRad > 0.f)
        R.EyeLookUpR    = AngleToWeight(SmoothedRPitchRad, MaxEyeAngleRad);
    else
        R.EyeLookDownR  = AngleToWeight(SmoothedRPitchRad, MaxEyeAngleRad);

    // ---- Smooth head angles toward per-state goal ------------------------
    const float HeadGoalYawRad   = bHeadTracks ? TargetHeadYawRad   : 0.f;
    const float HeadGoalPitchRad = bHeadTracks ? TargetHeadPitchRad : 0.f;

    const float PrevHeadYawRad   = FMath::DegreesToRadians(R.HeadYaw);
    const float PrevHeadPitchRad = FMath::DegreesToRadians(R.HeadPitch);

    float SmoothedHeadYawRad   = HeadGoalYawRad;
    float SmoothedHeadPitchRad = HeadGoalPitchRad;
    if (Config.HeadInterpSpeed > 0.f && DeltaTime > 0.f)
    {
        const float Alpha = FMath::Clamp(DeltaTime * Config.HeadInterpSpeed, 0.f, 1.f);
        SmoothedHeadYawRad   = FMath::Lerp(PrevHeadYawRad,   HeadGoalYawRad,   Alpha);
        SmoothedHeadPitchRad = FMath::Lerp(PrevHeadPitchRad, HeadGoalPitchRad, Alpha);
    }

    R.HeadYaw   = FMath::RadiansToDegrees(SmoothedHeadYawRad);
    R.HeadPitch = FMath::RadiansToDegrees(SmoothedHeadPitchRad);

    // ---- Apply flags + blend weights -------------------------------------
    R.bApplyEyes = bEyesTrack;
    R.bApplyHead = bHeadTracks && !R.bInHeadDeadZone;

    const float EyeBlendTarget  = R.bApplyEyes ? 1.f : 0.f;
    const float HeadBlendTarget = R.bApplyHead ? 1.f : 0.f;
    if (DeltaTime > 0.f)
    {
        R.EyeBlendWeight  = FMath::FInterpTo(R.EyeBlendWeight,
                                             EyeBlendTarget,
                                             DeltaTime, /*InterpSpeed=*/ 10.f);
        R.HeadBlendWeight = FMath::FInterpTo(R.HeadBlendWeight,
                                             HeadBlendTarget,
                                             DeltaTime, /*InterpSpeed=*/ 10.f);
    }
    else
    {
        R.EyeBlendWeight  = EyeBlendTarget;
        R.HeadBlendWeight = HeadBlendTarget;
    }

    // ---- Head magnitude + output rotation --------------------------------
    const float YawMag   = (MaxHeadYawRad   > KINDA_SMALL_NUMBER)
        ? FMath::Abs(SmoothedHeadYawRad)   / MaxHeadYawRad   : 0.f;
    const float PitchMag = (MaxHeadPitchRad > KINDA_SMALL_NUMBER)
        ? FMath::Abs(SmoothedHeadPitchRad) / MaxHeadPitchRad : 0.f;
    R.HeadMagnitude = FMath::Clamp(FMath::Max(YawMag, PitchMag), 0.f, 1.f);

    R.HeadLookRotation = FRotator(R.HeadPitch, R.HeadYaw, 0.f);

    return R;
}
