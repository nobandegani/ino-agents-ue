// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "InoGyroCameraSwayComponent.generated.h"

/**
 * Scene component that applies a gentle gyro/accelerometer-driven rotation
 * offset to itself, so any children (a camera, a camera rig, an overlay)
 * inherit a subtle "breathing" sway in response to how the user is holding
 * their device.
 *
 * Typical hierarchy:
 *
 *     CapsuleComponent (pawn root)
 *       └── SpringArm (or a fixed offset scene component)
 *           └── UInoGyroCameraSwayComponent   ← this
 *               └── CameraComponent           ← inherits the sway automatically
 *
 * The sway is driven by APlayerController::GetInputMotionState's Tilt
 * vector — a filtered "which way is the device oriented relative to
 * gravity" estimate. Tilt is much more stable than the raw gyro
 * RotationRate for this purpose; RotationRate drifts and reads as
 * twitchy when the user is holding the phone still.
 *
 * On a desktop host without a motion sensor, GetInputMotionState returns
 * zero, so the component is a clean no-op — safe to leave in for
 * cross-platform builds without any #if PLATFORM_* guard.
 *
 * Rest-pose calibration: the first non-zero Tilt sample observed at
 * runtime is latched as "rest" and every subsequent sample is measured
 * against it, so the camera starts centred no matter how the user is
 * holding the phone at launch. If RestDriftTimeConstant > 0, the rest
 * pose low-passes toward the current tilt so the effect re-centres when
 * the user changes posture (sitting → lying on couch → etc.) without
 * requiring an explicit recalibration call.
 *
 * All work happens in TickComponent on the game thread — cheap enough
 * to leave ticking every frame (a few floating-point ops + one
 * SetRelativeRotation).
 */
UCLASS(ClassGroup=(Ino), meta=(BlueprintSpawnableComponent),
       DisplayName="Ino Gyro Camera Sway")
class INOAGENTS_API UInoGyroCameraSwayComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UInoGyroCameraSwayComponent();

    // ── Master switch ──────────────────────────────────────────────────
    /** Set to false to disable the effect without removing the component.
        Disabling smoothly returns to the neutral pose (no snap). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway")
    bool bEnabled = true;

    // ── Per-axis strength ──────────────────────────────────────────────
    /** Degrees of camera pitch per unit of device tilt-X. Negate to invert.
        Default tuned for a natural, subtle response on typical mobile tilt
        magnitudes; if the effect feels too strong or too weak, this is
        the first knob to tune. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Strength",
              meta=(ClampMin="-30", ClampMax="30"))
    float PitchStrength = 10.0f;

    /** Degrees of camera yaw per unit of device tilt-Y (device roll maps to
        camera yaw — rolling the phone in-hand swings the camera horizontally,
        which reads as natural "peek" motion). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Strength",
              meta=(ClampMin="-30", ClampMax="30"))
    float YawStrength = 10.0f;

    /** Degrees of camera roll per unit of device tilt-Z. Default 0 —
        rolling the camera around its forward axis reads as drunk /
        cinematic rather than parallax. Leave off unless you specifically
        want that look. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Strength",
              meta=(ClampMin="-30", ClampMax="30"))
    float RollStrength = 0.0f;

    // ── Clamps ─────────────────────────────────────────────────────────
    /** Hard max pitch offset (deg). Regardless of how aggressive Strength
        is, the camera never swings past this. Keep small — beyond ~5°
        the effect stops reading as "parallax" and starts reading as
        "broken camera". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Clamps",
              meta=(ClampMin="0", ClampMax="20"))
    float MaxPitchDegrees = 4.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Clamps",
              meta=(ClampMin="0", ClampMax="20"))
    float MaxYawDegrees = 4.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Clamps",
              meta=(ClampMin="0", ClampMax="20"))
    float MaxRollDegrees = 2.0f;

    // ── Smoothing ──────────────────────────────────────────────────────
    /** FInterpTo speed. 1–2 = heavy / cinematic / "expensive-feeling".
        3–4 = natural. 5+ = nervous / too responsive. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Smoothing",
              meta=(ClampMin="0.1", ClampMax="20"))
    float InterpSpeed = 3.0f;

    /** Tilt magnitudes smaller than this (in the same units as the raw
        Tilt vector — radians on most mobile platforms) are treated as
        zero. Kills constant hand-jitter nudges at rest while still
        responding to intentional tilting. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Smoothing",
              meta=(ClampMin="0", ClampMax="0.5"))
    float DeadzoneMagnitude = 0.02f;

    // ── Rest-pose calibration ─────────────────────────────────────────
    /** If true, the first non-zero Tilt sample observed at runtime becomes
        the reference "rest" pose that all subsequent tilts are measured
        against. If false, raw Tilt is used as-is (which means the camera
        will be offset from neutral the moment the app starts, by however
        the user happens to be holding the phone). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Calibration")
    bool bAutoCalibrate = true;

    /** Time constant (seconds) for the rest pose to drift toward the
        current tilt via a first-order low-pass. 0 = off (rest pose is
        frozen at the first sample forever). ~30 seconds is a nice
        default: if the user shifts from sitting to lying down, the
        camera re-centres over ~half a minute instead of being
        permanently rotated or requiring an explicit recenter gesture. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ino|GyroSway|Calibration",
              meta=(ClampMin="0", ClampMax="120"))
    float RestDriftTimeConstant = 30.0f;

    // ── Runtime API ────────────────────────────────────────────────────
    /** Manually recapture the current device tilt as the "rest" reference.
        Useful after a UI-initiated recenter button, a level reload, or
        any moment where the user's posture has changed and you want an
        immediate (rather than drift-based) reset. */
    UFUNCTION(BlueprintCallable, Category="Ino|GyroSway")
    void Recalibrate();

    /** Read-only accessor for the currently-applied offset (after
        smoothing + clamp). Useful for debug overlays. */
    UFUNCTION(BlueprintPure, Category="Ino|GyroSway")
    FRotator GetCurrentOffset() const { return CurrentOffset; }

protected:
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

private:
    /** Relative rotation captured at BeginPlay. All sway is added on top
        of this so the component respects whatever pose you authored in
        the editor. */
    FRotator BaseRelativeRotation = FRotator::ZeroRotator;

    /** Currently-applied offset (after smoothing + clamp). */
    FRotator CurrentOffset = FRotator::ZeroRotator;

    /** Reference tilt — first valid sample after BeginPlay, optionally
        drifting via the rest-drift low-pass. */
    FVector RestTilt = FVector::ZeroVector;
    bool bHasRestTilt = false;

    /** Reads tilt from the local player controller. Returns false if
        there isn't one (e.g. editor viewport without PIE, or before
        the pawn is possessed). */
    bool TryReadTilt(FVector& OutTilt) const;
};
