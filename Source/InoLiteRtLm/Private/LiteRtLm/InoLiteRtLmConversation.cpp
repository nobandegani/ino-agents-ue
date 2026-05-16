// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmConversation.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmSubsystem.h"
#include "InoLiteRtLmConversationWorker.h"

#include "litert/lm/engine.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    /**
     * Defuse Gemma-4 control-token digraphs in untrusted strings before
     * they are injected into the prompt. The model's chat template
     * renders user / context content verbatim (`{{ item['text'] | trim }}`
     * — no escaping), so a literal "<|turn>", "<|tool_call>",
     * "<|tool_response>", "<|channel>", "<|\"|>", "<|think|>" (or the
     * matching "<...|>" closers) embedded in game state or player text
     * would spoof a turn / tool / channel boundary in the token stream.
     *
     * The SentencePiece tokenizer only matches the exact, uninterrupted
     * control strings, so breaking the contiguous "<|" and "|>" digraphs
     * with a space fully neutralises every Gemma-4 control token while
     * leaving the text human-readable. Cheap (two ReplaceInline passes)
     * and applied per context entry.
     */
    FString InoLrlSanitizeForPrompt(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("<|"), TEXT("< |"), ESearchCase::CaseSensitive);
        Out.ReplaceInline(TEXT("|>"), TEXT("| >"), ESearchCase::CaseSensitive);
        return Out;
    }
}

// Out-of-line ctors/dtor. These MUST live in this translation unit (not
// in the header) because the Worker TUniquePtr's deleter needs the full
// definition of FInoLiteRtLmConversationWorker to `delete` it, and only
// this TU includes InoLiteRtLmConversationWorker.h. Leaving any of them
// implicit in the header lets UHT's generated .gen.cpp instantiate the
// deleter against the forward-declared class, which fails with C4150.
//
// UHT generates TWO implicit ctors for every UCLASS: the default ctor
// AND a hot-reload vtable helper ctor (DEFINE_VTABLE_PTR_HELPER_CTOR_NS).
// Both must be supplied out-of-line.
UInoLiteRtLmConversation::UInoLiteRtLmConversation() = default;
UInoLiteRtLmConversation::UInoLiteRtLmConversation(FVTableHelper& Helper)
    : Super(Helper)
{
}
UInoLiteRtLmConversation::~UInoLiteRtLmConversation() = default;

void UInoLiteRtLmConversation::Initialize(
    UInoLiteRtLmSubsystem* InSubsystem,
    LiteRtLmEngine* InEngine,
    const FInoLiteRtLmModelConfig& InConfig,
    const TArray<FInoLiteRtLmMessage>& InInitialMessages)
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Conversation: Initialize called (engine=%s, model=%s, initial_history=%d)"),
           InEngine != nullptr ? TEXT("present") : TEXT("null"),
           *InConfig.ModelFileName,
           InInitialMessages.Num());

    if (InEngine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Conversation: Initialize FAILED — engine is null"));
        return;
    }
    if (InConfig.ModelFileName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Conversation: Initialize FAILED — config is null"));
        return;
    }

    Subsystem = InSubsystem;

    // Build the system message string for the C API.
    //
    // IMPORTANT: pass as a PLAIN STRING, not as a JSON object like
    // {"type":"text","text":"..."}.
    //
    // The C API (litert_lm_conversation_config_set_system_message in
    // c/engine.cc) tries to JSON-parse the string.
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
    const FTCHARToUTF8 SystemMessageUtf8(*InConfig.SystemMessage);
    const char* const SystemMessageCStr =
        InConfig.SystemMessage.IsEmpty() ? nullptr : SystemMessageUtf8.Get();

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
    // (saves overhead).
    FString    ToolsJson;
    bool       bEnableConstrainedDecoding = false;
    if (InSubsystem != nullptr)
    {
        ToolsJson = InSubsystem->BuildToolsJsonForConversation();
        bEnableConstrainedDecoding = !ToolsJson.IsEmpty();
        if (bEnableConstrainedDecoding)
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Conversation: Initialize — tools registered, "
                        "constrained decoding ENABLED (%d bytes of tools_json)"),
                   ToolsJson.Len());
        }
    }
    // Stack-scoped UTF-8 converter — set_tools reads tools_json
    // synchronously during the call, so the converter's storage only
    // needs to outlive that one call.
    const FTCHARToUTF8 ToolsJsonUtf8(*ToolsJson);
    const char* const  ToolsJsonCStr = ToolsJson.IsEmpty() ? nullptr : ToolsJsonUtf8.Get();

    // Session config (sampler params + max output tokens).
    //
    // Historical note (regression timeline):
    //   - v0.10.1: passing ANY user-created SessionConfig made
    //     Conversation::Create → engine.CreateSession() return NULL for
    //     Gemma 4 models. Block was gated off; conversations ran with
    //     engine defaults.
    //   - v0.10.2 / SHA 4dbbf937: fix landed upstream; block re-enabled.
    //   - v0.11.0-rc.1: regression returned. Symptom — same as v0.10.1:
    //     conversation_create returns NULL the moment a SessionConfig is
    //     attached, with no useful log line above it. Smoke tests
    //     (`Ino.LiteRtLm.ConversationTest`) which never attach a
    //     SessionConfig keep working on the same engine handle.
    //
    // Caller-controlled via FInoLiteRtLmModelConfig::bAttachSessionConfig
    // (default false). When false, conversations run with engine defaults
    // (TopK sampler, default temperature, unbounded output) — losing the
    // FInoLiteRtLmConfig::Sampler / MaxOutputTokens overrides. Flip on to
    // test whether the v0.11.0 final pin has fixed the regression; verify
    // with Ino.LiteRtLm.ConversationSendTest before shipping enabled.
    const bool bAttachSessionConfig = InConfig.bAttachSessionConfig;
    LiteRtLmSessionConfig* SessionConfig =
        bAttachSessionConfig ? litert_lm_session_config_create() : nullptr;
    if (SessionConfig != nullptr)
    {
        // The staged LiteRT-LM runtime's sampler factory implements only
        // TOP_P on CPU/GPU (no top-k kernel, no dedicated greedy kernel).
        // Passing kLiteRtLmSamplerTypeTopK / ...Greedy makes
        // CreateCpuSampler return UnimplementedError, which makes
        // engine_create_session return NULL — i.e. the conversation
        // silently fails to construct. So clamp every requested strategy
        // to TOP_P:
        //   - TopK   → TopP (keep the top_k value; the Top-P sampler
        //              also honours top_k as a pre-filter).
        //   - Greedy → TopP with temperature 0 + top_p 1.0, which is
        //              argmax-equivalent (deterministic) without needing
        //              a dedicated greedy kernel.
        LiteRtLmSamplerParams NativeSampler = {};
        NativeSampler.type        = kLiteRtLmSamplerTypeTopP;
        NativeSampler.top_k       = InConfig.Sampler.TopK;
        NativeSampler.top_p       = InConfig.Sampler.TopP;
        NativeSampler.temperature = InConfig.Sampler.Temperature;

        if (InConfig.Sampler.Type == EInoLiteRtLmSamplerType::Greedy)
        {
            NativeSampler.temperature = 0.0f;
            NativeSampler.top_p       = 1.0f;
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Conversation: sampler 'Greedy' mapped to Top-P "
                        "with temperature 0 (the staged runtime has no greedy "
                        "kernel; this is argmax-equivalent)."));
        }
        else if (InConfig.Sampler.Type == EInoLiteRtLmSamplerType::TopK)
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Conversation: sampler 'Top-K' clamped to Top-P "
                        "(the staged CPU/GPU runtime has no top-k kernel; "
                        "top_k=%d is still applied as a Top-P pre-filter)."),
                   InConfig.Sampler.TopK);
        }

        // Widen the host RNG seed: FMath::Rand() is only ~15 bits on
        // some platforms, a poor distribution for a 32-bit seed field.
        NativeSampler.seed = InConfig.Sampler.Seed >= 0
            ? InConfig.Sampler.Seed
            : (FMath::Rand() ^ (FMath::Rand() << 15) ^ (FMath::Rand() << 30));
        litert_lm_session_config_set_sampler_params(SessionConfig, &NativeSampler);

        if (InConfig.MaxOutputTokens > 0)
        {
            litert_lm_session_config_set_max_output_tokens(
                SessionConfig, InConfig.MaxOutputTokens);
        }
    }

    // Pre-populated conversation history (messages_json).
    // Serialize InitialMessages into the JSON array
    // [{"role":"user","content":"..."},{"role":"assistant","content":"..."}]
    // via FJsonSerializer rather than hand-rolled string concat — the
    // serializer escapes control characters, surrogate pairs, and the
    // edge cases the manual replace() chain didn't (all control chars
    // U+0000-U+001F, e.g. backspace / form-feed).
    FString MessagesJsonString;
    if (InInitialMessages.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> MessagesArray;
        MessagesArray.Reserve(InInitialMessages.Num());
        for (const FInoLiteRtLmMessage& Msg : InInitialMessages)
        {
            const TCHAR* RoleStr = (Msg.Role == EInoLiteRtLmMessageRole::Assistant)
                ? TEXT("assistant") : TEXT("user");

            const TSharedRef<FJsonObject> MsgObj = MakeShared<FJsonObject>();
            MsgObj->SetStringField(TEXT("role"), RoleStr);
            MsgObj->SetStringField(TEXT("content"), Msg.Content);
            MessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
        }

        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&MessagesJsonString);
        FJsonSerializer::Serialize(MessagesArray, Writer);
    }
    const FTCHARToUTF8 MessagesJsonUtf8(*MessagesJsonString);
    const char* const MessagesCStr =
        MessagesJsonString.IsEmpty() ? nullptr : MessagesJsonUtf8.Get();

    // Create the native conversation config.
    //
    // As of LiteRT-LM SHA 4dbbf937 the C API split the old multi-arg
    // create() into a no-arg create() + a family of setter functions.
    // We mirror the previous behaviour by calling each setter only when
    // we actually have a value (so the C side falls back to its own
    // defaults for unset fields, matching what the old call did when a
    // parameter was nullptr / false).
    LiteRtLmConversationConfig* NativeConvConfig = litert_lm_conversation_config_create();

    if (NativeConvConfig == nullptr)
    {
        if (SessionConfig != nullptr) litert_lm_session_config_delete(SessionConfig);
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Conversation: Initialize FAILED — "
                    "litert_lm_conversation_config_create returned NULL"));
        return;
    }

    if (SessionConfig != nullptr)
    {
        litert_lm_conversation_config_set_session_config(NativeConvConfig, SessionConfig);
    }
    if (SystemMessageCStr != nullptr)
    {
        litert_lm_conversation_config_set_system_message(NativeConvConfig, SystemMessageCStr);
    }
    if (ToolsJsonCStr != nullptr)
    {
        litert_lm_conversation_config_set_tools(NativeConvConfig, ToolsJsonCStr);
    }
    if (MessagesCStr != nullptr)
    {
        litert_lm_conversation_config_set_messages(NativeConvConfig, MessagesCStr);
    }
    litert_lm_conversation_config_set_enable_constrained_decoding(
        NativeConvConfig, bEnableConstrainedDecoding);

    // SessionConfig is COPIED into the conversation config by the
    // setter (see LiteRT-LM c/engine.cc:set_session_config —
    // `config->session_config = *session_config->config`). After every
    // setter has run, the SessionConfig pointer is no longer used and
    // can be released. Doing so before conversation_create avoids a
    // per-conversation leak.
    if (SessionConfig != nullptr)
    {
        litert_lm_session_config_delete(SessionConfig);
        SessionConfig = nullptr;
    }

    // Create the native conversation.
    UE_LOG(LogInoAgents, Verbose,
           TEXT("LiteRtLm: Conversation: calling native litert_lm_conversation_create"));
    LiteRtLmConversation* NativeConv = litert_lm_conversation_create(InEngine, NativeConvConfig);

    if (NativeConv == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Conversation: Initialize FAILED — "
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
    Worker = MakeUnique<FInoLiteRtLmConversationWorker>(
        TWeakObjectPtr<UInoLiteRtLmConversation>(this),
        TWeakObjectPtr<UInoLiteRtLmSubsystem>(InSubsystem),
        NativeConv,
        NativeConvConfig);

    // Seed history from initial messages if provided.
    History = InInitialMessages;

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Conversation: initialized (system_message=%s, history=%d msgs, tools_enabled=%s)"),
           InConfig.SystemMessage.IsEmpty() ? TEXT("<none>") : TEXT("<set>"),
           History.Num(),
           bEnableConstrainedDecoding ? TEXT("yes") : TEXT("no"));

    // Log the first 200 chars of the system message so we can verify
    // the right prompt is reaching the native layer. Truncated to
    // avoid flooding the log on very long prompts.
    if (!InConfig.SystemMessage.IsEmpty())
    {
        const FString Preview = InConfig.SystemMessage.Left(200);
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Conversation: system_message preview: \"%s%s\""),
               *Preview,
               InConfig.SystemMessage.Len() > 200 ? TEXT("...") : TEXT(""));
    }
}

void UInoLiteRtLmConversation::SendMessageAsync(const FString& UserText)
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Conversation: SendMessageAsync called (user_text_len=%d, history_before=%d, worker=%s)"),
           UserText.Len(), History.Num(), Worker.IsValid() ? TEXT("present") : TEXT("null"));

    // Broadcast the user's text synchronously BEFORE any validation so
    // chat UIs echo every submitted message regardless of whether the
    // downstream send then fails. Observers that only care about
    // successful sends can additionally bind OnComplete.
    OnUserMessage.Broadcast(UserText);

    if (!Worker.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Conversation: SendMessageAsync FAILED — worker is null "
                    "(conversation was not initialized)"));
        OnError.Broadcast(TEXT("Conversation not initialized"));
        return;
    }

    // Clear per-send state from a prior send.
    SentenceBuffer.Empty();
    SentenceScanOffset     = 0;
    TokenSquareDepth       = 0;
    TokenAngleDepth        = 0;
    TokenCurlyDepth        = 0;
    TokenParenDepth        = 0;
    bTokenInsideSlash      = false;
    bTokenInsidePipe       = false;
    bTokenInsideHash       = false;
    bTokenEatOneWhitespace = false;

    // Record the user message in history (original text, not augmented).
    FInoLiteRtLmMessage UserMsg;
    UserMsg.Role = EInoLiteRtLmMessageRole::User;
    UserMsg.Content = UserText;
    History.Add(MoveTemp(UserMsg));

    // Prepend context to the user message as plain text. The LiteRT-LM
    // C API's extra_context parameter is injected as Jinja2 template
    // variables, but the Gemma 4 chat template (embedded in the model
    // file) does not reference any custom variables — only bos_token,
    // messages, tools, add_generation_prompt, and enable_thinking.
    // Verified by extracting the template from the .litertlm binary.
    //
    // Prepending to the user message is the reliable path: the model
    // always sees message content. Context is wrapped in [Context] tags
    // so the model can distinguish it from the user's actual request.
    const FString ContextBlock = BuildMergedContext();
    if (ContextBlock.IsEmpty())
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("LiteRtLm: Conversation: SendMessageAsync enqueueing user message (no context)"));
        Worker->EnqueueMessage(UserText);
    }
    else
    {
        const FString AugmentedText = FString::Printf(
            TEXT("[Context]\n%s[/Context]\n\n%s"),
            *ContextBlock, *UserText);
        UE_LOG(LogInoAgents, Verbose,
               TEXT("LiteRtLm: Conversation: SendMessageAsync enqueueing user message with context (context %d bytes)"),
               ContextBlock.Len());
        Worker->EnqueueMessage(AugmentedText);
    }
}

void UInoLiteRtLmConversation::Cancel()
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Conversation: Cancel called (worker=%s, streaming=%s)"),
           Worker.IsValid() ? TEXT("present") : TEXT("null"),
           (Worker.IsValid() && Worker->IsStreamInFlight()) ? TEXT("yes") : TEXT("no"));

    if (!Worker.IsValid())
    {
        // No worker, no stream in flight, nothing to cancel. Not an error.
        return;
    }

    Worker->Cancel();
}

bool UInoLiteRtLmConversation::IsStreamingInFlight() const
{
    if (!Worker.IsValid())
    {
        // Zombie conversation (Shutdown called or Initialize never ran).
        // Not streaming because it can't stream.
        return false;
    }
    return Worker->IsStreamInFlight();
}

void UInoLiteRtLmConversation::Shutdown()
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Conversation: Shutdown called (worker=%s)"),
           Worker.IsValid() ? TEXT("present") : TEXT("null"));

    // Resetting the TUniquePtr invokes ~FInoLiteRtLmConversationWorker,
    // which cancels any in-flight stream, joins the worker thread,
    // and destroys the native LiteRT-LM resources. No delegate
    // invocation list is touched — safe to call from inside one of
    // this conversation's own delegate handlers.
    //
    // Second call is a no-op because TUniquePtr::Reset on an already-
    // null pointer does nothing. BeginDestroy calls the same
    // Worker.Reset() again, which also becomes a no-op after Shutdown.
    Worker.Reset();

    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Conversation: Shutdown complete"));
}

void UInoLiteRtLmConversation::SubmitDeferredToolResult(
    FName ToolCallId, const FString& ResultJson)
{
    // Stubbed — see the header doc-comment. All tool calls are
    // currently resolved synchronously inside the worker's agent loop
    // via a game-thread FEvent round-trip, so there is nothing for
    // this method to unblock. The future implementation will use
    // ToolCallId to look up a pending TPromise stored on the worker
    // and fulfil it with ResultJson.
    UE_LOG(LogInoAgents, Warning,
           TEXT("LiteRtLm: Conversation: SubmitDeferredToolResult(%s) ignored — deferred tool "
                "results are not yet implemented. All tools currently "
                "execute synchronously on the game thread from inside "
                "the worker's agent loop. (ResultJson length: %d)"),
           *ToolCallId.ToString(), ResultJson.Len());
}

void UInoLiteRtLmConversation::BeginDestroy()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Conversation: BeginDestroy (worker=%s, history=%d)"),
           Worker.IsValid() ? TEXT("present") : TEXT("null"),
           History.Num());

    // Resetting the TUniquePtr invokes ~FInoLiteRtLmConversationWorker,
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
// Context
// ======================================================================

void UInoLiteRtLmConversation::SetSystemContext(const FString& Key, const FString& Value)
{
    SystemContextMap.Add(Key, Value);
}

void UInoLiteRtLmConversation::SetUserContext(const FString& Key, const FString& Value)
{
    UserContextMap.Add(Key, Value);
}

void UInoLiteRtLmConversation::AddSystemContext(const FString& Key, const FString& Value)
{
    SystemContextMap.Add(Key, Value);
}

void UInoLiteRtLmConversation::AddUserContext(const FString& Key, const FString& Value)
{
    UserContextMap.Add(Key, Value);
}

FString UInoLiteRtLmConversation::GetSystemContext(const FString& Key) const
{
    const FString* Found = SystemContextMap.Find(Key);
    return Found ? *Found : FString();
}

FString UInoLiteRtLmConversation::GetUserContext(const FString& Key) const
{
    const FString* Found = UserContextMap.Find(Key);
    return Found ? *Found : FString();
}

void UInoLiteRtLmConversation::ClearSystemContext()
{
    SystemContextMap.Reset();
}

void UInoLiteRtLmConversation::ClearUserContext()
{
    UserContextMap.Reset();
}

FString UInoLiteRtLmConversation::BuildMergedContext() const
{
    if (SystemContextMap.Num() == 0 && UserContextMap.Num() == 0)
    {
        return FString();
    }

    // Build a human-readable context block. Each key-value pair is
    // rendered as "key: value" on its own line, grouped by source.
    //
    // Shape:
    //   Game state:
    //   - location: Dragon's Peak Castle
    //   - time_of_day: midnight
    //   Player state:
    //   - player_name: Sir Lancelot
    //   - player_class: knight
    FString Result;

    // Context keys/values are game/player-supplied and therefore
    // untrusted — sanitize each so an embedded Gemma-4 control token
    // can't escape the [Context] block and spoof a turn / tool boundary.
    if (SystemContextMap.Num() > 0)
    {
        Result += TEXT("Game state:\n");
        for (const auto& Pair : SystemContextMap)
        {
            Result += FString::Printf(TEXT("- %s: %s\n"),
                *InoLrlSanitizeForPrompt(Pair.Key),
                *InoLrlSanitizeForPrompt(Pair.Value));
        }
    }

    if (UserContextMap.Num() > 0)
    {
        Result += TEXT("Player state:\n");
        for (const auto& Pair : UserContextMap)
        {
            Result += FString::Printf(TEXT("- %s: %s\n"),
                *InoLrlSanitizeForPrompt(Pair.Key),
                *InoLrlSanitizeForPrompt(Pair.Value));
        }
    }

    return Result;
}

void UInoLiteRtLmConversation::RecordAssistantMessage(const FString& Text)
{
    check(IsInGameThread());

    FInoLiteRtLmMessage AssistantMsg;
    AssistantMsg.Role = EInoLiteRtLmMessageRole::Assistant;
    AssistantMsg.Content = Text;
    History.Add(MoveTemp(AssistantMsg));
}

// ======================================================================
// Sentence detection
// ======================================================================

FString UInoLiteRtLmConversation::StripTags(const FString& Raw) const
{
    // One-shot sibling of FilterCleanToken for OnSentence.CleanText. Operates
    // on a complete sentence (no chunk boundaries), so all state is local.
    const EInoLiteRtLmTagStrip Flags = static_cast<EInoLiteRtLmTagStrip>(TagStripFlags);

    const bool bSquare = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::SquareBrackets);
    const bool bAngle  = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::AngleBrackets);
    const bool bCurly  = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::CurlyBraces);
    const bool bParen  = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::Parentheses);
    const bool bSlash  = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::ForwardSlashes);
    const bool bPipe   = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::Pipes);
    const bool bHash   = EnumHasAnyFlags(Flags, EInoLiteRtLmTagStrip::Hashes);

    FString Clean;
    Clean.Reserve(Raw.Len());

    uint16 SquareDepth = 0, AngleDepth = 0, CurlyDepth = 0, ParenDepth = 0;
    bool   bInSlash = false, bInPipe = false, bInHash = false;

    auto InsideAny = [&]() -> bool
    {
        return SquareDepth > 0 || AngleDepth > 0 || CurlyDepth > 0 || ParenDepth > 0
            || bInSlash || bInPipe || bInHash;
    };

    for (const TCHAR Ch : Raw)
    {
        // Asymmetric openers / closers.
        if (bSquare && Ch == TEXT('['))                        { SquareDepth++; continue; }
        if (bSquare && Ch == TEXT(']') && SquareDepth > 0)     { SquareDepth--; continue; }
        if (bAngle  && Ch == TEXT('<'))                        { AngleDepth++;  continue; }
        if (bAngle  && Ch == TEXT('>') && AngleDepth > 0)      { AngleDepth--;  continue; }
        if (bCurly  && Ch == TEXT('{'))                        { CurlyDepth++;  continue; }
        if (bCurly  && Ch == TEXT('}') && CurlyDepth > 0)      { CurlyDepth--;  continue; }
        if (bParen  && Ch == TEXT('('))                        { ParenDepth++;  continue; }
        if (bParen  && Ch == TEXT(')') && ParenDepth > 0)      { ParenDepth--;  continue; }

        // Symmetric toggles (same char opens and closes).
        if (bSlash  && Ch == TEXT('/')) { bInSlash = !bInSlash; continue; }
        if (bPipe   && Ch == TEXT('|')) { bInPipe  = !bInPipe;  continue; }
        if (bHash   && Ch == TEXT('#')) { bInHash  = !bInHash;  continue; }

        // Inside any currently-open tag: skip content.
        if (InsideAny())
        {
            continue;
        }

        Clean.AppendChar(Ch);
    }

    return Clean.TrimStartAndEnd();
}

FString UInoLiteRtLmConversation::FilterCleanToken(const FString& RawChunk)
{
    // Stateful per-character filter. Member state (the seven tag trackers
    // and bTokenEatOneWhitespace) persists across chunks so delimiters that
    // straddle a chunk boundary still close cleanly end-to-end.
    //
    //   1. Stripped delimiter pairs are controlled by TagStripFlags:
    //        - asymmetric pairs ([], <>, {}, ()) are nest-aware via depth
    //          counters; identical types can nest ("[outer [inner] tail]")
    //          and different types cross freely ("[<foo>]");
    //        - symmetric pairs (//, ||, ##) toggle an inside/outside bool
    //          on each occurrence (non-nesting by construction).
    //
    //   2. After any tag closes (depth → 0 or toggle → outside) AND nothing
    //      else is still open, arm "eat one whitespace": the next char, if
    //      it is space / newline / CR / tab, is dropped — just one, not a
    //      run. No other events arm this flag; sentence-split flags are
    //      purely for boundary detection in the accumulator and do NOT
    //      affect CleanText.
    //
    //   3. Anything else is emitted verbatim.
    //
    // Example with TagStripFlags = SquareBrackets:
    //     Raw:    "[hello]   test. More"
    //     Clean:  "  test. More"
    //              ^^              one WS eaten after ']' (3→2 spaces);
    //                              '.' and its trailing space both preserved.
    //
    // RawText in OnToken.Broadcast is always the untouched original chunk.
    FString Clean;
    Clean.Reserve(RawChunk.Len());

    const EInoLiteRtLmTagStrip TagFlags =
        static_cast<EInoLiteRtLmTagStrip>(TagStripFlags);

    const bool bSquare = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::SquareBrackets);
    const bool bAngle  = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::AngleBrackets);
    const bool bCurly  = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::CurlyBraces);
    const bool bParen  = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::Parentheses);
    const bool bSlash  = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::ForwardSlashes);
    const bool bPipe   = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::Pipes);
    const bool bHash   = EnumHasAnyFlags(TagFlags, EInoLiteRtLmTagStrip::Hashes);

    auto InsideAnyTag = [&]() -> bool
    {
        return TokenSquareDepth > 0 || TokenAngleDepth > 0
            || TokenCurlyDepth  > 0 || TokenParenDepth  > 0
            || bTokenInsideSlash || bTokenInsidePipe || bTokenInsideHash;
    };
    auto OnTagClosed = [&]()
    {
        // Only arm eat-one once we've actually surfaced from all tags —
        // nested closes inside other tags don't arm (there's still a wrapper).
        if (!InsideAnyTag())
        {
            bTokenEatOneWhitespace = true;
        }
    };

    for (const TCHAR Ch : RawChunk)
    {
        // --- Asymmetric tag delimiters: depth-based (nestable) ---
        if (bSquare && Ch == TEXT('['))                            { TokenSquareDepth++; continue; }
        if (bSquare && Ch == TEXT(']') && TokenSquareDepth > 0)    { TokenSquareDepth--; OnTagClosed(); continue; }
        if (bAngle  && Ch == TEXT('<'))                            { TokenAngleDepth++;  continue; }
        if (bAngle  && Ch == TEXT('>') && TokenAngleDepth  > 0)    { TokenAngleDepth--;  OnTagClosed(); continue; }
        if (bCurly  && Ch == TEXT('{'))                            { TokenCurlyDepth++;  continue; }
        if (bCurly  && Ch == TEXT('}') && TokenCurlyDepth  > 0)    { TokenCurlyDepth--;  OnTagClosed(); continue; }
        if (bParen  && Ch == TEXT('('))                            { TokenParenDepth++;  continue; }
        if (bParen  && Ch == TEXT(')') && TokenParenDepth  > 0)    { TokenParenDepth--;  OnTagClosed(); continue; }

        // --- Symmetric tag delimiters: toggle ---
        if (bSlash && Ch == TEXT('/'))
        {
            bTokenInsideSlash = !bTokenInsideSlash;
            if (!bTokenInsideSlash) OnTagClosed();
            continue;
        }
        if (bPipe && Ch == TEXT('|'))
        {
            bTokenInsidePipe = !bTokenInsidePipe;
            if (!bTokenInsidePipe) OnTagClosed();
            continue;
        }
        if (bHash && Ch == TEXT('#'))
        {
            bTokenInsideHash = !bTokenInsideHash;
            if (!bTokenInsideHash) OnTagClosed();
            continue;
        }

        // Inside any currently-open tag: drop the content entirely.
        if (InsideAnyTag())
        {
            continue;
        }

        // --- Eat-one mode: consume at most ONE whitespace char ---
        if (bTokenEatOneWhitespace)
        {
            bTokenEatOneWhitespace = false;
            if (Ch == TEXT(' ') || Ch == TEXT('\n') || Ch == TEXT('\r') || Ch == TEXT('\t'))
            {
                continue;
            }
            // Non-whitespace: flag cleared, fall through and emit it.
        }

        // --- Emit ordinary character ---
        Clean.AppendChar(Ch);
    }

    return Clean;
}

void UInoLiteRtLmConversation::SetSentenceSplitFlags(int32 NewFlags)
{
    SentenceSplitFlags = NewFlags;
}

void UInoLiteRtLmConversation::SetTagStripFlags(int32 NewFlags)
{
    TagStripFlags = NewFlags;
}

void UInoLiteRtLmConversation::AccumulateTokenForSentence(const FString& Chunk)
{
    check(IsInGameThread());

    SentenceBuffer += Chunk;

    // Split on whichever boundaries the caller has enabled in
    // SentenceSplitFlags. If no flags are set, this loop exits
    // immediately on the first iteration and everything accumulates
    // until FlushSentenceBuffer runs at OnComplete time (one big
    // sentence for the full response).
    const EInoLiteRtLmSentenceSplit Flags =
        static_cast<EInoLiteRtLmSentenceSplit>(SentenceSplitFlags);

    // Punctuation+space delimiters paired with their enum flag so we
    // can skip the Find for any flag the caller disabled.
    struct FSplitCandidate { const TCHAR* Delim; int32 Len; EInoLiteRtLmSentenceSplit Flag; };
    static const FSplitCandidate PunctDelims[] = {
        { TEXT(". "), 2, EInoLiteRtLmSentenceSplit::Period      },
        { TEXT(", "), 2, EInoLiteRtLmSentenceSplit::Comma       },
        { TEXT("? "), 2, EInoLiteRtLmSentenceSplit::Question    },
        { TEXT("! "), 2, EInoLiteRtLmSentenceSplit::Exclamation },
        { TEXT("; "), 2, EInoLiteRtLmSentenceSplit::Semicolon   },
        { TEXT(": "), 2, EInoLiteRtLmSentenceSplit::Colon       },
    };

    // Incremental scan: a delimiter that wasn't present before can only
    // newly appear inside the just-appended text or straddling the old
    // tail. Delimiters are at most 2 chars, so re-including the single
    // last previously-scanned char is enough to catch a straddle. This
    // turns the per-token cost from O(buffer) to O(chunk).
    int32 SearchStart =
        FMath::Clamp(SentenceScanOffset, 0, FMath::Max(0, SentenceBuffer.Len()));

    while (true)
    {
        int32 SplitIndex = INDEX_NONE;
        int32 SplitLen   = 0;

        if (EnumHasAnyFlags(Flags, EInoLiteRtLmSentenceSplit::Newline))
        {
            const int32 NlIdx = SentenceBuffer.Find(
                TEXT("\n"), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchStart);
            if (NlIdx != INDEX_NONE)
            {
                SplitIndex = NlIdx;
                SplitLen   = 1;
            }
        }

        for (const FSplitCandidate& Candidate : PunctDelims)
        {
            if (!EnumHasAnyFlags(Flags, Candidate.Flag))
            {
                continue;
            }
            const int32 Idx = SentenceBuffer.Find(
                Candidate.Delim, ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchStart);
            if (Idx != INDEX_NONE
                && (SplitIndex == INDEX_NONE || Idx < SplitIndex))
            {
                SplitIndex = Idx;
                SplitLen   = Candidate.Len;
            }
        }

        if (SplitIndex == INDEX_NONE)
        {
            // Nothing to split yet. Remember how far we got so the next
            // token doesn't rescan it; keep 1 char of overlap so a
            // 2-char delimiter split across the next append is caught.
            SentenceScanOffset = FMath::Max(0, SentenceBuffer.Len() - 1);
            break;
        }

        // Include the punctuation in the emitted sentence (split
        // AFTER it). For newlines, drop the newline itself.
        const int32 SentenceEnd = (SplitLen == 2) ? SplitIndex + 1 : SplitIndex;
        FString RawLine = SentenceBuffer.Left(SentenceEnd).TrimStartAndEnd();
        SentenceBuffer.MidInline(SplitIndex + SplitLen);

        // Buffer was reindexed by the trim — the remaining text must be
        // rescanned from its new start.
        SearchStart        = 0;
        SentenceScanOffset = 0;

        if (!RawLine.IsEmpty())
        {
            const FString CleanLine = StripTags(RawLine);
            OnSentence.Broadcast(RawLine, CleanLine);
        }

        // Boundary signal — fires on every configured split, not just
        // newlines. Consumers that wanted the OLD newline-only behavior
        // can check for trailing '\n' inside their OnSentence handler.
        OnSentenceBoundary.Broadcast();
    }
}

void UInoLiteRtLmConversation::FlushSentenceBuffer()
{
    check(IsInGameThread());

    const FString Remainder = SentenceBuffer.TrimStartAndEnd();
    SentenceBuffer.Empty();
    SentenceScanOffset = 0;

    if (!Remainder.IsEmpty())
    {
        const FString CleanRemainder = StripTags(Remainder);
        OnSentence.Broadcast(Remainder, CleanRemainder);
    }
}
