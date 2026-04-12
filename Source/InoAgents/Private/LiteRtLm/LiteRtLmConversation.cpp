// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmConversation.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmModelConfig.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"
#include "LiteRtLmConversationWorker.h"

#include "Serialization/JsonWriter.h"  // EscapeJsonString

#include "litert/lm/engine.h"

// Out-of-line ctors/dtor. These MUST live in this translation unit (not
// in the header) because the Worker TUniquePtr's deleter needs the full
// definition of FLiteRtLmConversationWorker to `delete` it, and only
// this TU includes LiteRtLmConversationWorker.h. Leaving any of them
// implicit in the header lets UHT's generated .gen.cpp instantiate the
// deleter against the forward-declared class, which fails with C4150.
//
// UHT generates TWO implicit ctors for every UCLASS: the default ctor
// AND a hot-reload vtable helper ctor (DEFINE_VTABLE_PTR_HELPER_CTOR_NS).
// Both must be supplied out-of-line.
ULiteRtLmConversation::ULiteRtLmConversation() = default;
ULiteRtLmConversation::ULiteRtLmConversation(FVTableHelper& Helper)
    : Super(Helper)
{
}
ULiteRtLmConversation::~ULiteRtLmConversation() = default;

void ULiteRtLmConversation::Initialize(
    ULiteRtLmSubsystem* InSubsystem,
    LiteRtLmEngine* InEngine,
    const ULiteRtLmModelConfig* InConfig)
{
    check(IsInGameThread());

    if (InEngine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: engine is null"));
        return;
    }
    if (InConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: config is null"));
        return;
    }

    Subsystem = InSubsystem;

    // Build the system message string for the C API.
    //
    // IMPORTANT: pass as a PLAIN STRING, not as a JSON object like
    // {"type":"text","text":"..."}.
    //
    // The C API (c/engine.cc:214-226) tries to JSON-parse the string.
    // If parsing fails, it treats the raw string as the "content"
    // field of a {"role":"system","content":"..."} message. Gemma's
    // Jinja2 chat template expects "content" to be a plain string
    // (or an array). If we pass a JSON object, the template sees an
    // object it can't index with [0] and SILENTLY DROPS the system
    // message — the model never sees our prompt.
    //
    // By passing the raw text (not valid JSON), the C API's parse
    // fails gracefully and uses the raw string as content, which is
    // exactly what Gemma's template expects.
    const FTCHARToUTF8 SystemMessageUtf8(*InConfig->SystemMessage);
    const char* const SystemMessageCStr =
        InConfig->SystemMessage.IsEmpty() ? nullptr : SystemMessageUtf8.Get();

    // Snapshot the subsystem's tool registry into a tools_json array
    // for this conversation. Tools registered AFTER this call do not
    // retroactively apply — the LiteRT-LM C API consumes tools_json
    // during conversation_config_create and does not expose a way
    // to mutate it afterward. Tool calls from the model are routed
    // back to the subsystem's registry via TWeakObjectPtr in the
    // worker, not via a per-conversation tool list, so runtime tool
    // dispatch still uses the current registry state — the snapshot
    // only determines what the MODEL is TOLD about.
    //
    // Constrained decoding is enabled whenever at least one tool is
    // registered. It routes sampling through
    // libGemmaModelConstraintProvider.dll, which forces the model to
    // emit syntactically-valid function-call JSON when a tool call
    // is expected. With no tools, constrained decoding is disabled
    // (saves overhead) and the conversation behaves exactly as D.3.
    FString        ToolsJson;
    const char*    ToolsJsonCStr               = nullptr;
    bool           bEnableConstrainedDecoding  = false;
    FTCHARToUTF8*  ToolsJsonUtf8Ptr            = nullptr;

    if (InSubsystem != nullptr)
    {
        ToolsJson = InSubsystem->BuildToolsJsonForConversation();
        if (!ToolsJson.IsEmpty())
        {
            // Heap-allocate the UTF-8 converter so its storage outlives
            // this stack frame through the C API call below — the call
            // reads tools_json synchronously during config creation so
            // this is actually overcautious (the stack frame is still
            // alive), but doing it this way matches the pattern used
            // for the system message and is easy to reason about.
            ToolsJsonUtf8Ptr = new FTCHARToUTF8(*ToolsJson);
            ToolsJsonCStr = ToolsJsonUtf8Ptr->Get();
            bEnableConstrainedDecoding = true;

            UE_LOG(LogInoAgents, Log,
                   TEXT("ULiteRtLmConversation::Initialize: tools registered, "
                        "constrained decoding ENABLED (%d bytes of tools_json)"),
                   ToolsJson.Len());
        }
    }

    // Create the native conversation config. D.4 passes tools_json
    // and enable_constrained_decoding when the subsystem has
    // registered tools; otherwise both stay in their D.3 defaults
    // (null / false) and the conversation behaves as a plain chat.
    LiteRtLmConversationConfig* NativeConvConfig = litert_lm_conversation_config_create(
        InEngine,
        /*session_config=*/              nullptr,
        /*system_message_json=*/         SystemMessageCStr,
        /*tools_json=*/                  ToolsJsonCStr,
        /*messages_json=*/               nullptr,
        /*enable_constrained_decoding=*/ bEnableConstrainedDecoding);

    // Free the heap UTF-8 converter — the C API has already read
    // the string by the time conversation_config_create returns.
    if (ToolsJsonUtf8Ptr != nullptr)
    {
        delete ToolsJsonUtf8Ptr;
        ToolsJsonUtf8Ptr = nullptr;
    }

    if (NativeConvConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: "
                    "litert_lm_conversation_config_create returned NULL"));
        return;
    }

    // Create the native conversation. On failure, clean up the config we
    // just built so we don't leak it.
    LiteRtLmConversation* NativeConv = litert_lm_conversation_create(InEngine, NativeConvConfig);
    if (NativeConv == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: "
                    "litert_lm_conversation_create returned NULL"));
        litert_lm_conversation_config_delete(NativeConvConfig);
        return;
    }

    // Hand the native resources to a new worker. From this point on, the
    // worker owns them — it will destroy them in its destructor when
    // BeginDestroy → Worker.Reset() runs. The subsystem weak pointer
    // lets the worker's agent loop look up tools on the game thread
    // without routing through this UObject (the delegates live here
    // but the tool registry lives on the subsystem).
    Worker = MakeUnique<FLiteRtLmConversationWorker>(
        TWeakObjectPtr<ULiteRtLmConversation>(this),
        TWeakObjectPtr<ULiteRtLmSubsystem>(InSubsystem),
        NativeConv,
        NativeConvConfig);

    UE_LOG(LogInoAgents, Log,
           TEXT("ULiteRtLmConversation: initialized (system_message=%s)"),
           InConfig->SystemMessage.IsEmpty() ? TEXT("<none>") : TEXT("<set>"));

    // Log the first 200 chars of the system message so we can verify
    // the right prompt is reaching the native layer. Truncated to
    // avoid flooding the log on very long prompts.
    if (!InConfig->SystemMessage.IsEmpty())
    {
        const FString Preview = InConfig->SystemMessage.Left(200);
        UE_LOG(LogInoAgents, Log,
               TEXT("ULiteRtLmConversation: system_message preview: \"%s%s\""),
               *Preview,
               InConfig->SystemMessage.Len() > 200 ? TEXT("...") : TEXT(""));
    }
}

void ULiteRtLmConversation::SendMessageAsync(const FString& UserText)
{
    check(IsInGameThread());

    if (!Worker.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::SendMessageAsync: worker is null — "
                    "conversation was not initialized"));
        OnError.Broadcast(TEXT("Conversation not initialized"));
        return;
    }

    // Clear any leftover sentence buffer from a prior send so it
    // doesn't leak into this one. (FlushSentenceBuffer at OnComplete
    // normally empties it, but if the prior send errored instead of
    // completing, the buffer may be non-empty.)
    SentenceBuffer.Empty();

    Worker->EnqueueMessage(UserText);
}

void ULiteRtLmConversation::Cancel()
{
    check(IsInGameThread());

    if (!Worker.IsValid())
    {
        // No worker, no stream in flight, nothing to cancel. Not an error.
        return;
    }

    Worker->Cancel();
}

bool ULiteRtLmConversation::IsStreamingInFlight() const
{
    if (!Worker.IsValid())
    {
        // Zombie conversation (Shutdown called or Initialize never ran).
        // Not streaming because it can't stream.
        return false;
    }
    return Worker->IsStreamInFlight();
}

void ULiteRtLmConversation::Shutdown()
{
    check(IsInGameThread());

    // Resetting the TUniquePtr invokes ~FLiteRtLmConversationWorker,
    // which cancels any in-flight stream, joins the worker thread,
    // and destroys the native LiteRT-LM resources. No delegate
    // invocation list is touched — safe to call from inside one of
    // this conversation's own delegate handlers.
    //
    // Second call is a no-op because TUniquePtr::Reset on an already-
    // null pointer does nothing. BeginDestroy calls the same
    // Worker.Reset() again, which also becomes a no-op after Shutdown.
    Worker.Reset();
}

void ULiteRtLmConversation::SubmitDeferredToolResult(
    FName ToolCallId, const FString& ResultJson)
{
    // Stubbed for Milestone D.4 — see the header doc-comment. All
    // D.4 tool calls are resolved synchronously inside the worker's
    // agent loop via a game-thread FEvent round-trip, so there is
    // nothing for this method to unblock. The future implementation
    // will use ToolCallId to look up a pending TPromise stored on
    // the worker and fulfil it with ResultJson.
    UE_LOG(LogInoAgents, Warning,
           TEXT("SubmitDeferredToolResult(%s): ignored — deferred tool "
                "results are not wired through in Milestone D.4. All "
                "tools execute synchronously on the game thread from "
                "inside the worker's agent loop. (ResultJson length: %d)"),
           *ToolCallId.ToString(), ResultJson.Len());
}

void ULiteRtLmConversation::BeginDestroy()
{
    // Resetting the TUniquePtr invokes ~FLiteRtLmConversationWorker,
    // which:
    //   1. Signals the worker thread to stop
    //   2. Waits for the worker thread to finish its current message
    //   3. Destroys the native LiteRtLmConversation and ConversationConfig
    //   4. Returns the queue event to the UE pool
    //
    // This is the whole cleanup path for the conversation. No explicit
    // delete of native pointers needed here — the worker owns them.
    Worker.Reset();

    Super::BeginDestroy();
}

// ======================================================================
// Sentence detection
// ======================================================================

void ULiteRtLmConversation::AccumulateTokenForSentence(const FString& Chunk)
{
    check(IsInGameThread());

    SentenceBuffer += Chunk;

    // Split on newline only — each line becomes one OnSentence.
    // Periods, exclamation marks, and question marks stay inside the
    // line text so TTS speaks them with natural intonation rather than
    // fragmenting "1." or "**Learn something new?**" into separate
    // TTS calls.
    while (true)
    {
        const int32 NewlineIndex = SentenceBuffer.Find(TEXT("\n"));
        if (NewlineIndex == INDEX_NONE)
        {
            break;
        }

        FString Line = SentenceBuffer.Left(NewlineIndex).TrimStartAndEnd();
        SentenceBuffer.MidInline(NewlineIndex + 1);

        if (!Line.IsEmpty())
        {
            OnSentence.Broadcast(Line);
        }
        OnNewLine.Broadcast();
    }
}

void ULiteRtLmConversation::FlushSentenceBuffer()
{
    check(IsInGameThread());

    const FString Remainder = SentenceBuffer.TrimStartAndEnd();
    SentenceBuffer.Empty();

    if (!Remainder.IsEmpty())
    {
        OnSentence.Broadcast(Remainder);
    }
}
