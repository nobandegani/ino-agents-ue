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
 *   Chatterbox    Chatterbox Turbo TTS (4-ORT-session pipeline)
 *   LiteRtLm      Google LiteRT-LM (Gemma 4 inference)
 *   NeuTtsNano    Neuphonic NeuTTS Nano (llama.cpp + NeuCodec)
 *   Onnx          ONNX Runtime infrastructure (generic, not per-model)
 *   LlamaCpp      llama.cpp runtime infrastructure
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
 *   Worker        Any background worker thread (synth worker, conversation
 *                 worker, decoder worker)
 *   Session       An individual ORT / LiteRT session (per-component load,
 *                 single Run call diagnostics)
 *   Runner        Inner-loop pipeline driver (Chatterbox runner, NeuTtsNano
 *                 runner)
 *   Decoder       Chatterbox's parallel conditional_decoder thread
 *   Tokenizer     BPE / SentencePiece tokenizer state + stats
 *   Tool          LiteRtLm tool-call registry + dispatch
 *   Download      HTTP download flow (HEAD probe, chunk GET, rename)
 *   Voice         NeuTtsNano voice registry + encoding
 *   Settings      UDeveloperSettings load / reload
 *   AsyncAction   UBlueprintAsyncActionBase subclasses
 *   SmokeTest     Test commands under Private/SmokeTests/ — prefixed by
 *                 their owning subsystem (e.g. "Chatterbox: SmokeTest: …")
 *
 * Log-level convention:
 *
 *   Log       Important events a developer reading the log should see:
 *             load/unload, synth complete, state transitions, delegate
 *             binding checks, milestone timings. Default verbosity.
 *
 *   Verbose   Noisy per-iteration diagnostics: per-AR-token, per-chunk,
 *             per-HTTP-header, per-sample stats. OFF by default; enable
 *             with console command `log LogInoAgents Verbose` when
 *             diagnosing a specific problem.
 *
 *   Warning   Recoverable issues: optional file 404, fallback triggered,
 *             user-visible suboptimal config (DML on known-broken session,
 *             mixed-group variants). Something the dev should know but
 *             doesn't break anything.
 *
 *   Error     Unrecoverable failures: load failed, kernel missing, SHA
 *             mismatch with no URL, session construction failure. The
 *             operation's terminal callback will fire with bSuccess=false.
 *
 * Grepping the log:
 *
 *   Find everything Chatterbox:            grep " Chatterbox: "
 *   Find everything about ORT sessions:    grep ": Session: "
 *   Find a specific synth attempt:         grep " Chatterbox: Subsystem: SynthesizeAsync"
 *   Find all errors:                       grep "Error:"
 *   Find all downloads:                    grep ": Download: "
 *
 * Examples of well-formed lines:
 *
 *   LogInoAgents: Chatterbox: Subsystem: SynthesizeAsync queued (text_len=121, max_new_tokens=1024)
 *   LogInoAgents: Chatterbox: Runner: encoder done in 187.3 ms (cond_len=145, prompt_len=312)
 *   LogInoAgents: Chatterbox: Runner: AR loop iter 42/1024 (hit STOP: no, elapsed=3621 ms)
 *   LogInoAgents: LiteRtLm: Conversation: stream round 2 started (4 tool results pending)
 *   LogInoAgents: Onnx: Session: registered provider DirectML (adapter=0)
 *   LogInoAgents: NeuTtsNano: Runner: llama_decode prefill complete (ctx=653 tokens, 318.4 ms)
 *
 * When adding new logs: err on the side of more, not less. Runtime
 * cost of UE_LOG is nil when the verbosity is below the active level.
 */
INOAGENTS_API DECLARE_LOG_CATEGORY_EXTERN(LogInoAgents, Log, All);
