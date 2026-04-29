// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.StreamTest (milestone C)
// ============================================================================
//
// Phase-1 smoke test for the streaming generation path with thread
// marshaling. Unlike every earlier smoke test, this one does NOT freeze the
// editor — the console command returns immediately, chunks trickle into the
// Output Log over the next few seconds, and the final chunk triggers
// cleanup.
//
// Uses litert_lm_session_generate_content_stream (raw completion, no chat
// template) for simplicity and because the purpose of this milestone is
// streaming + threading, not instruction following.
//
// Threading model:
//   1. RunStreamSmokeTest allocates an FInoAgentsStreamTestState on the
//      heap, builds the engine/session, and kicks off
//      generate_content_stream. The state owns the engine + session + the
//      UTF-8 prompt buffer, so LiteRT-LM can safely read them from its
//      worker thread.
//   2. LiteRT-LM's worker thread calls OnStreamChunk() for each chunk.
//      That callback MUST NOT touch any UObject or any field of State
//      from the worker thread. Its only job is to copy `chunk` and
//      `error_msg` into FStrings (which own their storage) and enqueue a
//      lambda on the game thread via AsyncTask().
//   3. On the game thread, the lambda accumulates chunks, logs them, and
//      on the final chunk destroys the engine/session/state.
//
// Lifetime:
//   - State is created in RunStreamSmokeTest.
//   - If any setup step fails BEFORE generate_content_stream returns success,
//     State is cleaned up synchronously.
//   - If generate_content_stream returns success, the stream owns State.
//     It is destroyed only by the final-chunk game thread lambda (is_final
//     true or error_msg non-null). The very last statement of the
//     destroying lambda is `delete State`. No other code may touch *State
//     after that.
//
// Invoke:
//     Ino.StreamTest
//     Ino.StreamTest Once upon a time in a land far far away
// ============================================================================

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "litert/lm/engine.h"

namespace
{
    /**
     * Heap-allocated state shared between the console command (setup) and
     * the final-chunk game-thread lambda (teardown). All non-const fields
     * are touched only from the game thread after construction.
     */
    struct FInoAgentsStreamTestState
    {
        double TStreamStart = 0.0;
        int    NumChunks    = 0;
        FString Accumulated;

        /**
         * UTF-8 prompt buffer, heap-allocated so its storage outlives the
         * console command that kicked off the stream.
         */
        FTCHARToUTF8* PromptUtf8 = nullptr;

        /** LiteRT-LM resources, destroyed in reverse order of creation. */
        LiteRtLmEngineSettings* Settings = nullptr;
        LiteRtLmEngine*         Engine   = nullptr;
        LiteRtLmSession*        Session  = nullptr;
    };

    void DeleteStreamTestState(FInoAgentsStreamTestState* State)
    {
        if (State == nullptr)
        {
            return;
        }
        if (State->Session != nullptr)
        {
            litert_lm_session_delete(State->Session);
            State->Session = nullptr;
        }
        if (State->Engine != nullptr)
        {
            litert_lm_engine_delete(State->Engine);
            State->Engine = nullptr;
        }
        if (State->Settings != nullptr)
        {
            litert_lm_engine_settings_delete(State->Settings);
            State->Settings = nullptr;
        }
        if (State->PromptUtf8 != nullptr)
        {
            delete State->PromptUtf8;
            State->PromptUtf8 = nullptr;
        }
        delete State;
    }

    /**
     * C callback — runs on LiteRT-LM's internal worker thread. MUST NOT
     * touch any UObject, UE global, or field of State. Copies the chunk
     * and error message into FStrings, captures them by value into a
     * lambda, and marshals the lambda to the game thread via AsyncTask.
     */
    void OnStreamChunk(void* callback_data,
                       const char* chunk,
                       bool is_final,
                       const char* error_msg)
    {
        if (callback_data == nullptr)
        {
            return;
        }
        auto* const State = static_cast<FInoAgentsStreamTestState*>(callback_data);

        // `chunk` and `error_msg` are valid only for the duration of this
        // call — copy their contents now, capture the FStrings into the
        // lambda.
        const FString ChunkStr = (chunk     != nullptr) ? FString(UTF8_TO_TCHAR(chunk))     : FString();
        const FString ErrorStr = (error_msg != nullptr) ? FString(UTF8_TO_TCHAR(error_msg)) : FString();
        const double  NowTime  = FPlatformTime::Seconds();

        AsyncTask(ENamedThreads::GameThread,
            [State, ChunkStr, ErrorStr, is_final, NowTime]()
            {
                const bool bHasError = !ErrorStr.IsEmpty();

                if (bHasError)
                {
                    UE_LOG(LogInoAgents, Error, TEXT("StreamTest: error: %s"), *ErrorStr);
                }

                if (!ChunkStr.IsEmpty())
                {
                    State->NumChunks += 1;
                    State->Accumulated += ChunkStr;
                    UE_LOG(LogInoAgents, Log,
                           TEXT("StreamTest: chunk %3d (+%.3f s)  \"%s\""),
                           State->NumChunks, NowTime - State->TStreamStart, *ChunkStr);
                }

                if (is_final || bHasError)
                {
                    const double Elapsed = NowTime - State->TStreamStart;
                    const double TokPerSec = (Elapsed > 0.0 && State->NumChunks > 0)
                        ? (static_cast<double>(State->NumChunks) / Elapsed)
                        : 0.0;
                    UE_LOG(LogInoAgents, Log,
                           TEXT("StreamTest: DONE in %.2f s (%d chunks, ~%.1f chunks/sec)"),
                           Elapsed, State->NumChunks, TokPerSec);
                    UE_LOG(LogInoAgents, Log,
                           TEXT("StreamTest: full accumulated text: \"%s\""),
                           *State->Accumulated);

                    // State is owned by this lambda from here on. Destroy
                    // resources in reverse order of construction and free
                    // the state itself. Nothing may touch *State after this.
                    DeleteStreamTestState(State);
                }
            });
    }
}

static void RunStreamSmokeTest(const TArray<FString>& Args)
{
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("Once upon a time, in a land far far away,"));

    const FString ModelPath = InoSmokeTest::ResolveDefaultModelPath();
    if (ModelPath.IsEmpty())
    {
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("StreamTest: starting (NON-BLOCKING — editor stays responsive)"));
    UE_LOG(LogInoAgents, Log, TEXT("  Prompt: \"%s\""), *Prompt);
    UE_LOG(LogInoAgents, Log, TEXT("  Chunks will arrive asynchronously in the Output Log over the next few seconds."));

    // --- Allocate state ---
    auto* State = new FInoAgentsStreamTestState();
    State->PromptUtf8 = new FTCHARToUTF8(*Prompt);

    const double TSetupStart = FPlatformTime::Seconds();

    // --- Load engine ---
    const FTCHARToUTF8 ModelPathUtf8(*ModelPath);
    State->Settings = litert_lm_engine_settings_create(
        ModelPathUtf8.Get(), "cpu", nullptr, nullptr);
    if (State->Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("StreamTest: engine_settings_create returned NULL"));
        DeleteStreamTestState(State);
        return;
    }

    State->Engine = litert_lm_engine_create(State->Settings);
    if (State->Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("StreamTest: engine_create returned NULL"));
        DeleteStreamTestState(State);
        return;
    }

    State->Session = litert_lm_engine_create_session(State->Engine, /*config=*/ nullptr);
    if (State->Session == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("StreamTest: engine_create_session returned NULL"));
        DeleteStreamTestState(State);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamTest: engine + session ready in %.2f s — kicking off stream"),
           FPlatformTime::Seconds() - TSetupStart);

    // --- Build LiteRtLmInputData ---
    // LiteRtLmInputData itself is a plain POD struct; generate_content_stream
    // reads its fields synchronously during tokenization, so passing a
    // stack-local is safe. The `.data` pointer must be valid for the duration
    // of the call — we route it through State->PromptUtf8 which lives on the
    // heap.
    LiteRtLmInputData TextInput;
    TextInput.type = kLiteRtLmInputDataTypeText;
    TextInput.data = State->PromptUtf8->Get();
    TextInput.size = static_cast<size_t>(State->PromptUtf8->Length());

    // Mark the start of "streaming time" so chunk timestamps in the callback
    // lambda are relative to when generation actually started, not when the
    // engine was created.
    State->TStreamStart = FPlatformTime::Seconds();

    // --- Kick off the stream ---
    // Returns 0 on success. The callback will fire asynchronously on a
    // worker thread; from that point on, ownership of State belongs to the
    // stream and only the final-chunk lambda may destroy it.
    const int RC = litert_lm_session_generate_content_stream(
        State->Session, &TextInput, /*num_inputs=*/ 1,
        &OnStreamChunk, /*callback_data=*/ State);

    if (RC != 0)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamTest: generate_content_stream returned non-zero (%d) — stream did not start"),
               RC);
        DeleteStreamTestState(State);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamTest: stream accepted (RC=0). Control returning to game thread — waiting for chunks."));
    // Function returns here. Engine/session remain alive inside State.
}

static FAutoConsoleCommand GStreamTestCommand(
    TEXT("Ino.StreamTest"),
    TEXT("Phase-1 smoke test (milestone C): non-blocking streaming. Loads "
         "engine, creates a session, kicks off generate_content_stream with "
         "a worker-thread C callback that marshals each chunk back to the "
         "game thread via AsyncTask. Editor stays responsive — chunks appear "
         "asynchronously in the Output Log. Final chunk destroys the engine/"
         "session. Optional custom prompt: 'Ino.StreamTest Once upon a time'."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamSmokeTest));
