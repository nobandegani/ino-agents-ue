// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "InoLiteRtLmConversation.generated.h"

class UInoLiteRtLmSubsystem;
class FInoLiteRtLmConversationWorker;

// Forward declarations of opaque native types from LiteRT-LM's C API.
// We deliberately do NOT include "litert/lm/engine.h" here — that would pull
// the C API surface into every translation unit that uses conversations.
extern "C" {
    struct LiteRtLmEngine;
    struct LiteRtLmConversation;
    struct LiteRtLmConversationConfig;
}

/**
 * Bitmask of boundaries that trigger OnSentence broadcasts.
 *
 * The conversation accumulates streamed tokens in an internal buffer
 * and fires OnSentence every time a configured boundary is seen. All
 * punctuation flags match the punctuation character followed by a
 * single space — so "3.14" doesn't split, but "Done. Next" does.
 *
 * Enable as many or as few as you like. Empty (0) = OnSentence only
 * fires at end-of-response (FlushSentenceBuffer) with the full reply
 * as one sentence.
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EInoLiteRtLmSentenceSplit : uint8
{
    None        = 0          UMETA(Hidden),
    Newline     = 0x01       UMETA(DisplayName = "Newline (\\n)"),
    Period      = 0x02       UMETA(DisplayName = "Period + space (\". \")"),
    Comma       = 0x04       UMETA(DisplayName = "Comma + space (\", \")"),
    Question    = 0x08       UMETA(DisplayName = "Question + space (\"? \")"),
    Exclamation = 0x10       UMETA(DisplayName = "Exclamation + space (\"! \")"),
    Semicolon   = 0x20       UMETA(DisplayName = "Semicolon + space (\"; \")"),
    Colon       = 0x40       UMETA(DisplayName = "Colon + space (\": \")"),
};
ENUM_CLASS_FLAGS(EInoLiteRtLmSentenceSplit);

/**
 * Bitmask of delimiter pairs that get stripped from OnToken.CleanText and
 * OnSentence.CleanText. OnToken.RawText and OnSentence.RawText are never
 * affected — the raw surfaces always preserve the original characters so
 * TTS or markup-aware consumers still see the tags.
 *
 * Asymmetric pairs (square / angle / curly / parens) are nest-aware: the
 * filter tracks depth per pair, so "[outer [inner] tail]" collapses to ""
 * correctly. Symmetric pairs (slash / pipe / hash) toggle an inside/outside
 * bool on each occurrence — be aware that stray single occurrences of those
 * characters in ordinary prose will unbalance the toggle (e.g. enabling
 * ForwardSlashes and then encountering a URL like "http://example.com/path"
 * would strip the segment between the first two '/'s).
 *
 * A tag-close also arms the "eat one whitespace" rule (same rule as
 * sentence-split punctuation): the single space / newline immediately after
 * a closed tag gets dropped, so "[hello] world" cleans to "world".
 *
 * Default: SquareBrackets | CurlyBraces (matches the original hard-coded
 * behaviour from before this flag existed).
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EInoLiteRtLmTagStrip : uint8
{
    None             = 0      UMETA(Hidden),
    SquareBrackets   = 0x01   UMETA(DisplayName = "Square brackets [ ... ]"),
    AngleBrackets    = 0x02   UMETA(DisplayName = "Angle brackets < ... >"),
    CurlyBraces      = 0x04   UMETA(DisplayName = "Curly braces { ... }"),
    Parentheses      = 0x08   UMETA(DisplayName = "Parentheses ( ... )"),
    ForwardSlashes   = 0x10   UMETA(DisplayName = "Forward slashes / ... /"),
    Pipes            = 0x20   UMETA(DisplayName = "Pipes | ... |"),
    Hashes           = 0x40   UMETA(DisplayName = "Hashes # ... #"),
    // 0x80 reserved for the "|\" pair pending clarification of open/close chars.
};
ENUM_CLASS_FLAGS(EInoLiteRtLmTagStrip);

/**
 * One stateful conversation with a LiteRT-LM model.
 *
 * Construction: via UInoLiteRtLmSubsystem::CreateConversation. Do NOT construct
 * directly with NewObject — the subsystem must populate the internal native
 * conversation + worker thread.
 *
 * Lifetime: owned by whoever holds a UPROPERTY reference to it. When the
 * last reference drops, UE garbage collection eventually calls BeginDestroy,
 * which joins the worker thread and destroys native resources. For
 * deterministic cleanup in tests, release the reference and call
 * CollectGarbage(RF_NoFlags, true) — but in normal gameplay, letting GC
 * handle it is fine.
 *
 * Threading: SendMessageAsync returns immediately. Generation happens on
 * a pinned worker thread owned by the conversation; chunks and final
 * responses marshal back to the game thread via AsyncTask before any
 * delegate broadcasts. Blueprint code only ever sees delegates firing on
 * the game thread — there is no thread-safety burden on callers.
 *
 * Current surface: SendMessageAsync (streaming via
 * litert_lm_conversation_send_message_stream on the worker), OnToken
 * (per-chunk text delta), OnComplete (once at end with full
 * accumulated text from the final round), OnError (once on failure),
 * OnToolCalled (diagnostic, fires after each tool round-trip),
 * Cancel, Shutdown, SubmitDeferredToolResult (stubbed for future).
 *
 * Per-send ordering guarantee: OnToken fires zero or more times on
 * the game thread, in order, for text chunks from the FINAL round
 * of the agent loop only (tokens emitted during intermediate
 * tool-call rounds are suppressed — the caller never sees the
 * model's tool-call JSON as OnToken). OnToolCalled fires zero or
 * more times on the game thread for each executed tool call, in
 * order. After all rounds complete, exactly one of OnComplete or
 * OnError fires, always strictly AFTER every OnToken / OnToolCalled
 * broadcast for the same send. A caller that only wants the final
 * answer can ignore OnToken and OnToolCalled and read the text
 * passed to OnComplete.
 */
UCLASS(BlueprintType)
class INOAGENTS_API UInoLiteRtLmConversation : public UObject
{
    GENERATED_BODY()

public:
    // Out-of-line constructors / destructor. Required because the private
    // TUniquePtr<FInoLiteRtLmConversationWorker> member references a
    // forward-declared type: if any compiler-synthesized constructor or
    // destructor were emitted in the .gen.cpp (where only the forward
    // decl is visible), TDefaultDelete<FInoLiteRtLmConversationWorker>::
    // operator() would try to `delete` an incomplete type and fail with
    // C4150. UHT emits TWO implicit ctors in .gen.cpp — the default one
    // AND the FVTableHelper hot-reload helper — so BOTH must be declared
    // here and defined in InoLiteRtLmConversation.cpp (which #includes the
    // worker header), along with the destructor.
    UInoLiteRtLmConversation();
    UInoLiteRtLmConversation(FVTableHelper& Helper);
    virtual ~UInoLiteRtLmConversation();

    //~ UObject interface
    virtual void BeginDestroy() override;
    //~ End UObject interface

    // ------------------------------------------------------------------
    // Entry points
    // ------------------------------------------------------------------

    /**
     * Send a user message to the conversation. Returns immediately.
     * As the model generates the response, OnToken fires zero or more
     * times on the game thread with each chunk. When the stream ends,
     * exactly one of OnComplete (success — full accumulated text) or
     * OnError (failure or cancellation) fires on the game thread.
     *
     * Calling SendMessageAsync while a previous send is still in flight
     * enqueues the new message — it will be processed once the current
     * one completes. Messages are FIFO.
     *
     * Internally uses litert_lm_conversation_send_message_stream on a
     * dedicated worker thread; each chunk marshals back to the game
     * thread via AsyncTask before any delegate broadcasts. Callers in
     * Blueprint or C++ never see off-thread delegates.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SendMessageAsync(const FString& UserText);

    /** Strip delimiter-pair tags from a string, honouring TagStripFlags
     *  (so "[cheerfully] Hello!" becomes "Hello!" when SquareBrackets is
     *  enabled). Non-static because it reads the per-conversation
     *  TagStripFlags — the Blueprint node takes a conversation target. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    FString StripTags(const FString& Raw) const;

    // ------------------------------------------------------------------
    // History
    // ------------------------------------------------------------------

    /** Get the full conversation history (user + assistant messages). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="InoAgents|LiteRT-LM")
    const TArray<FInoLiteRtLmMessage>& GetHistory() const { return History; }

    /** Clear the tracked history. Does NOT affect the native conversation's
     *  KV cache — the model still remembers prior turns. This only clears
     *  the UE-side record used for save/load. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void ClearHistory() { History.Reset(); }

    // ------------------------------------------------------------------
    // Context (per-turn injection, not stored in chat history)
    // ------------------------------------------------------------------

    /** Set a system context value (game/world state). */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void SetSystemContext(const FString& Key, const FString& Value);

    /** Set a user context value (player state). */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void SetUserContext(const FString& Key, const FString& Value);

    /** Add a system context value. Same as SetSystemContext. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void AddSystemContext(const FString& Key, const FString& Value);

    /** Add a user context value. Same as SetUserContext. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void AddUserContext(const FString& Key, const FString& Value);

    /** Get a system context value. Empty if not found. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM|Context")
    FString GetSystemContext(const FString& Key) const;

    /** Get a user context value. Empty if not found. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM|Context")
    FString GetUserContext(const FString& Key) const;

    /** Clear all system context. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void ClearSystemContext();

    /** Clear all user context. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void ClearUserContext();

    /**
     * Cancel the in-flight stream, if any. Safe to call at any time
     * from the game thread; safe to call with no stream in flight
     * (no-op). After Cancel, the currently-running send will emit
     * OnError("Cancelled by caller") on the game thread once the
     * native cancel has propagated through LiteRT-LM (typically
     * within a few hundred milliseconds).
     *
     * Cancel does NOT drain the queue — any messages that have been
     * enqueued but not yet started will still run. To abort everything
     * AND release resources, call Shutdown instead.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void Cancel();

    /**
     * True while a SendMessageAsync is actively generating — i.e.
     * from the moment the worker's stream has been kicked off to
     * the moment the round's terminal callback fires. Useful for
     * Blueprint UI that wants to disable the "Send" button while
     * the model is replying, or to show a typing indicator.
     *
     * Returns false before the first send, between rounds of the
     * agent loop (briefly), after OnComplete / OnError has fired,
     * and after Shutdown has been called.
     *
     * Safe to call from any thread, but the intended caller is the
     * game thread from Blueprint / Tick.
     */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    bool IsStreamingInFlight() const;

    /**
     * Immediately release the worker thread and native LiteRT-LM
     * resources. After calling Shutdown the conversation is a
     * "zombie": SendMessageAsync will log an error and fail, Cancel
     * becomes a no-op, and no further delegate broadcasts will fire.
     * The UObject itself remains alive until natural garbage
     * collection reclaims it.
     *
     * Intended for callers that need DETERMINISTIC teardown without
     * waiting for GC — for example:
     *   - Tests that want native resources released before the next
     *     assertion / test run.
     *   - Scene transitions that need the LiteRT-LM engine available
     *     for a new conversation on the next frame.
     *   - Manual lifetime control in C++ code that can't tolerate
     *     GC latency.
     *
     * In normal Blueprint gameplay you usually do NOT need to call
     * this — just drop the last reference to the conversation and
     * let GC handle it. The worker + native teardown happens during
     * BeginDestroy, which is safe in gameplay because the last
     * reference drop typically happens outside of any delegate
     * broadcast.
     *
     * Safe to call multiple times (second call is a no-op). Safe to
     * call while a stream is in flight — the destructor cancels the
     * stream and joins the worker thread before returning, same as
     * BeginDestroy.
     *
     * IMPORTANT: calling Shutdown from inside one of the
     * conversation's own delegate handlers (OnComplete, OnError,
     * OnToken) is supported — the internal Worker.Reset() does not
     * touch the delegate invocation list and is immune to the
     * reentrancy issues that make CollectGarbage() unsafe in the
     * same context.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void Shutdown();

    /**
     * Advanced: submit a tool result asynchronously, after the
     * conversation worker has already returned control to the game
     * thread waiting for it. Reserved for a future iteration where
     * tool implementations need to do their own async work (network
     * calls, disk I/O, user confirmation dialogs) before answering.
     *
     * STUBBED — all tool calls are currently executed synchronously
     * on the game thread inside the conversation's agent loop, which
     * blocks the worker thread until Execute returns. Calling this
     * method logs a warning and is otherwise a no-op.
     *
     * The method exists in the header now so that consumers of the
     * plugin can already wire it up in Blueprint — a future commit
     * will add the worker-side state machine that actually consumes
     * the deferred result.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SubmitDeferredToolResult(FName ToolCallId, const FString& ResultJson);

    // ------------------------------------------------------------------
    // Delegates (multicast, Blueprint-bindable)
    // ------------------------------------------------------------------

    /**
     * Fires at the START of every SendMessageAsync call, synchronously,
     * with the user's text as passed in. Useful for chat UI that wants
     * to echo the user's message immediately (without waiting for the
     * model's first token) and for logging / analytics.
     *
     * Always on the game thread, fires BEFORE OnToken / OnComplete /
     * OnError for the same send.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmUserMessage OnUserMessage;

    /**
     * Fires zero or more times per SendMessageAsync call as the model
     * streams out its response. Each broadcast delivers two strings:
     *
     *   RawText   — the chunk as the model produced it, including any
     *               [emotion] or [audio] tags.
     *   CleanText — the same chunk with all [bracketed] tags stripped.
     *
     * Always on the game thread, in order.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmToken OnToken;

    /**
     * Fires exactly once per SendMessageAsync call on success. FullText
     * is the assistant's response concatenated from every content part
     * of type="text". Always on the game thread. Always fires AFTER
     * every OnToken broadcast for the same send — the worker orders
     * the AsyncTask dispatches so the game-thread observer sees
     * tokens in order and then the completion.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmComplete OnComplete;

    /**
     * Fires exactly once per SendMessageAsync call on failure. Mutually
     * exclusive with OnComplete — exactly one of them fires per send.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmError OnError;

    /**
     * Fires each time the accumulated streaming tokens cross a newline
     * boundary (\n). Delivers two strings:
     *   RawText   — the line with [emotion] / [audio] tags intact
     *               (send to ElevenLabs for expressive delivery)
     *   CleanText — the line with all [bracketed] tags stripped
     *               (use for subtitles, chat display, etc.)
     *
     * At OnComplete time, any remaining text that hasn't crossed a
     * newline is flushed as a final OnSentence broadcast, so the
     * concatenation of every OnSentence always equals the full
     * assistant response.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmSentence OnSentence;

    /**
     * Fires after each OnSentence broadcast — at EVERY split boundary
     * configured via SentenceSplitFlags (newline + any enabled
     * punctuation + space). Use this for sentence-paced effects that
     * don't need the text payload (animation cues, viseme resets,
     * subtitle fade-in markers).
     *
     * Renamed from OnNewLine — the delegate now fires on every
     * configured split, not only newlines, so the new name reflects
     * the broader behavior.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmSentenceBoundary OnSentenceBoundary;

    // ------------------------------------------------------------------
    // Sentence split configuration
    // ------------------------------------------------------------------

    /**
     * Bitmask of boundaries that trigger OnSentence broadcasts while
     * streaming tokens. See EInoLiteRtLmSentenceSplit for the flag set.
     *
     * Default: Newline | Period | Comma | Question | Exclamation
     * (matches the original hard-coded behavior).
     *
     * Changing mid-stream is allowed but takes effect on the NEXT
     * boundary search — already-accumulated buffer contents aren't
     * re-scanned.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="InoAgents|LiteRT-LM",
              meta = (Bitmask, BitmaskEnum = "/Script/InoAgents.EInoLiteRtLmSentenceSplit"))
    int32 SentenceSplitFlags =
          static_cast<int32>(EInoLiteRtLmSentenceSplit::Newline)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Period)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Comma)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Question)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Exclamation);

    /** Replace the current split flags. Pass any combination of
     *  EInoLiteRtLmSentenceSplit values OR'd into an int32. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SetSentenceSplitFlags(
        UPARAM(meta = (Bitmask, BitmaskEnum = "/Script/InoAgents.EInoLiteRtLmSentenceSplit"))
        int32 NewFlags);

    /** Current split flags as an int32 bitmask. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    int32 GetSentenceSplitFlags() const { return SentenceSplitFlags; }

    /**
     * Bitmask of delimiter pairs that get stripped from CleanText surfaces.
     * See EInoLiteRtLmTagStrip for the flag set.
     *
     * Default: SquareBrackets | CurlyBraces (matches the pre-flag hardcoded
     * behavior — OnToken.CleanText / OnSentence.CleanText had [bracket] and
     * {curly} tags stripped unconditionally).
     *
     * Changing mid-stream is allowed; the change takes effect on the NEXT
     * character processed. Already-accumulated tag state (e.g. if we are
     * currently inside a tag) is preserved so switching off a flag while
     * inside a tag of that type still closes it cleanly.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="InoAgents|LiteRT-LM",
              meta = (Bitmask, BitmaskEnum = "/Script/InoAgents.EInoLiteRtLmTagStrip"))
    int32 TagStripFlags =
          static_cast<int32>(EInoLiteRtLmTagStrip::SquareBrackets)
        | static_cast<int32>(EInoLiteRtLmTagStrip::CurlyBraces);

    /** Replace the current tag-strip flags. Pass any combination of
     *  EInoLiteRtLmTagStrip values OR'd into an int32. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SetTagStripFlags(
        UPARAM(meta = (Bitmask, BitmaskEnum = "/Script/InoAgents.EInoLiteRtLmTagStrip"))
        int32 NewFlags);

    /** Current tag-strip flags as an int32 bitmask. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    int32 GetTagStripFlags() const { return TagStripFlags; }

    /**
     * Diagnostic event: fires AFTER a tool has been executed and its
     * result has been fed back into the conversation. Broadcast on
     * the game thread with the tool name, the arguments JSON the
     * model supplied, and the result JSON the tool returned.
     *
     * This is purely observational, for debug UI, logs, and
     * validation assertions in smoke tests. The tool call itself is
     * handled transparently inside the conversation worker — you do
     * NOT need to bind OnToolCalled to make tool calls work. You
     * only bind it if you want visibility into which tools ran.
     *
     * Fires once per tool call executed during a single
     * SendMessageAsync (so zero or more times per send, depending on
     * whether the model decided to use a tool and how many rounds
     * the agent loop ran). All broadcasts fire strictly before the
     * terminal OnComplete / OnError broadcast for the same send.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnInoLiteRtLmToolCalled OnToolCalled;

    // ------------------------------------------------------------------
    // Internal — called by UInoLiteRtLmSubsystem::CreateConversation only
    // ------------------------------------------------------------------

    /**
     * Set up native resources and spawn the worker thread. Called exactly
     * once per instance, by the subsystem's CreateConversation factory.
     * Game thread only.
     */
    void Initialize(
        UInoLiteRtLmSubsystem* InSubsystem,
        LiteRtLmEngine* InEngine,
        const FInoLiteRtLmModelConfig& InConfig,
        const TArray<FInoLiteRtLmMessage>& InInitialMessages = TArray<FInoLiteRtLmMessage>());

    // ------------------------------------------------------------------
    // Sentence detection (called from worker's game-thread token path)
    // ------------------------------------------------------------------

    /**
     * Append a token chunk and broadcast OnSentence for each complete
     * sentence boundary found. Called automatically from the worker's
     * game-thread OnToken dispatch — callers do NOT need to call this
     * themselves; it runs as part of the normal token flow.
     *
     * Game thread only.
     */
    void AccumulateTokenForSentence(const FString& Chunk);

    /**
     * Broadcast any remaining text in SentenceBuffer as a final sentence.
     * Called from the game-thread OnComplete dispatch so no trailing
     * text is lost.
     *
     * Game thread only.
     */
    void FlushSentenceBuffer();

    /** Record an assistant response in the tracked history. Called by
     *  the worker's DispatchCompleteOnGameThread, game thread only. */
    void RecordAssistantMessage(const FString& Text);

    /** Filter a raw token chunk through the bracket-tag state machine.
     *  Returns the clean portion (characters outside [tags]). Handles
     *  tags that span multiple tokens. Game thread only. */
    FString FilterCleanToken(const FString& RawChunk);

private:
    UPROPERTY()
    TWeakObjectPtr<UInoLiteRtLmSubsystem> Subsystem;

    /** Tracked conversation history for save/load. Appended to in
     *  SendMessageAsync (user) and the OnComplete handler (assistant).
     *  Seeded from InitialMessages in Initialize if provided. */
    UPROPERTY()
    TArray<FInoLiteRtLmMessage> History;

    /** Context key-value maps. Merged on each SendMessageAsync. */
    TMap<FString, FString> SystemContextMap;
    TMap<FString, FString> UserContextMap;
    FString BuildMergedContext() const;

    // Per-chunk cross-boundary tag state for FilterCleanToken. Matches the
    // seven delimiter pairs exposed by EInoLiteRtLmTagStrip. Asymmetric pairs
    // carry a depth so nesting works ("[outer [inner] tail]"); symmetric pairs
    // carry a bool that toggles on each occurrence. All reset between sends.
    uint16 TokenSquareDepth = 0;   // [ ... ]
    uint16 TokenAngleDepth  = 0;   // < ... >
    uint16 TokenCurlyDepth  = 0;   // { ... }
    uint16 TokenParenDepth  = 0;   // ( ... )
    bool   bTokenInsideSlash = false;  // / ... /
    bool   bTokenInsidePipe  = false;  // | ... |
    bool   bTokenInsideHash  = false;  // # ... #

    // When true, FilterCleanToken drops the very next character if — and
    // only if — it is a space or newline; then clears the flag. Armed
    // exclusively by closing a tag of any type currently enabled in
    // TagStripFlags. Sentence-split flags never affect CleanText — they
    // are purely for OnSentence boundary detection in the accumulator.
    // Persists across chunks so a tag-close and its trailing space can
    // land in separate token chunks. Only ONE whitespace char is ever
    // eaten — runs of whitespace keep everything past the first.
    bool bTokenEatOneWhitespace = false;

    /** Rolling buffer for sentence detection. Accumulates tokens until
     *  a sentence-ending delimiter is found, at which point the complete
     *  sentence is broadcast via OnSentence and the buffer shifts to
     *  whatever follows the delimiter. */
    FString SentenceBuffer;

    // Worker owns the pinned thread + native conversation + native config.
    // TUniquePtr because FInoLiteRtLmConversationWorker is a plain C++ class,
    // not a UObject. Destroyed when this UObject's BeginDestroy runs, which
    // joins the worker thread before releasing native resources.
    TUniquePtr<FInoLiteRtLmConversationWorker> Worker;
};
