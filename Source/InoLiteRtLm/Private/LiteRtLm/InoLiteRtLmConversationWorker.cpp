// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLiteRtLmConversationWorker.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "LiteRtLm/InoLiteRtLmSubsystem.h"
#include "LiteRtLm/InoLiteRtLmToolBase.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"  // EscapeJsonString

#include "litert/lm/engine.h"

#if PLATFORM_WINDOWS
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <io.h>
    #include <fcntl.h>
    #include <stdio.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
    // Safety cap for the agent loop. If the model keeps calling tools
    // without producing final text for this many rounds in a row, we
    // bail with an error rather than spinning forever. The number is
    // pulled out of the air but generous — real interactions rarely
    // need more than 2-3 rounds.
    constexpr int32 kMaxAgentLoopRounds = 8;
}

FInoLiteRtLmConversationWorker::FInoLiteRtLmConversationWorker(
    TWeakObjectPtr<UInoLiteRtLmConversation> InOwner,
    TWeakObjectPtr<UInoLiteRtLmSubsystem>    InSubsystem,
    LiteRtLmConversation* InConversation,
    LiteRtLmConversationConfig* InConversationConfig)
    : WeakOwner(InOwner)
    , WeakSubsystem(InSubsystem)
    , NativeConversation(InConversation)
    , NativeConversationConfig(InConversationConfig)
{
    // QueueEvent is auto-reset: one Trigger wakes the worker exactly
    // once, matching "one enqueue = one wake, one Stop = one wake".
    QueueEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

    // StreamEvent is manual-reset: before each send, the worker thread
    // calls ->Reset() to clear any stale signal, then Wait()s. The
    // callback thread signals it exactly once on is_final or error.
    // Manual-reset (rather than auto) is safer here because there is
    // ONE waiter and (potentially) ONE signaler per send, and Reset
    // before each Wait guarantees we never see a leftover trigger
    // from the previous message.
    StreamEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ true);

    // Start the thread. TPri_Normal is fine for LLM inference —
    // bumping priority can starve the game thread on single-core
    // laptops and rarely helps on modern multicore machines.
    Thread.Reset(FRunnableThread::Create(
        this,
        TEXT("LiteRtLmConversationWorker"),
        /*InStackSize=*/ 0,
        TPri_Normal));

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Worker: thread started (native_conversation=%s, native_config=%s)"),
           NativeConversation != nullptr ? TEXT("present") : TEXT("null"),
           NativeConversationConfig != nullptr ? TEXT("present") : TEXT("null"));
}

FInoLiteRtLmConversationWorker::~FInoLiteRtLmConversationWorker()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Worker: ~Worker — teardown starting (stream_in_flight=%s)"),
           bStreamInFlight.Load() ? TEXT("yes") : TEXT("no"));

    // Signal the worker to exit its Run() loop. This is the path for a
    // graceful shutdown from the game thread.
    bStopRequested = true;

    // If a stream is in flight, we must cancel it so the LiteRT-LM
    // callback fires its final chunk and signals StreamEvent. Otherwise
    // the worker thread is stuck inside ProcessMessage's StreamEvent->
    // Wait and will never see bStopRequested, and Thread->WaitForCompletion
    // below will hang forever.
    //
    // Setting bStreamCancelled first ensures that when the worker wakes
    // up from StreamEvent->Wait, it dispatches OnError("Cancelled")
    // rather than OnComplete(partial text). That error will fire on the
    // game thread AFTER this destructor returns, but the AsyncTask
    // broadcast checks the weak pointer — if the owning UInoLiteRtLmConversation
    // has already been GC'd, the broadcast is skipped.
    if (bStreamInFlight.Load())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Worker: ~Worker cancelling in-flight stream before join"));
        bStreamCancelled = true;
        if (NativeConversation != nullptr)
        {
            litert_lm_conversation_cancel_process(NativeConversation);
        }
    }

    // Wake the queue-wait event in case the worker is idle between
    // messages. If it's mid-stream instead, this is a no-op for the
    // worker (it's blocked on StreamEvent, not QueueEvent) but we
    // still need to trigger it so that once the stream finishes and
    // the worker loops back to the top of Run(), the subsequent
    // QueueEvent->Wait returns immediately and the loop exits via
    // bStopRequested.
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }

    // Wait for Run() to return. This is the point where the worker
    // thread actually finishes; any in-flight ProcessMessage call will
    // complete (because we just asked LiteRT-LM to cancel it) before
    // the thread exits.
    if (Thread.IsValid())
    {
        Thread->WaitForCompletion();
        Thread.Reset();
    }

    // Return the events to the pool. Must happen AFTER the thread has
    // joined and AFTER LiteRT-LM's stream callback thread has delivered
    // its final chunk — if we returned either event while another
    // thread might still Trigger/Wait on it, we would access a freed
    // event. Thread->WaitForCompletion guarantees both conditions:
    // the worker thread is done, and the worker only returns from
    // ProcessMessage after StreamEvent has been signaled (which only
    // happens after LiteRT-LM's last callback).
    if (QueueEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(QueueEvent);
        QueueEvent = nullptr;
    }
    if (StreamEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(StreamEvent);
        StreamEvent = nullptr;
    }

    // Now it is safe to destroy the native resources. The worker is
    // guaranteed not to be in the middle of any LiteRT-LM call because
    // the thread has finished and the stream callback has delivered
    // its final chunk.
    if (NativeConversation != nullptr)
    {
        litert_lm_conversation_delete(NativeConversation);
        NativeConversation = nullptr;
    }
    if (NativeConversationConfig != nullptr)
    {
        litert_lm_conversation_config_delete(NativeConversationConfig);
        NativeConversationConfig = nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Worker: destroyed (thread joined, native resources freed)"));
}

void FInoLiteRtLmConversationWorker::EnqueueMessage(FString UserText, FString ExtraContext)
{
    UE_LOG(LogInoAgents, Verbose,
           TEXT("LiteRtLm: Worker: EnqueueMessage (user_text_len=%d, extra_context_len=%d)"),
           UserText.Len(), ExtraContext.Len());

    MessageQueue.Enqueue({ MoveTemp(UserText), MoveTemp(ExtraContext) });
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

void FInoLiteRtLmConversationWorker::Cancel()
{
    // No-op if nothing is in flight. Reading the atomic once is fine
    // here — the worst case is a TOCTOU where the stream finishes
    // between our read and the cancel_process call, in which case
    // cancel_process is itself harmless (LiteRT-LM handles "cancel
    // with nothing to cancel" gracefully per the C API docs).
    if (!bStreamInFlight.Load())
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("LiteRtLm: Worker: Cancel — no stream in flight, no-op"));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Worker: Cancel — invoking litert_lm_conversation_cancel_process"));

    // Mark the stream as cancelled. The worker thread reads this after
    // StreamEvent unblocks and dispatches OnError("Cancelled by caller")
    // instead of OnComplete. The static callback also reads it to
    // suppress further OnToken broadcasts after cancel is requested —
    // if any chunks arrive between the cancel call and LiteRT-LM
    // processing it, they won't leak into the game thread.
    bStreamCancelled = true;

    if (NativeConversation != nullptr)
    {
        litert_lm_conversation_cancel_process(NativeConversation);
    }
}

uint32 FInoLiteRtLmConversationWorker::Run()
{
    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Worker: Run — entering main loop"));

    while (!bStopRequested)
    {
        FPendingMessage Msg;
        if (MessageQueue.Dequeue(Msg))
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Worker: dequeued message (user_text_len=%d, extra_context_len=%d); dispatching to ProcessMessage"),
                   Msg.UserText.Len(), Msg.ExtraContext.Len());
            ProcessMessage(Msg.UserText, Msg.ExtraContext);
        }
        else
        {
            // Queue is empty — block until either a new message is
            // enqueued or Stop() / destructor triggers the event.
            if (QueueEvent)
            {
                QueueEvent->Wait();
            }
        }
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Worker: Run — exiting main loop (stop observed)"));
    return 0;
}

void FInoLiteRtLmConversationWorker::Stop()
{
    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Worker: Stop called"));
    bStopRequested = true;
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

void FInoLiteRtLmConversationWorker::ProcessMessage(
    const FString& UserText, const FString& ExtraContext)
{
    if (NativeConversation == nullptr)
    {
        DispatchErrorOnGameThread(TEXT("Native conversation is null"));
        return;
    }

    // ==================================================================
    // Multi-round agent loop
    // ==================================================================
    //
    // Round 0: send the user message.
    // Round N+1: if round N produced tool calls, execute them on the
    //            game thread, build a tool_result message, and send
    //            that as a fresh stream on the same native conversation.
    // Terminate: when a round produces no tool calls, its accumulated
    //            text is the final answer — dispatch OnComplete.
    //
    // Safety cap: bail with an error after kMaxAgentLoopRounds rounds
    // to avoid spinning on a stuck tool-call loop.

    // Build the initial user message. Shape (same as D.3 / Phase 1
    // ConversationTest): {"role":"user","content":[{"type":"text","text":...}]}
    // EscapeJsonString includes the surrounding quotes (UE convention)
    // so the format string must NOT re-wrap %s in "".
    FString CurrentMessageJson;
    {
        const FString EscapedPromptQuoted = EscapeJsonString(UserText);
        CurrentMessageJson = FString::Printf(
            TEXT(R"({"role":"user","content":[{"type":"text","text":%s}]})"),
            *EscapedPromptQuoted);
    }

    for (int32 Round = 0; Round < kMaxAgentLoopRounds; ++Round)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Worker: agent loop round %d/%d starting (message_json_len=%d)"),
               Round, kMaxAgentLoopRounds, CurrentMessageJson.Len());

        // --- Run the stream for this round ---
        // Pass ExtraContext only on round 0 (the user's message).
        // Subsequent rounds are tool-result messages where extra
        // context isn't meaningful.
        const FString& RoundContext = (Round == 0) ? ExtraContext : FString();
        if (!RunOneStreamRound(CurrentMessageJson, RoundContext))
        {
            // RunOneStreamRound populates StreamError on failure
            // (stream failed to start). Dispatch and bail.
            UE_LOG(LogInoAgents, Error,
                   TEXT("LiteRtLm: Worker: round %d RunOneStreamRound returned false; dispatching error"),
                   Round);
            DispatchErrorOnGameThread(StreamError);
            return;
        }

        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Worker: round %d stream finished (accumulated_chars=%d, pending_tool_calls=%d, cancelled=%s, error=%s)"),
               Round, StreamAccumulated.Len(), StreamPendingToolCalls.Num(),
               bStreamCancelled.Load() ? TEXT("yes") : TEXT("no"),
               StreamError.IsEmpty() ? TEXT("<none>") : *StreamError);

        // --- Handle terminal conditions ---
        if (bStreamCancelled.Load())
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Worker: round %d cancelled by caller"), Round);
            DispatchErrorOnGameThread(TEXT("Cancelled by caller"));
            return;
        }

        if (!StreamError.IsEmpty())
        {
            DispatchErrorOnGameThread(StreamError);
            return;
        }

        // --- Handle the round's output ---
        if (StreamPendingToolCalls.Num() > 0)
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Worker: round %d produced %d tool call(s); executing on game thread"),
                   Round, StreamPendingToolCalls.Num());

            // The model asked to call tools. Execute each on the game
            // thread (synchronously from this worker's perspective),
            // broadcast OnToolCalled for visibility, and build a
            // tool_result message to feed back into the conversation
            // as round N+1.
            //
            // LiteRT-LM's Gemma 4 data processor accepts one
            // tool_response per content entry, so we bundle every
            // executed tool into a single {"role":"tool"} message.
            FString ToolResultContentParts;
            ToolResultContentParts.Reserve(256);

            for (int32 CallIdx = 0; CallIdx < StreamPendingToolCalls.Num(); ++CallIdx)
            {
                const FPendingToolCall& Call = StreamPendingToolCalls[CallIdx];

                UE_LOG(LogInoAgents, Log,
                       TEXT("LiteRtLm: Tool: invoke \"%s\" (args %d bytes): %s"),
                       *Call.Name.ToString(), Call.ArgumentsJson.Len(), *Call.ArgumentsJson);

                const double TStart = FPlatformTime::Seconds();
                const FString ResultJson = ExecuteToolSynchronously(Call);
                const double Elapsed = FPlatformTime::Seconds() - TStart;

                UE_LOG(LogInoAgents, Log,
                       TEXT("LiteRtLm: Tool: result for \"%s\" (%d bytes, %.2f ms): %s"),
                       *Call.Name.ToString(), ResultJson.Len(), Elapsed * 1000.0, *ResultJson);

                DispatchToolCalledOnGameThread(
                    Call.Name, Call.ArgumentsJson, ResultJson);

                // Build one content entry per tool call. tool_response
                // shape matches the Phase 1 ToolCallTest — see its
                // comment for the details. ResultJson is the raw JSON
                // literal (bare number, string, object) from the tool;
                // it's embedded verbatim into the "value" field.
                const FString EscapedToolNameQuoted =
                    EscapeJsonString(Call.Name.ToString());
                const FString OneEntry = FString::Printf(
                    TEXT(R"({"type":"tool_response","tool_response":{"name":%s,"value":%s}})"),
                    *EscapedToolNameQuoted,
                    *ResultJson);

                if (CallIdx > 0)
                {
                    ToolResultContentParts += TEXT(",");
                }
                ToolResultContentParts += OneEntry;
            }

            CurrentMessageJson = FString::Printf(
                TEXT(R"({"role":"tool","content":[%s]})"),
                *ToolResultContentParts);

            // Drop the pending list — it was consumed. The next
            // round's OnStreamChunk will start fresh.
            StreamPendingToolCalls.Reset();

            // Loop back to the top for round N+1.
            continue;
        }

        // No tool calls this round — we have the final text answer.
        if (StreamAccumulated.IsEmpty())
        {
            // Empty text with no tool calls is pathological. Match
            // D.3 semantics: treat as error rather than OnComplete("").
            UE_LOG(LogInoAgents, Error,
                   TEXT("LiteRtLm: Worker: round %d empty terminal response (no text, no tool calls)"),
                   Round);
            DispatchErrorOnGameThread(
                TEXT("Stream finished with no text content and no tool calls"));
            return;
        }

        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Worker: agent loop terminated at round %d with final text (%d chars); dispatching OnComplete"),
               Round, StreamAccumulated.Len());
        DispatchCompleteOnGameThread(StreamAccumulated);
        return;
    }

    // Safety cap exceeded.
    UE_LOG(LogInoAgents, Error,
           TEXT("LiteRtLm: Worker: agent loop safety cap exceeded (%d rounds)"),
           kMaxAgentLoopRounds);
    DispatchErrorOnGameThread(FString::Printf(
        TEXT("Agent loop exceeded %d rounds without reaching a final answer"),
        kMaxAgentLoopRounds));
}

bool FInoLiteRtLmConversationWorker::RunOneStreamRound(
    const FString& MessageJson, const FString& ExtraContextForRound)
{
    // Reset per-round state BEFORE calling into LiteRT-LM. StreamEvent
    // is manual-reset, so we must clear any leftover signal from the
    // previous round. StreamAccumulated and StreamPendingToolCalls are
    // cleared so only THIS round's output is visible to the caller.
    // bStreamCancelled is intentionally NOT cleared — if the caller
    // cancelled between rounds, we want to honour it immediately at
    // the top of the next round.
    StreamAccumulated.Reset();
    StreamError.Reset();
    StreamPendingToolCalls.Reset();
    if (StreamEvent)
    {
        StreamEvent->Reset();
    }

    const FTCHARToUTF8 MessageJsonUtf8(*MessageJson);

    bStreamInFlight = true;

    // Extra context is passed as a plain string (like system message).
    // The C API parses it as JSON; if that fails, it uses the raw string.
    const FTCHARToUTF8 ExtraContextUtf8(*ExtraContextForRound);
    const char* const ExtraContextCStr =
        ExtraContextForRound.IsEmpty() ? nullptr : ExtraContextUtf8.Get();

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Worker: RunOneStreamRound sending message (%d chars, extra_context=%s)"),
           MessageJson.Len(),
           ExtraContextCStr ? TEXT("<set>") : TEXT("<null>"));

    // Capture stderr around the C API call so we can surface LiteRT-LM's
    // ABSL_LOG(ERROR) diagnostics via UE_LOG. Without this, error details
    // are lost because ABSL_LOG goes to stderr and UE GUI apps don't
    // display stderr output.
#if PLATFORM_WINDOWS
    // Redirect stderr to a temp file BEFORE the C API call.
    // Guard: UE GUI apps have no console — _fileno(stderr) returns -2
    // (invalid fd). Calling _dup/-2 trips the SECURE CRT invalid-parameter
    // handler and crashes. Only attempt the redirect when stderr is valid.
    FString StderrCapturePath;
    FILE* StderrFile = nullptr;
    int OldStderr = -1;
    const int StderrFd = _fileno(stderr);
    if (StderrFd >= 0)
    {
        StderrCapturePath = FPaths::CreateTempFilename(
            *FPaths::ProjectSavedDir(), TEXT("litert_stderr_"));
        const FTCHARToUTF8 PathUtf8(*StderrCapturePath);
        OldStderr = _dup(StderrFd);
        StderrFile = fopen(PathUtf8.Get(), "w");
        if (StderrFile != nullptr)
        {
            _dup2(_fileno(StderrFile), StderrFd);
        }
    }
#endif

    const int StartRc = litert_lm_conversation_send_message_stream(
        NativeConversation,
        MessageJsonUtf8.Get(),
        /*extra_context=*/ ExtraContextCStr,
        &FInoLiteRtLmConversationWorker::OnStreamChunkStatic,
        /*callback_data=*/ this);

#if PLATFORM_WINDOWS
    // Restore stderr and read any captured output.
    FString CapturedStderr;
    if (StderrFile != nullptr)
    {
        fflush(stderr);
        _dup2(OldStderr, StderrFd);
        fclose(StderrFile);
        _close(OldStderr);
        FFileHelper::LoadFileToString(CapturedStderr, *StderrCapturePath);
        IFileManager::Get().Delete(*StderrCapturePath, /*RequireExists=*/ false);
    }
    else if (OldStderr >= 0)
    {
        _close(OldStderr);
    }
    if (!CapturedStderr.IsEmpty())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Worker: LiteRT-LM stderr during send_message_stream: %s"),
               *CapturedStderr.TrimEnd());
    }
#endif

    if (StartRc != 0)
    {
        bStreamInFlight = false;
        StreamError = FString::Printf(
            TEXT("litert_lm_conversation_send_message_stream returned non-zero (%d) — stream did not start"),
            StartRc);
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Worker: send_message_stream failed to start (rc=%d)"),
               StartRc);
        return false;
    }

    UE_LOG(LogInoAgents, Verbose,
           TEXT("LiteRtLm: Worker: send_message_stream dispatched, waiting on StreamEvent"));

    if (StreamEvent)
    {
        StreamEvent->Wait();
    }

    bStreamInFlight = false;
    return true;
}

FString FInoLiteRtLmConversationWorker::ExecuteToolSynchronously(
    const FPendingToolCall& Call)
{
    // Queue an AsyncTask to the game thread that looks up the tool
    // in the subsystem's registry, invokes Execute, and fulfils the
    // result into ToolResult. Block on DoneEvent until the task
    // completes. Reference capture of ToolResult is safe because
    // this stack frame is still alive throughout the Wait.
    FString ToolResult;
    FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

    TWeakObjectPtr<UInoLiteRtLmSubsystem> WeakSubs = WeakSubsystem;
    const FName    ToolName = Call.Name;
    const FString  ArgsJson = Call.ArgumentsJson;

    AsyncTask(ENamedThreads::GameThread,
        [WeakSubs, ToolName, ArgsJson, &ToolResult, DoneEvent]()
    {
        // Runs on the game thread. Safe to dereference weak pointers
        // and touch UObjects.
        if (UInoLiteRtLmSubsystem* Subs = WeakSubs.Get())
        {
            UInoLiteRtLmToolBase* Tool = Subs->FindTool(ToolName);
            if (Tool != nullptr)
            {
                // Execute is a BlueprintNativeEvent — calling it on
                // the UObject dispatches to either C++ or Blueprint
                // implementations. Wrap in try/catch so a misbehaving
                // tool that throws can't kill the game thread.
                FString LocalResult;
                #if PLATFORM_EXCEPTIONS_DISABLED
                    LocalResult = Tool->ExecuteFromString(ArgsJson);
                #else
                    try
                    {
                        LocalResult = Tool->ExecuteFromString(ArgsJson);
                    }
                    catch (...)
                    {
                        LocalResult = FString(TEXT("\"ERROR: uncaught exception in tool Execute\""));
                    }
                #endif

                if (LocalResult.IsEmpty())
                {
                    // An empty return is not a valid JSON value.
                    // Coerce to a JSON string literal so the model
                    // sees SOMETHING in the tool_response.value
                    // field rather than a syntax error.
                    LocalResult = FString(TEXT("\"\""));
                }

                // Validate that the tool returned valid JSON. The
                // result is embedded verbatim into the tool_response
                // "value" field — if it's not valid JSON, the entire
                // tool response message becomes malformed and the
                // C API will fail to parse it.
                //
                // UE's FJsonSerializer only parses objects/arrays at
                // the root. Bare JSON values (numbers, booleans, null,
                // quoted strings) are also valid here, so we check
                // those explicitly first.
                //
                // Common mistake: returning a plain string like
                // "hello" instead of a quoted JSON string "\"hello\"".
                // Safety net: if it doesn't parse as JSON, wrap it in
                // quotes to produce a valid JSON string literal.
                {
                    const FString Trimmed = LocalResult.TrimStartAndEnd();
                    const bool bIsBareNumber = Trimmed.IsNumeric()
                        || (Trimmed.Len() > 0 && (Trimmed[0] == TEXT('-') || Trimmed[0] == TEXT('.'))
                            && Trimmed.Mid(1).IsNumeric());
                    const bool bIsBool = (Trimmed == TEXT("true") || Trimmed == TEXT("false"));
                    const bool bIsNull = (Trimmed == TEXT("null"));
                    const bool bIsQuotedString = (Trimmed.Len() >= 2
                        && Trimmed[0] == TEXT('"')
                        && Trimmed[Trimmed.Len() - 1] == TEXT('"'));
                    const bool bIsObjectOrArray = (Trimmed.Len() >= 2
                        && (Trimmed[0] == TEXT('{') || Trimmed[0] == TEXT('[')));

                    bool bIsValidJson = bIsBareNumber || bIsBool || bIsNull || bIsQuotedString;
                    if (!bIsValidJson && bIsObjectOrArray)
                    {
                        // Validate object/array with the full parser.
                        const auto Reader = TJsonReaderFactory<>::Create(LocalResult);
                        TSharedPtr<FJsonValue> Parsed;
                        bIsValidJson = FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
                    }

                    if (!bIsValidJson)
                    {
                        UE_LOG(LogInoAgents, Warning,
                               TEXT("LiteRtLm: Tool: \"%s\" returned invalid JSON: \"%s\". "
                                    "Wrapping in quotes to produce a valid JSON string. "
                                    "Tool implementations should return valid JSON "
                                    "(bare number, quoted string, object, or array)."),
                               *ToolName.ToString(),
                               *LocalResult.Left(200));
                        LocalResult = EscapeJsonString(LocalResult);
                    }
                }

                ToolResult = LocalResult;
            }
            else
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LiteRtLm: Tool: \"%s\" not registered at dispatch time"),
                       *ToolName.ToString());
                ToolResult = FString::Printf(
                    TEXT("\"ERROR: tool '%s' is not registered\""),
                    *ToolName.ToString());
            }
        }
        else
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Tool: subsystem was destroyed before tool \"%s\" could run"),
                   *ToolName.ToString());
            ToolResult = FString(TEXT("\"ERROR: subsystem has been destroyed\""));
        }

        DoneEvent->Trigger();
    });

    // Block here on the worker thread until the game thread finishes
    // Execute. The game thread is NOT blocked on us — the AsyncTask
    // is a normal game-thread task that runs during the next tick /
    // TaskGraph pass. The worker thread stays asleep until Trigger.
    DoneEvent->Wait();

    FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

    return ToolResult;
}

void FInoLiteRtLmConversationWorker::OnStreamChunkStatic(
    void* callback_data,
    const char* chunk,
    bool is_final,
    const char* error_msg)
{
    // Static trampoline. callback_data is the worker instance pointer
    // we passed to send_message_stream. It is guaranteed valid for
    // the lifetime of the stream because the worker thread is blocked
    // inside ProcessMessage's StreamEvent->Wait and ~FInoLiteRtLmConversationWorker
    // always waits for the final callback before destroying.
    if (callback_data == nullptr)
    {
        return;
    }
    auto* const Self = static_cast<FInoLiteRtLmConversationWorker*>(callback_data);
    Self->OnStreamChunk(chunk, is_final, error_msg);
}

void FInoLiteRtLmConversationWorker::OnStreamChunk(
    const char* chunk, bool is_final, const char* error_msg)
{
    // Runs on LiteRT-LM's internal thread. MUST NOT touch UObject
    // state directly. Safe to touch worker instance fields that are
    // synchronized via StreamEvent — StreamAccumulated and StreamError
    // are read only by the worker thread AFTER StreamEvent is signaled
    // (happens-before edge), so writes here are safe as long as we
    // don't signal until we're done writing.

    const bool bCancelledNow = bStreamCancelled.Load();

    // Capture the error message if LiteRT-LM reported one.
    if (error_msg != nullptr && *error_msg != '\0')
    {
        StreamError = FString(UTF8_TO_TCHAR(error_msg));
    }

    // Parse the chunk JSON and extract parts, then accumulate +
    // dispatch as appropriate. LiteRT-LM's conversation_send_message_stream
    // delivers each chunk as a FULL assistant message JSON wrapping
    // one token delta (for text responses) or a single tool call
    // (for tool_call responses). The two shapes are quite different:
    //
    //   TEXT chunk (D.3-style, one per token):
    //     {"role":"assistant","content":[{"type":"text","text":"delta"}]}
    //
    //   TOOL CALL chunk (OpenAI-compatible, per
    //   runtime/conversation/model_data_processor/gemma4_data_processor_test.cc):
    //     {"role":"assistant","tool_calls":[
    //       {"type":"function",
    //        "function":{"name":"<tool>","arguments":{...}}}
    //     ]}
    //
    // We walk BOTH `content[*]` (for text parts) AND the top-level
    // `tool_calls[*]` (for tool-call entries) so a single chunk can
    // in principle carry both; in practice LiteRT-LM's constrained
    // decoding emits one or the other per chunk.
    //
    // Suppressed entirely if the stream is cancelled — the final
    // terminal broadcast will be OnError("Cancelled by caller") and
    // leaking tokens or tool calls through after cancel would
    // confuse observers and round ordering.
    if (chunk != nullptr && *chunk != '\0' && !bCancelledNow)
    {
        const FString ChunkJson(UTF8_TO_TCHAR(chunk));

        TSharedPtr<FJsonObject> RootObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ChunkJson);
        if (FJsonSerializer::Deserialize(Reader, RootObj) && RootObj.IsValid())
        {
            bool bChunkHadAnyContent = false;

            // ---- Walk content[*] for text parts --------------------
            FString ChunkText;
            const TArray<TSharedPtr<FJsonValue>>* ContentArrayPtr = nullptr;
            if (RootObj->TryGetArrayField(TEXT("content"), ContentArrayPtr)
                && ContentArrayPtr != nullptr)
            {
                for (const TSharedPtr<FJsonValue>& PartValue : *ContentArrayPtr)
                {
                    if (!PartValue.IsValid() || PartValue->Type != EJson::Object)
                    {
                        continue;
                    }
                    const TSharedPtr<FJsonObject>& PartObj = PartValue->AsObject();

                    FString PartType;
                    PartObj->TryGetStringField(TEXT("type"), PartType);

                    if (PartType == TEXT("text"))
                    {
                        FString PartText;
                        if (PartObj->TryGetStringField(TEXT("text"), PartText))
                        {
                            ChunkText += PartText;
                        }
                    }
                    // Any other content-part type is currently
                    // ignored. A future iteration may handle audio /
                    // image response parts here.
                }
            }

            if (!ChunkText.IsEmpty())
            {
                UE_LOG(LogInoAgents, Verbose,
                       TEXT("LiteRtLm: Worker: token chunk (%d chars)"), ChunkText.Len());
                StreamAccumulated += ChunkText;
                DispatchTokenOnGameThread(ChunkText);
                bChunkHadAnyContent = true;
            }

            // ---- Walk top-level tool_calls[*] -----------------------
            //
            // Each entry is an OpenAI-style function-call envelope:
            //   { "type": "function",
            //     "function": { "name": "...", "arguments": {...} } }
            //
            // We re-serialise the arguments sub-object to a compact
            // JSON string here so the worker thread can hand it
            // directly to UInoLiteRtLmToolBase::Execute without needing to
            // re-serialise.
            const TArray<TSharedPtr<FJsonValue>>* ToolCallsArrayPtr = nullptr;
            if (RootObj->TryGetArrayField(TEXT("tool_calls"), ToolCallsArrayPtr)
                && ToolCallsArrayPtr != nullptr)
            {
                for (const TSharedPtr<FJsonValue>& CallValue : *ToolCallsArrayPtr)
                {
                    if (!CallValue.IsValid() || CallValue->Type != EJson::Object)
                    {
                        continue;
                    }
                    const TSharedPtr<FJsonObject>& CallObj = CallValue->AsObject();

                    const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
                    if (!CallObj->TryGetObjectField(TEXT("function"), FunctionObjPtr)
                        || FunctionObjPtr == nullptr
                        || !FunctionObjPtr->IsValid())
                    {
                        continue;
                    }
                    const TSharedPtr<FJsonObject>& FunctionObj = *FunctionObjPtr;

                    FString ToolNameStr;
                    if (!FunctionObj->TryGetStringField(TEXT("name"), ToolNameStr)
                        || ToolNameStr.IsEmpty())
                    {
                        UE_LOG(LogInoAgents, Warning,
                               TEXT("LiteRtLm: Worker: tool_call entry has no function.name field, dropping: %s"),
                               *ChunkJson);
                        continue;
                    }

                    // Serialise the arguments sub-object (or default
                    // to "{}" for zero-arg tools). Use a condensed
                    // writer so the result is a single-line JSON
                    // string — the default TJsonWriter emits
                    // pretty-printed JSON with newlines between
                    // fields, which makes log lines look truncated
                    // and confuses the tool Execute wrapper if it
                    // naively splits on whitespace.
                    FString ArgsJsonStr = TEXT("{}");
                    const TSharedPtr<FJsonObject>* ArgsObjPtr = nullptr;
                    if (FunctionObj->TryGetObjectField(TEXT("arguments"), ArgsObjPtr)
                        && ArgsObjPtr != nullptr
                        && ArgsObjPtr->IsValid())
                    {
                        ArgsJsonStr.Reset();
                        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> ArgsWriter =
                            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArgsJsonStr);
                        FJsonSerializer::Serialize(ArgsObjPtr->ToSharedRef(), ArgsWriter);
                    }

                    UE_LOG(LogInoAgents, Verbose,
                           TEXT("LiteRtLm: Worker: captured tool_call \"%s\" (args %d bytes)"),
                           *ToolNameStr, ArgsJsonStr.Len());

                    FPendingToolCall NewCall;
                    NewCall.Name          = FName(*ToolNameStr);
                    NewCall.ArgumentsJson = MoveTemp(ArgsJsonStr);
                    StreamPendingToolCalls.Add(MoveTemp(NewCall));

                    bChunkHadAnyContent = true;
                }
            }

            // ---- Diagnostic for unrecognised chunks -----------------
            // If we got a well-formed JSON object with neither text
            // parts nor tool_calls entries, log it at Log level so
            // future shape surprises are immediately visible without
            // having to re-enable a debug build. Not a warning
            // because some final/empty chunks are legitimate
            // terminators.
            if (!bChunkHadAnyContent && !is_final)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("LiteRtLm: Worker: chunk had no recognised content (not text or tool_calls). Raw chunk: %s"),
                       *ChunkJson);
            }
        }
        else
        {
            // Parse failure on a chunk is unexpected — LiteRT-LM
            // should always emit well-formed JSON. Log a warning and
            // keep going; accumulated text will just be incomplete
            // for this send. Don't abort the stream — we still want
            // the final callback to fire so the worker unblocks.
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Worker: failed to parse stream chunk JSON, dropping: %s"),
                   *ChunkJson);
        }
    }

    // On the final callback (or on error), signal the worker thread
    // to wake up from StreamEvent->Wait and dispatch the terminal
    // OnComplete / OnError. is_final is what LiteRT-LM uses to mark
    // the end of a clean stream; error_msg != nullptr is end-of-stream
    // via failure. Either way, there will be no more callbacks.
    const bool bErrored = (error_msg != nullptr && *error_msg != '\0');
    if (is_final || bErrored)
    {
        if (StreamEvent)
        {
            StreamEvent->Trigger();
        }
    }
}

void FInoLiteRtLmConversationWorker::DispatchTokenOnGameThread(FString Chunk)
{
    // Called from the stream callback on LiteRT-LM's internal thread
    // (not the worker thread and definitely not the game thread).
    // Capture the weak pointer and chunk by value into the lambda and
    // queue it on the game thread via AsyncTask. The game thread
    // processes AsyncTasks in FIFO order, so tokens dispatched in
    // sequence from the callback arrive on the game thread in the
    // same order — and any dispatches we queue for OnComplete / OnError
    // after the stream finishes are ordered strictly AFTER every
    // OnToken dispatch from the same send.
    TWeakObjectPtr<UInoLiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, Chunk = MoveTemp(Chunk)]()
    {
        if (UInoLiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            const FString CleanChunk = Conv->FilterCleanToken(Chunk);
            Conv->OnToken.Broadcast(Chunk, CleanChunk);
            Conv->AccumulateTokenForSentence(Chunk);
        }
    });
}

void FInoLiteRtLmConversationWorker::DispatchCompleteOnGameThread(FString FullText)
{
    // Capture WeakOwner by value so the lambda has its own copy. The
    // lambda runs on the game thread; the game-thread validity check
    // guards against the UObject having been GC'd between the time
    // the worker dispatches and the time the lambda runs.
    TWeakObjectPtr<UInoLiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, FullText = MoveTemp(FullText)]()
    {
        if (UInoLiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            // Flush any trailing text that didn't end with a sentence
            // delimiter (e.g. "The answer is 42" with no period). This
            // fires a final OnSentence so the concatenation of every
            // OnSentence always equals the full response.
            Conv->FlushSentenceBuffer();
            Conv->RecordAssistantMessage(FullText);
            Conv->OnComplete.Broadcast(FullText);
        }
    });
}

void FInoLiteRtLmConversationWorker::DispatchErrorOnGameThread(FString ErrorMessage)
{
    TWeakObjectPtr<UInoLiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, ErrorMessage = MoveTemp(ErrorMessage)]()
    {
        if (UInoLiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            Conv->OnError.Broadcast(ErrorMessage);
        }
    });
}

void FInoLiteRtLmConversationWorker::DispatchToolCalledOnGameThread(
    FName ToolName, FString ArgumentsJson, FString ResultJson)
{
    // Dispatched from the worker thread AFTER ExecuteToolSynchronously
    // has already run the tool on the game thread and returned with
    // a result. This broadcast is purely observational — it tells
    // listeners "this tool ran with these arguments and returned
    // this JSON". Listeners who want to block / filter / modify
    // tool behaviour are not supported in D.4; they would need the
    // deferred-result API that is stubbed for future work.
    TWeakObjectPtr<UInoLiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, ToolName,
         ArgumentsJson = MoveTemp(ArgumentsJson),
         ResultJson = MoveTemp(ResultJson)]()
    {
        if (UInoLiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            Conv->OnToolCalled.Broadcast(ToolName, ArgumentsJson, ResultJson);
        }
    });
}
