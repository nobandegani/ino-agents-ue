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
 *
 * Simulates natural human blinking:
 *   - Random interval between blinks (~2–6 s)
 *   - Fast close (~75 ms), brief hold (~50 ms), slower open (~150 ms)
 *   - Occasional double blinks
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
// Head-look (CalculateHeadLookRotation)
// ============================================================================

/**
 * Configuration for the head-look rotation function.
 *
 * Tune to taste per character. The defaults describe a generic attentive
 * NPC engaged in conversation — holds eye contact in bursts with occasional
 * brief glance-aways. Change GlanceAwayChance + hold-duration bounds to
 * shift personality:
 *
 *   - Focused / stoic (assassin, interrogator): GlanceAwayChance ~ 0.2,
 *       MinHoldDuration ~ 8.
 *   - Default NPC (engaged, social): keep defaults.
 *   - Shy / nervous: GlanceAwayChance ~ 0.85, MinHoldDuration ~ 2.
 *   - Disengaged / distracted: GlanceAwayChance ~ 0.95, MinHoldDuration ~ 1.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoHeadLookConfig
{
    GENERATED_BODY()

    // ---- Rotation limits ---------------------------------------------------

    /** Maximum yaw (left-right head rotation), degrees. Human neck maxes
     *  at ~70-80 degrees; beyond that the shoulders + torso have to turn. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "180.0"))
    float MaxYawDegrees = 70.f;

    /** Maximum pitch (up-down head rotation), degrees. Vertical neck range
     *  is smaller than horizontal — ~45 degrees realistic, 60 for stylized. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MaxPitchDegrees = 45.f;

    /** Interpolation speed (units/sec). Higher = faster tracking. Head is
     *  heavier than eyes (eye default is 10), so 6-10 feels natural. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float InterpSpeed = 8.f;

    /** Dead zone cone around the forward direction (degrees). Targets
     *  within this cone do not trigger head rotation — real humans don't
     *  micro-adjust head position for things straight ahead; the eyes
     *  handle that. 3-8 is a good range. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "45.0"))
    float DeadZoneDegrees = 5.f;

    // ---- Glance-away behaviour --------------------------------------------

    /** Minimum seconds to hold gaze on target before possibly glancing away.
     *  Real humans hold eye contact in 3-10 s bursts. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.1"))
    float MinHoldDuration = 4.f;

    /** Maximum seconds to hold gaze on target. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.1"))
    float MaxHoldDuration = 12.f;

    /** Minimum seconds a single glance-away lasts. Real glances are brief
     *  — 300 ms to 2 s typical. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float MinGlanceAwayDuration = 0.5f;

    /** Maximum seconds a single glance-away lasts. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0"))
    float MaxGlanceAwayDuration = 2.f;

    /** Probability that a completed hold-duration transitions into a
     *  glance-away instead of rolling into another hold (0-1).
     *    0.0 = always tracks, never looks away (creepy locked-on gaze).
     *    0.6 = default attentive NPC — glances sometimes, looks mostly.
     *    1.0 = glances away after every hold (twitchy / anxious character). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Animation",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float GlanceAwayChance = 0.6f;
};

/**
 * Result of CalculateHeadLookRotation. Drive your head bone from
 * HeadLookRotation (when applicable), feed the whole struct back next
 * frame as PreviousResult for smooth interpolation + glance state.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoHeadLookResult
{
    GENERATED_BODY()

    // ---- Outputs the caller uses ------------------------------------------

    /**
     * Target head rotation, local-space (relative to the character's
     * neutral head orientation). Roll is always 0; only Yaw and Pitch
     * are used. Typical use: additive aim-offset or a manual bone
     * rotation applied on top of the neck's animation rotation.
     */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    FRotator HeadLookRotation = FRotator::ZeroRotator;

    /**
     * Simple on/off flag: TRUE  → use HeadLookRotation this frame.
     *                     FALSE → ignore HeadLookRotation; let the
     *                             character's base animation drive
     *                             the head (the character is briefly
     *                             glancing away OR the target is in
     *                             the dead zone).
     *
     * If you want smooth transitions between these states rather than
     * a hard cut, use BlendWeight instead (see below).
     */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    bool bShouldApply = true;

    /**
     * Smoothed 0-1 version of bShouldApply. Ramps over ~150 ms when the
     * glance state flips. Use for a crossfade:
     *   FinalRotation = Lerp(AnimRotation, HeadLookRotation, BlendWeight)
     * which hides the small pop you'd see with a hard bShouldApply branch.
     */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float BlendWeight = 1.f;

    /** How far the head is turned, normalised [0, 1] against the max
     *  angle. Useful for driving secondary animation (shoulder turn,
     *  spine twist) at fractional values of head rotation. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float Magnitude = 0.f;

    /** True if the target was inside the dead-zone cone (head stays put). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    bool bInDeadZone = false;

    // ---- Smoothed angles (also feed back) --------------------------------

    /** Smoothed yaw angle, degrees. Left-negative, right-positive. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float Yaw = 0.f;

    /** Smoothed pitch angle, degrees. Up-positive, down-negative. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Animation")
    float Pitch = 0.f;

    // ---- Internal state (feed back via PreviousResult, don't modify) ----

    UPROPERTY()
    float GlanceTimer = 0.f;

    UPROPERTY()
    float NextGlanceChangeTime = 0.f;

    UPROPERTY()
    bool bIsGlancingAway = false;

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
     * Calculate ARKit-style eye look blend shape weights from a
     * world-space look-at target, with frame-rate-independent smoothing.
     *
     * Interpolation happens in angle space (yaw / pitch per eye),
     * then the smoothed angles are converted to blend shape weights.
     * This avoids jitter at direction crossings.
     *
     * Pass the previous frame's output as PreviousWeights. On the
     * first frame, pass a default-constructed (zeroed) FInoEyeLookWeights.
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

    /**
     * Procedural eye blink simulation.
     *
     * Call every frame, feed the output back as PreviousState. Produces
     * a natural blink pattern with several layers of realism:
     *
     *   - Random inter-blink intervals in the configured range, with an
     *     occasional long "stare" pause (log-normal-like distribution).
     *     Breaks the "metronome" feel that pure uniform sampling has
     *     over long observation periods.
     *   - Asymmetric eyelid motion: fast cubic-ease-in close (gravity-
     *     assisted), brief hold, slower quadratic-ease-out reopen
     *     (muscle-driven). Matches real eyelid kinematics.
     *   - Occasional double blinks (configurable chance). Real humans
     *     do this unconsciously every so often.
     *   - Speaking-rate modulation: humans blink ~60% more often while
     *     talking than while silent. Drive SpeakingIntensity from your
     *     TTS / voice-activity signal to have the character's blink
     *     rate breathe with its speech.
     *   - Framerate-independent time consumption: even at 30 fps with
     *     a 33 ms frame, short phases (hold = 30–70 ms) transition
     *     correctly without losing or double-counting time — internally
     *     the state machine drains DeltaTime across as many phase
     *     transitions as it needs to.
     *
     * @param DeltaTime          Frame delta time (seconds).
     * @param PreviousState      Output from the previous frame.
     * @param Config             Timing + distribution parameters.
     * @param SpeakingIntensity  0.0 = idle/silent (configured intervals),
     *                           1.0 = actively talking (intervals
     *                           compressed to ~60% of configured range).
     *                           Typical game signal: set to your TTS
     *                           amplitude envelope or a simple
     *                           "isSpeaking ? 1.0 : 0.0" boolean.
     *                           Takes effect on the next interval pick
     *                           (at end of a blink), not mid-interval.
     * @return                   Updated state with BlinkWeight in [0, 1].
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
     * Calculate head-look rotation toward a world-space target, with
     * frame-rate-independent smoothing, realistic angle clamping, a
     * dead-zone cone for targets directly ahead, and a random
     * glance-away behaviour so the character doesn't hold eye contact
     * mechanically forever.
     *
     * Output is local-space (Yaw/Pitch in degrees relative to the
     * supplied HeadForwardVector). Plug HeadLookRotation into your
     * AnimBP's aim-offset or add it on top of the neck's animation
     * rotation in a post-process slot.
     *
     * Pass the previous frame's result back as PreviousResult. On the
     * first frame, pass a default-constructed FInoHeadLookResult.
     *
     * @param LookAtTarget        World-space point to aim at (usually
     *                            another character's head bone position,
     *                            or the player camera location).
     * @param HeadWorldLocation   World position of this character's head
     *                            socket / bone.
     * @param HeadForwardVector   Neutral-gaze forward direction, world
     *                            space. For upright humanoids, the actor's
     *                            forward vector is usually correct.
     * @param HeadUpVector        Neutral "up" direction, world space.
     *                            FVector::UpVector for upright characters;
     *                            pass the actor's up for tilted states.
     * @param DeltaTime           Frame delta time (seconds).
     * @param PreviousResult      Output from the previous frame — carries
     *                            smoothed angles + glance-away state.
     * @param Config              Rotation limits + glance-away timing.
     * @return                    Rotation to apply + state fields.
     *
     * Using bShouldApply (simple): branch on it. If false, don't override
     * the head rotation at all — the character is in the middle of a
     * glance-away or the target is dead-centre already.
     *
     * Using BlendWeight (smooth): cross-fade between anim and result:
     *     Final = Lerp(AnimRotation, HeadLookRotation, BlendWeight)
     * BlendWeight ramps over ~150 ms when the glance state flips, hiding
     * the pop you'd see with a hard branch.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Animation",
              meta = (DisplayName = "Calculate Head Look Rotation"))
    static FInoHeadLookResult CalculateHeadLookRotation(
        FVector LookAtTarget,
        FVector HeadWorldLocation,
        FVector HeadForwardVector,
        FVector HeadUpVector,
        float   DeltaTime,
        const FInoHeadLookResult& PreviousResult,
        const FInoHeadLookConfig& Config);
};
