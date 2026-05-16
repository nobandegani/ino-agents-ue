// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// LiteRT-LM C API stub implementations for non-shipped platforms.
// ============================================================================
//
// LiteRT-LM is provided by the separate `InoLiteRT` plugin, which ships
// built libraries for Windows (Win64), Android (arm64-v8a + x86_64), macOS
// (arm64), and iOS (arm64 device + sim_arm64). The plugin's LiteRT-LM
// consumer code (subsystem, conversation, worker, smoke tests) compiles on
// every platform — but on platforms where InoLiteRT has no library to link
// against, the linker would fail.
//
// This file provides empty stub implementations of every litert_lm_* symbol
// declared in litert/lm/engine.h. The stubs compile into the main InoAgents
// module on non-shipped platforms only, satisfying the linker while
// guaranteeing that every runtime entry point fails gracefully:
//
//   - Constructor-style functions (engine_create, conversation_create, etc.)
//     return nullptr. Callers already check for nullptr and dispatch an
//     OnError delegate.
//   - Void teardown functions (engine_delete, etc.) are no-ops — safe to
//     call on a nullptr or with mismatched state.
//   - Int-returning functions return 0 (or a non-zero failure code for
//     stream-start functions).
//
//   - Windows + Android + macOS + iOS: skipped here; InoLiteRT supplies
//     the real .dll / .so / .dylib / .framework binary.
//   - Linux: not yet ported by InoLiteRT, stubs still apply.
//
// Status note: on Linux, calling any LiteRT-LM feature will produce an
// immediate "Native engine failed" error via the subsystem's OnLoaded
// delegate. ElevenLabs, the streaming-audio component, and all other
// plugin features unrelated to local LLM inference continue to work
// normally on that platform.

#if !PLATFORM_WINDOWS && !PLATFORM_ANDROID && !PLATFORM_IOS && !PLATFORM_MAC

#include "CoreMinimal.h"

#include "litert/lm/engine.h"

// ----- Session config ----------------------------------------------------

extern "C" LiteRtLmSessionConfig* litert_lm_session_config_create()
{
    return nullptr;
}

extern "C" void litert_lm_session_config_set_max_output_tokens(
    LiteRtLmSessionConfig* /*config*/, int /*max_output_tokens*/)
{
}

extern "C" void litert_lm_session_config_set_apply_prompt_template(
    LiteRtLmSessionConfig* /*config*/, bool /*apply_prompt_template*/)
{
}

extern "C" void litert_lm_session_config_set_sampler_params(
    LiteRtLmSessionConfig* /*config*/,
    const LiteRtLmSamplerParams* /*sampler_params*/)
{
}

extern "C" void litert_lm_session_config_delete(LiteRtLmSessionConfig* /*config*/)
{
}

// ----- Conversation config ----------------------------------------------

extern "C" LiteRtLmConversationConfig* litert_lm_conversation_config_create()
{
    return nullptr;
}

extern "C" void litert_lm_conversation_config_set_session_config(
    LiteRtLmConversationConfig*  /*config*/,
    const LiteRtLmSessionConfig* /*session_config*/)
{
}

extern "C" void litert_lm_conversation_config_set_system_message(
    LiteRtLmConversationConfig* /*config*/, const char* /*system_message_json*/)
{
}

extern "C" void litert_lm_conversation_config_set_tools(
    LiteRtLmConversationConfig* /*config*/, const char* /*tools_json*/)
{
}

extern "C" void litert_lm_conversation_config_set_messages(
    LiteRtLmConversationConfig* /*config*/, const char* /*messages_json*/)
{
}

extern "C" void litert_lm_conversation_config_set_enable_constrained_decoding(
    LiteRtLmConversationConfig* /*config*/, bool /*enable_constrained_decoding*/)
{
}

extern "C" void litert_lm_conversation_config_delete(
    LiteRtLmConversationConfig* /*config*/)
{
}

// ----- Logging -----------------------------------------------------------

extern "C" void litert_lm_set_min_log_level(int /*level*/)
{
}

// ----- Engine settings ---------------------------------------------------

extern "C" LiteRtLmEngineSettings* litert_lm_engine_settings_create(
    const char* /*model_path*/,
    const char* /*backend_str*/,
    const char* /*vision_backend_str*/,
    const char* /*audio_backend_str*/)
{
    return nullptr;
}

extern "C" void litert_lm_engine_settings_delete(
    LiteRtLmEngineSettings* /*settings*/)
{
}

extern "C" void litert_lm_engine_settings_set_max_num_tokens(
    LiteRtLmEngineSettings* /*settings*/, int /*max_num_tokens*/)
{
}

extern "C" void litert_lm_engine_settings_set_parallel_file_section_loading(
    LiteRtLmEngineSettings* /*settings*/, bool /*parallel_file_section_loading*/)
{
}

extern "C" void litert_lm_engine_settings_set_cache_dir(
    LiteRtLmEngineSettings* /*settings*/, const char* /*cache_dir*/)
{
}

extern "C" void litert_lm_engine_settings_set_activation_data_type(
    LiteRtLmEngineSettings* /*settings*/, int /*activation_data_type_int*/)
{
}

extern "C" void litert_lm_engine_settings_set_prefill_chunk_size(
    LiteRtLmEngineSettings* /*settings*/, int /*prefill_chunk_size*/)
{
}

extern "C" void litert_lm_engine_settings_enable_benchmark(
    LiteRtLmEngineSettings* /*settings*/)
{
}

extern "C" void litert_lm_engine_settings_set_num_prefill_tokens(
    LiteRtLmEngineSettings* /*settings*/, int /*num_prefill_tokens*/)
{
}

extern "C" void litert_lm_engine_settings_set_num_decode_tokens(
    LiteRtLmEngineSettings* /*settings*/, int /*num_decode_tokens*/)
{
}

extern "C" void litert_lm_engine_settings_set_enable_speculative_decoding(
    LiteRtLmEngineSettings* /*settings*/, bool /*enable_speculative_decoding*/)
{
}

// ----- Engine lifecycle --------------------------------------------------

extern "C" LiteRtLmEngine* litert_lm_engine_create(
    const LiteRtLmEngineSettings* /*settings*/)
{
    return nullptr;
}

extern "C" void litert_lm_engine_delete(LiteRtLmEngine* /*engine*/)
{
}

// ----- Session lifecycle + generation -----------------------------------

extern "C" LiteRtLmSession* litert_lm_engine_create_session(
    LiteRtLmEngine* /*engine*/, LiteRtLmSessionConfig* /*config*/)
{
    return nullptr;
}

extern "C" void litert_lm_session_delete(LiteRtLmSession* /*session*/)
{
}

extern "C" int litert_lm_session_run_prefill(
    LiteRtLmSession*           /*session*/,
    const LiteRtLmInputData*   /*inputs*/,
    size_t                     /*num_inputs*/)
{
    return -1;
}

extern "C" LiteRtLmResponses* litert_lm_session_run_decode(
    LiteRtLmSession* /*session*/)
{
    return nullptr;
}

extern "C" LiteRtLmResponses* litert_lm_session_run_text_scoring(
    LiteRtLmSession* /*session*/,
    const char**     /*target_text*/,
    size_t           /*num_targets*/,
    bool             /*store_token_lengths*/)
{
    return nullptr;
}

extern "C" void litert_lm_session_cancel_process(LiteRtLmSession* /*session*/)
{
}

extern "C" LiteRtLmResponses* litert_lm_session_generate_content(
    LiteRtLmSession*         /*session*/,
    const LiteRtLmInputData* /*inputs*/,
    size_t                   /*num_inputs*/)
{
    return nullptr;
}

extern "C" int litert_lm_session_generate_content_stream(
    LiteRtLmSession*         /*session*/,
    const LiteRtLmInputData* /*inputs*/,
    size_t                   /*num_inputs*/,
    LiteRtLmStreamCallback   /*callback*/,
    void*                    /*callback_data*/)
{
    return -1;  // non-zero = failed to start stream
}

// ----- Responses ---------------------------------------------------------

extern "C" void litert_lm_responses_delete(LiteRtLmResponses* /*responses*/)
{
}

extern "C" int litert_lm_responses_get_num_candidates(
    const LiteRtLmResponses* /*responses*/)
{
    return 0;
}

extern "C" const char* litert_lm_responses_get_response_text_at(
    const LiteRtLmResponses* /*responses*/, int /*index*/)
{
    return nullptr;
}

// ----- Benchmark info ----------------------------------------------------

extern "C" LiteRtLmBenchmarkInfo* litert_lm_session_get_benchmark_info(
    LiteRtLmSession* /*session*/)
{
    return nullptr;
}

extern "C" void litert_lm_benchmark_info_delete(
    LiteRtLmBenchmarkInfo* /*benchmark_info*/)
{
}

extern "C" double litert_lm_benchmark_info_get_time_to_first_token(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/)
{
    return 0.0;
}

extern "C" double litert_lm_benchmark_info_get_total_init_time_in_second(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/)
{
    return 0.0;
}

extern "C" int litert_lm_benchmark_info_get_num_prefill_turns(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/)
{
    return 0;
}

extern "C" int litert_lm_benchmark_info_get_num_decode_turns(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/)
{
    return 0;
}

extern "C" int litert_lm_benchmark_info_get_prefill_token_count_at(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/, int /*index*/)
{
    return 0;
}

extern "C" int litert_lm_benchmark_info_get_decode_token_count_at(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/, int /*index*/)
{
    return 0;
}

extern "C" double litert_lm_benchmark_info_get_prefill_tokens_per_sec_at(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/, int /*index*/)
{
    return 0.0;
}

extern "C" double litert_lm_benchmark_info_get_decode_tokens_per_sec_at(
    const LiteRtLmBenchmarkInfo* /*benchmark_info*/, int /*index*/)
{
    return 0.0;
}

// ----- Conversation ------------------------------------------------------

extern "C" LiteRtLmConversation* litert_lm_conversation_create(
    LiteRtLmEngine* /*engine*/, LiteRtLmConversationConfig* /*config*/)
{
    return nullptr;
}

extern "C" void litert_lm_conversation_delete(
    LiteRtLmConversation* /*conversation*/)
{
}

extern "C" LiteRtLmJsonResponse* litert_lm_conversation_send_message(
    LiteRtLmConversation* /*conversation*/,
    const char*           /*message_json*/,
    const char*           /*extra_context*/,
    const LiteRtLmConversationOptionalArgs* /*optional_args*/)
{
    return nullptr;
}

extern "C" void litert_lm_json_response_delete(LiteRtLmJsonResponse* /*response*/)
{
}

extern "C" const char* litert_lm_json_response_get_string(
    const LiteRtLmJsonResponse* /*response*/)
{
    return nullptr;
}

extern "C" int litert_lm_conversation_send_message_stream(
    LiteRtLmConversation*  /*conversation*/,
    const char*            /*message_json*/,
    const char*            /*extra_context*/,
    const LiteRtLmConversationOptionalArgs* /*optional_args*/,
    LiteRtLmStreamCallback /*callback*/,
    void*                  /*callback_data*/)
{
    return -1;  // non-zero = failed to start stream
}

extern "C" void litert_lm_conversation_cancel_process(
    LiteRtLmConversation* /*conversation*/)
{
}

extern "C" LiteRtLmBenchmarkInfo* litert_lm_conversation_get_benchmark_info(
    LiteRtLmConversation* /*conversation*/)
{
    return nullptr;
}

#endif  // !PLATFORM_WINDOWS && !PLATFORM_ANDROID && !PLATFORM_IOS && !PLATFORM_MAC
