// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "InoAnimationBlueprintHelper.generated.h"

// ============================================================================
// Blink — FInoBlinkConfig / FInoBlinkState / CalculateBlinkWeight
// ============================================================================

/** Configuration for procedural blink timing. */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoBlinkConfig
{
    GENERATED_BODY()

    /** Minimum seconds between blinks. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MinInterval = 2.f;

    /** Maximum seconds between blinks. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MaxInterval = 6.f;

    /** Minimum eyelid close duration (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MinCloseDuration = 0.05f;

    /** Maximum eyelid close duration (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MaxCloseDuration = 0.10f;

    /** Minimum hold-closed duration (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MinHoldDuration = 0.03f;

    /** Maximum hold-closed duration (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MaxHoldDuration = 0.07f;

    /** Minimum eyelid open duration (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MinOpenDuration = 0.10f;

    /** Maximum eyelid open duration (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation")
    float MaxOpenDuration = 0.20f;

    /** Chance of a double blink (0.0 – 1.0). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float DoubleBlinkChance = 0.20f;

    /**
     * Chance that any given inter-blink interval is an extended "stare"
     * pause rather than a typical-length interval (0.0 – 1.0). Real human
     * blink intervals follow a log-normal-like distribution — most fall
     * in the typical 2–6 s range, but occasional pauses stretch out to
     * 10–15 s (someone focused, thinking, or staring). 10% default
     * approximates real data.
     *
     * Set to 0.0 for purely uniform intervals (more metronomic but
     * simpler to reason about).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float LongPauseChance = 0.10f;

    /**
     * Upper multiplier on MaxInterval when a long pause fires. A value
     * of 2.5 means occasional intervals extend up to 2.5 × MaxInterval
     * (with MaxInterval=6 s → up to 15 s). Lower values keep long
     * pauses closer to the typical range.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "1.0", ClampMax = "10.0"))
    float LongPauseMaxMultiplier = 2.5f;
};

/**
 * Procedural eye blink state. Feed back as PreviousState each frame.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoBlinkState
{
    GENERATED_BODY()

    /** Current blink weight: 0 = fully open, 1 = fully closed. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float BlinkWeight = 0.f;

    // ---- internal state (feed back via PreviousState, don't modify) ----

    UPROPERTY()
    float Timer = 0.f;

    UPROPERTY()
    float NextBlinkTime = 0.f;

    /** 0 = idle, 1 = closing, 2 = hold, 3 = opening. */
    UPROPERTY()
    int32 Phase = 0;

    UPROPERTY()
    float PhaseTimer = 0.f;

    UPROPERTY()
    float CloseDuration = 0.f;

    UPROPERTY()
    float HoldDuration = 0.f;

    UPROPERTY()
    float OpenDuration = 0.f;

    /** Remaining double-blinks to perform after the current one. */
    UPROPERTY()
    int32 PendingDoubleBlinks = 0;

    /** Whether the RNG has been seeded (first-frame init). */
    UPROPERTY()
    bool bSeeded = false;

    UPROPERTY()
    int32 Seed = 0;
};

// ============================================================================
// Gaze — FInoGazeConfig / FInoGazeResult / CalculateGaze
// ============================================================================
//
// Merged eye + head tracking with a unified attention state machine.
// Replaces the older separate CalculateEyeLookWeights and
// CalculateHeadLookRotation.
//
// Three attention states drive natural glance-away behaviour:
//
//   Tracking       both eyes and head aim at the target. bApplyEyes and
//                  bApplyHead are both true.
//   Eye-only glance  eyes dart off briefly while the head stays aimed at
//                  the target. bApplyEyes=false, bApplyHead=true. This
//                  is the most common way humans break eye contact (~50%
//                  of breaks) — short (~0.2–0.8 s), subtle, head doesn't
//                  move.
//   Full disengage   both head and eyes look away. bApplyEyes=false and
//                  bApplyHead=false — the caller's base animation drives
//                  both head and eyes. Less common (~20% of breaks),
//                  longer (~0.5–2 s).
//
// The remaining ~30% of "hold ends" rolls into another hold duration
// without breaking — makes the behaviour less metronomic.
// ============================================================================

/** Configuration for the merged gaze function. */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoGazeConfig
{
    GENERATED_BODY()

    // ---- Eye tracking ------------------------------------------------------

    /** Maximum eye deflection from forward, degrees. ARKit blend shapes are
     *  typically calibrated for 35°. Beyond this the eye visibly distorts. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "1.0", ClampMax = "90.0"))
    float MaxEyeAngleDegrees = 35.f;

    /** Interpolation speed for eye angles (units/sec). Eyes are light and
     *  fast — 10-15 feels natural. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float EyeInterpSpeed = 10.f;

    // ---- Head tracking ----------------------------------------------------

    /** Maximum yaw (left-right head rotation), degrees. Human neck maxes
     *  at ~70-80 degrees; beyond that the shoulders + torso have to turn. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "180.0"))
    float MaxHeadYawDegrees = 70.f;

    /** Maximum pitch (up-down head rotation), degrees. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MaxHeadPitchDegrees = 45.f;

    /** Interpolation speed for head rotation (units/sec). Head has more
     *  mass than eyes — 6-10 feels natural. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float HeadInterpSpeed = 8.f;

    /** Dead zone cone around forward (degrees). If the target is within
     *  this cone, the head doesn't rotate (eyes handle small adjustments).
     *  Prevents twitchy micro-motion. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "45.0"))
    float HeadDeadZoneDegrees = 5.f;

    // ---- Attention / glance-away timing -----------------------------------

    /** Minimum seconds to hold gaze on target before possibly breaking. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.1"))
    float MinHoldDuration = 4.f;

    /** Maximum seconds to hold gaze on target. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.1"))
    float MaxHoldDuration = 12.f;

    /** Minimum duration of an eye-only glance (head stays), seconds.
     *  Brief eye darts to scan environment. Real humans: 200-800 ms. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float MinEyeGlanceDuration = 0.2f;

    /** Maximum duration of an eye-only glance, seconds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float MaxEyeGlanceDuration = 0.8f;

    /** Minimum duration of a full disengage (head + eyes both look away),
     *  seconds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float MinFullGlanceDuration = 0.5f;

    /** Maximum duration of a full disengage, seconds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float MaxFullGlanceDuration = 2.f;

    /**
     * After a hold duration completes, probability of transitioning to a
     * brief eye-only glance (head stays aimed at target). Most common
     * real-world break. 0.5 = half the time, 0 = never.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EyeGlanceChance = 0.50f;

    /**
     * After a hold duration completes, probability of transitioning to a
     * full disengage (head + eyes look away). Deeper "looking away" signal
     * than an eye flick. 0.2 = 20% of the time, 0 = never.
     *
     * Note: EyeGlanceChance + FullGlanceChance should sum to <= 1.0. The
     * remainder is the probability of continuing to track without a
     * break (rolling into another hold).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float FullGlanceChance = 0.20f;
};

/**
 * Result of CalculateGaze. Drive your character's eye blend shapes
 * from the Eye*L/R fields, and the head bone from HeadLookRotation.
 * Feed the whole struct back next frame as PreviousResult.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoGazeResult
{
    GENERATED_BODY()

    // ---- Eye blend shape weights (for ARKit-style rigs) --------------------
    //
    // Each weight is in [0, 1]. For a given eye only one horizontal
    // (Left/Right) and one vertical (Up/Down) weight will be non-zero
    // at a time — the opposite direction is clamped to 0.

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookUpL = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookDownL = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookLeftL = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookRightL = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookUpR = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookDownR = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookLeftR = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeLookRightR = 0.f;

    // ---- Head rotation ---------------------------------------------------

    /**
     * Local-space head rotation (Pitch/Yaw in degrees, Roll always 0).
     * Apply as an Add-To-Existing Component-Space bone rotation on the
     * head bone in your AnimBP.
     */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    FRotator HeadLookRotation = FRotator::ZeroRotator;

    // ---- Apply flags (the reason we merged the two functions) ------------

    /**
     * True if the caller should apply the computed eye weights this
     * frame; false to let the animation drive the eyes (character is
     * mid-glance or mid-disengage). Simple hard on/off — for a smooth
     * crossfade use EyeBlendWeight instead.
     */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    bool bApplyEyes = true;

    /**
     * True if the caller should apply HeadLookRotation this frame; false
     * to let the animation drive the head (character is in full disengage
     * or target is dead-centre). Head stays applied during eye-only
     * glances — only a full disengage turns this off.
     */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    bool bApplyHead = true;

    /** Smooth 0-1 version of bApplyEyes (ramps over ~150 ms on flips). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float EyeBlendWeight = 1.f;

    /** Smooth 0-1 version of bApplyHead. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float HeadBlendWeight = 1.f;

    // ---- Secondary / diagnostic outputs ---------------------------------

    /** How far the head is currently turned, normalised [0, 1]. Useful
     *  for layering secondary animation (shoulder turn, spine twist). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float HeadMagnitude = 0.f;

    /** True if the target is inside the head's dead-zone cone. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    bool bInHeadDeadZone = false;

    // ---- Smoothed angles (feed back) -----------------------------------

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float LeftEyeYaw = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float LeftEyePitch = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float RightEyeYaw = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float RightEyePitch = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float HeadYaw = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float HeadPitch = 0.f;

    // ---- Internal state (feed back via PreviousResult, don't modify) ----

    /** 0 = tracking, 1 = eye-only glance, 2 = full disengage. */
    UPROPERTY()
    uint8 GlanceState = 0;

    UPROPERTY()
    float GlanceTimer = 0.f;

    UPROPERTY()
    float NextGlanceChangeTime = 0.f;

    UPROPERTY()
    bool bSeeded = false;

    UPROPERTY()
    int32 Seed = 0;
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
     * Procedural eye blink simulation.
     *
     * See the `FInoBlinkConfig` / `FInoBlinkState` docstrings above for
     * behaviour detail. In short: call every frame, feed the state back
     * next frame, drive your eyeBlink blend shapes from BlinkWeight.
     *
     * SpeakingIntensity (0..1) compresses blink intervals while the
     * character is speaking — humans blink ~60% more often during speech.
     * Default 0.0 = idle/silent behaviour.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Animation",
              meta = (DisplayName = "Calculate Blink Weight"))
    static FInoBlinkState CalculateBlinkWeight(
        float DeltaTime,
        const FInoBlinkState& PreviousState,
        const FInoBlinkConfig& Config,
        UPARAM(meta = (ClampMin = "0.0", ClampMax = "1.0"))
        float SpeakingIntensity = 0.0f);

    /**
     * Merged eye + head look-at with unified attention state machine.
     *
     * Computes per-eye blend shape weights AND a head rotation to aim
     * at a world-space target. A three-state attention machine drives
     * natural "look at target → occasional glance away → return"
     * behaviour:
     *
     *   Tracking          both eyes + head aim at target
     *                     → bApplyEyes=true, bApplyHead=true
     *   Eye-only glance   eyes dart off briefly, head stays on target
     *                     → bApplyEyes=false, bApplyHead=true
     *   Full disengage    head + eyes both look away
     *                     → bApplyEyes=false, bApplyHead=false
     *
     * On either glance state, the smoothed internal angles interpolate
     * back toward neutral so re-engaging the target doesn't snap.
     *
     * @param LookAtTarget       World-space point to aim at.
     * @param HeadWorldLocation  World position of the head bone / socket.
     * @param LeftEyeWorldPos    World position of the left eye socket.
     * @param RightEyeWorldPos   World position of the right eye socket.
     * @param HeadForwardVector  Character's forward in world space. Prefer
     *                           Mesh->GetForwardVector() over the head
     *                           bone's socket forward (MetaHuman head
     *                           bones have non-obvious local axes).
     * @param HeadUpVector       Character's up in world space. Usually
     *                           FVector::UpVector for upright characters.
     * @param DeltaTime          Frame delta seconds.
     * @param PreviousResult     Last frame's output — feed back whole.
     * @param Config             Tuning knobs.
     * @return                   Struct with eye weights, head rotation,
     *                           bApplyEyes/bApplyHead flags, etc.
     *
     * ApplyBeBlueprint wiring (Component Space, Add To Existing):
     *   - Eyes: drive blend shapes only when bApplyEyes=true (or use
     *           EyeBlendWeight to crossfade).
     *   - Head: apply HeadLookRotation only when bApplyHead=true (or
     *           Lerp via HeadBlendWeight for smooth transitions).
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Animation",
              meta = (DisplayName = "Calculate Gaze"))
    static FInoGazeResult CalculateGaze(
        FVector LookAtTarget,
        FVector HeadWorldLocation,
        FVector LeftEyeWorldPos,
        FVector RightEyeWorldPos,
        FVector HeadForwardVector,
        FVector HeadUpVector,
        float   DeltaTime,
        const FInoGazeResult& PreviousResult,
        const FInoGazeConfig& Config);
};
