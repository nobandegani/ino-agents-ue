// Copyright 2026 Inoland. Licensed under the Apache License 2.0.
//
// LiteRtLm_exports.cc
//
// Force-reference every public litert_lm_* C API function so MSVC's linker
// pulls their defining .obj files out of the //c:engine_cpu static archive
// and into LiteRtLm.dll. Once pulled in, the __declspec(dllexport) annotations
// on those functions (from LITERT_LM_C_API_EXPORT in c/engine.h) drive the
// export table automatically.
//
// BACKGROUND
// ----------
// MSVC's linker only pulls in .obj files from a static library (.lib) when
// something already-included references them. LiteRT-LM's upstream .bazelrc
// disables whole-archive linking on Windows (build:windows --legacy_whole_archive=0)
// to avoid symbol conflicts (upstream bug b/469455895). So a cc_binary target
// that depends on //c:engine_cpu but has no code calling litert_lm_* functions
// gets a LiteRtLm.dll where those functions have been silently dropped during
// link, even though they are marked __declspec(dllexport) in the header.
//
// The canonical Windows fix is to take the address of each API function from
// code that IS definitely linked (like this translation unit, which is part of
// the cc_binary's own srcs rather than a static library). The references are
// stored in a volatile array so the compiler cannot prove them dead and
// eliminate them, and because taking a function's address requires the
// function to exist in the final binary, MSVC's linker pulls in the defining
// .obj files from //c:engine_cpu. Once linked, their dllexport annotations
// are respected and the symbols appear in the DLL's export table.
//
// MAINTENANCE
// -----------
// Keep kInoAgentsForceKeep[] in sync with every LITERT_LM_C_API_EXPORT
// function declared in c/engine.h. A missing entry results in that symbol
// NOT being exported from LiteRtLm.dll — the build succeeds silently, but
// callers fail at runtime (or at UE link time) with "undefined external".
// There is no static verification.
//
// When LiteRT-LM is bumped to a new version, re-extract the current list with:
//
//     awk '/^LITERT_LM_C_API_EXPORT$/{flag=1; next} flag{
//          match($0, /litert_lm_[a-zA-Z_0-9]+/);
//          print substr($0, RSTART, RLENGTH); flag=0}' c/engine.h
//
// and reconcile against this file.

#include "c/engine.h"

namespace {

using InoAgentsFn = void (*)(void);

// Volatile prevents dead-code elimination.
volatile InoAgentsFn kInoAgentsForceKeep[] = {
    // Session config
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_config_create),
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_config_set_max_output_tokens),
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_config_set_sampler_params),
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_config_delete),

    // Conversation config
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_config_create),
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_config_delete),

    // Logging
    reinterpret_cast<InoAgentsFn>(&litert_lm_set_min_log_level),

    // Engine settings
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_create),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_max_num_tokens),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_cache_dir),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_activation_data_type),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_prefill_chunk_size),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_enable_benchmark),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_num_prefill_tokens),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_num_decode_tokens),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_settings_set_parallel_file_section_loading),  // v0.10.2

    // Engine lifecycle
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_create),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_engine_create_session),

    // Session lifecycle + generation
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_generate_content),
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_generate_content_stream),

    // Responses
    reinterpret_cast<InoAgentsFn>(&litert_lm_responses_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_responses_get_num_candidates),
    reinterpret_cast<InoAgentsFn>(&litert_lm_responses_get_response_text_at),

    // Benchmark info
    reinterpret_cast<InoAgentsFn>(&litert_lm_session_get_benchmark_info),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_time_to_first_token),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_total_init_time_in_second),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_num_prefill_turns),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_num_decode_turns),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_prefill_token_count_at),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_decode_token_count_at),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_prefill_tokens_per_sec_at),
    reinterpret_cast<InoAgentsFn>(&litert_lm_benchmark_info_get_decode_tokens_per_sec_at),

    // Conversation
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_create),
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_send_message),
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_send_message_stream),
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_cancel_process),
    reinterpret_cast<InoAgentsFn>(&litert_lm_conversation_get_benchmark_info),

    // JSON response (from conversation API)
    reinterpret_cast<InoAgentsFn>(&litert_lm_json_response_delete),
    reinterpret_cast<InoAgentsFn>(&litert_lm_json_response_get_string),
};

}  // namespace

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD /*ul_reason_for_call*/,
                      LPVOID /*lpReserved*/) {
  return TRUE;
}

#endif  // _WIN32
