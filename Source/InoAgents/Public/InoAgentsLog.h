// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Logging/LogMacros.h"

/**
 * Shared log category for the InoAgents runtime module.
 *
 * Declared EXTERN here so every TU in the module can log under the same
 * category name. The single definition lives in InoAgents.cpp via
 * DEFINE_LOG_CATEGORY(LogInoAgents).
 *
 * This header is Private because LogInoAgents is an implementation detail.
 * External modules should not tie themselves to our log category — they
 * should use their own categories or the global LogTemp.
 *
 * ============================================================================
 *  Log-message convention (please follow when adding new log lines)
 * ============================================================================
 *
 * Every line under LogInoAgents follows this shape:
 *
 *   <Subsystem>: <Component>: <free-form message>
 *
 * where <Subsystem> is one of:
 *
 *   LiteRtLm      Google LiteRT-LM (Gemma 4 inference)
 *   ElevenLabs    ElevenLabs cloud TTS
 *   Audio         InoAudioFunctionLibrary helpers
 *   Anim          InoAnimationBlueprintHelper helpers
 *   UI            Slate chat panel + bridge
 *
 * and <Component> is the sub-area within that subsystem. Common ones:
 *
 *   Subsystem     The UInoXxxSubsystem UObject itself (lifecycle, top-level
 *                 public API entry/exit, state transitions)
 *   Module        DLL/.so init, symbol resolution, module startup/shutdown
 *   Conversation  LiteRtLm UInoLiteRtLmConversation UObject
 *   Worker        Any background worker thread (conversation worker, etc.)
 *   Tool          LiteRtLm tool-call registry + dispatch
 *   Download      HTTP download flow (HEAD probe, chunk GET, rename)
 *   Settings      UDeveloperSettings load / reload
 *   AsyncAction   UBlueprintAsyncActionBase subclasses
 *   SmokeTest     Test commands under Private/SmokeTests/ — prefixed by
 *                 their owning subsystem (e.g. "ElevenLabs: SmokeTest: …")
 *
 * Log-level convention:
 *
 *   Log       Important events a developer reading the log should see:
 *             load/unload, state transitions, delegate binding checks,
 *             milestone timings. Default verbosity.
 *
 *   Verbose   Noisy per-iteration diagnostics: per-token, per-chunk,
 *             per-HTTP-header, per-sample stats. OFF by default; enable
 *             with console command `log LogInoAgents Verbose` when
 *             diagnosing a specific problem.
 *
 *   Warning   Recoverable issues: optional file 404, fallback triggered,
 *             user-visible suboptimal config. Something the dev should
 *             know but doesn't break anything.
 *
 *   Error     Unrecoverable failures: load failed, SHA mismatch with no
 *             URL, session construction failure. The operation's terminal
 *             callback will fire with bSuccess=false.
 *
 * Grepping the log:
 *
 *   Find everything LiteRtLm:              grep " LiteRtLm: "
 *   Find a specific conversation round:    grep " LiteRtLm: Conversation: "
 *   Find all errors:                       grep "Error:"
 *   Find all downloads:                    grep ": Download: "
 *
 * Examples of well-formed lines:
 *
 *   LogInoAgents: LiteRtLm: Subsystem: LoadModelAsync queued (path=...)
 *   LogInoAgents: LiteRtLm: Conversation: stream round 2 started (4 tool results pending)
 *   LogInoAgents: ElevenLabs: AsyncAction: TextToDialogueStream begin (voice_id=...)
 *
 * When adding new logs: err on the side of more, not less. Runtime
 * cost of UE_LOG is nil when the verbosity is below the active level.
 */
INOAGENTS_API DECLARE_LOG_CATEGORY_EXTERN(LogInoAgents, Log, All);
