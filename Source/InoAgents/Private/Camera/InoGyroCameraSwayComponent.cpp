// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Camera/InoGyroCameraSwayComponent.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

UInoGyroCameraSwayComponent::UInoGyroCameraSwayComponent()
{
    PrimaryComponentTick.bCanEverTick = true;

    // Tick after physics so whatever moved the camera base this frame
    // (spring arm lag, cinematic sequencer, pawn move component, etc.)
    // is already settled before we lay our sway offset on top. TG_PostPhysics
    // runs before rendering but after most gameplay logic — the natural slot.
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UInoGyroCameraSwayComponent::BeginPlay()
{
    Super::BeginPlay();

    // Snapshot the designer-authored pose so every frame's sway is layered
    // on top of this base. Without this, the first SetRelativeRotation
    // would wipe out any non-identity rotation the designer set in the
    // editor (e.g. a slight downward tilt on a third-person camera).
    BaseRelativeRotation = GetRelativeRotation();

    CurrentOffset = FRotator::ZeroRotator;
    bHasRestTilt  = false;
}

void UInoGyroCameraSwayComponent::Recalibrate()
{
    // Drop the latch — next TickComponent will capture a fresh rest sample
    // from whatever the device is currently reporting.
    bHasRestTilt = false;
    RestTilt     = FVector::ZeroVector;
}

bool UInoGyroCameraSwayComponent::TryReadTilt(FVector& OutTilt) const
{
    const UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    // First PC is the local player's controller in all single-player
    // flows. Split-screen / networked multiplayer would want the
    // controller that owns *this* component's pawn, but the demo is
    // single-player and this keeps the component drop-in.
    APlayerController* PC = World->GetFirstPlayerController();
    if (!PC)
    {
        return false;
    }

    FVector Tilt, RotationRate, Gravity, Acceleration;
    PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);
    OutTilt = Tilt;
    return true;
}

void UInoGyroCameraSwayComponent::TickComponent(
    float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // Early-out: effect disabled. Smoothly return to neutral so flipping
    // bEnabled mid-play doesn't pop the camera.
    if (!bEnabled)
    {
        CurrentOffset = FMath::RInterpTo(CurrentOffset, FRotator::ZeroRotator,
                                         DeltaTime, InterpSpeed);
        SetRelativeRotation(BaseRelativeRotation + CurrentOffset);
        return;
    }

    FVector Tilt;
    if (!TryReadTilt(Tilt))
    {
        // Pre-possession or no PC (editor viewport). Hold whatever offset
        // we had last frame — popping to zero would be worse.
        return;
    }

    // ── Rest-pose calibration ──────────────────────────────────────────
    // First non-zero sample latches as the reference. The IsNearlyZero check
    // matters: on some platforms GetInputMotionState briefly returns (0,0,0)
    // for the first frame or two after startup, and latching that as "rest"
    // would leave every subsequent sample offset by the user's actual hold
    // angle — effectively disabling auto-calibration.
    if (bAutoCalibrate)
    {
        if (!bHasRestTilt && !Tilt.IsNearlyZero())
        {
            RestTilt     = Tilt;
            bHasRestTilt = true;
        }
        else if (bHasRestTilt && RestDriftTimeConstant > KINDA_SMALL_NUMBER)
        {
            // First-order low-pass: RestTilt moves toward Tilt with time
            // constant RestDriftTimeConstant. The Clamp guards against
            // Alpha > 1 on a very long DeltaTime (e.g. debugger pause
            // resume) which would cause overshoot.
            const float Alpha = FMath::Clamp(DeltaTime / RestDriftTimeConstant,
                                             0.0f, 1.0f);
            RestTilt = FMath::Lerp(RestTilt, Tilt, Alpha);
        }
    }

    // Offset relative to rest. If calibration is off, raw Tilt is used
    // directly (which means the camera will be rotated by whatever the
    // user's current hold angle is — rarely what you want, but exposed
    // for completeness).
    FVector Effective = Tilt - (bAutoCalibrate ? RestTilt : FVector::ZeroVector);

    // Deadzone on the input side — sub-threshold quivers read as zero.
    // Squared comparison avoids a sqrt every frame.
    if (Effective.SizeSquared() < DeadzoneMagnitude * DeadzoneMagnitude)
    {
        Effective = FVector::ZeroVector;
    }

    // ── Tilt axes → camera rotation axes ───────────────────────────────
    // Tilt convention on mobile:
    //   Tilt.X ≈ device pitch (tilting the top of the phone toward/away from you)
    //   Tilt.Y ≈ device roll  (rocking the phone left/right in the hand)
    //   Tilt.Z ≈ device yaw   (spinning around the vertical axis — rare / unreliable)
    //
    // Natural mapping: device pitch → camera pitch, device roll → camera
    // yaw (rocking the phone feels like peeking sideways), device yaw →
    // camera roll (almost never enabled; see RollStrength comment).
    FRotator TargetOffset(
        Effective.X * PitchStrength,   // Pitch
        Effective.Y * YawStrength,     // Yaw
        Effective.Z * RollStrength);   // Roll

    // Per-axis clamp. Runs after the strength multiply so the limits are
    // in camera-space degrees (not tilt-space units) — what the designer
    // actually wants to reason about.
    TargetOffset.Pitch = FMath::Clamp(TargetOffset.Pitch, -MaxPitchDegrees, MaxPitchDegrees);
    TargetOffset.Yaw   = FMath::Clamp(TargetOffset.Yaw,   -MaxYawDegrees,   MaxYawDegrees);
    TargetOffset.Roll  = FMath::Clamp(TargetOffset.Roll,  -MaxRollDegrees,  MaxRollDegrees);

    // Smooth toward target. RInterpTo is frame-rate independent (uses
    // DeltaTime + speed), so the "feel" stays consistent across 30 / 60 / 120 Hz.
    CurrentOffset = FMath::RInterpTo(CurrentOffset, TargetOffset, DeltaTime, InterpSpeed);

    // Apply as base + offset. Because this component is a *parent* of the
    // camera in the scene hierarchy, the camera inherits this rotation
    // automatically — we never touch the camera component directly. That's
    // the whole point of the "sway pivot" pattern: clean ownership, no
    // reach-across-siblings coupling.
    //
    // Note on composition: FRotator + FRotator is component-wise addition,
    // not quaternion composition. For the small angles we clamp to
    // (≤ ~4° per axis), this is a perfectly good approximation of "lay
    // this small offset on top of the base". Using FQuat composition
    // would be more correct for large rotations but adds nothing here.
    SetRelativeRotation(BaseRelativeRotation + CurrentOffset);
}
