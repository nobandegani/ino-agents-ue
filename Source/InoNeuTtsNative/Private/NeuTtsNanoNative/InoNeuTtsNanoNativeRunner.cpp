// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNanoNativeRunner.h"

#include "InoAgentsLog.h"
#include "InoLlama.h"                       // FLlamaCppApi + GetApi (pulls in llama.h, from sibling InoLlama plugin)
#include "Onnx/InoOnnxSession.h"            // FInoOnnxSession::Create
#include "Onnx/InoOnnxTypes.h"              // FInoOnnxSessionOptions + EInoOnnxProvider

#include "Containers/StringConv.h"          // FTCHARToUTF8
#include "HAL/PlatformFileManager.h"        // file-stat for friendly error
#include "HAL/PlatformTime.h"               // FPlatformTime::Seconds for per-stage timing

// ============================================================================
// Local helpers
// ============================================================================

namespace
{
    /**
     * Scan the vocab for a specific literal token piece and return its
     * token id. Returns -1 if not found.
     *
     * NeuTTS Nano's "<|SPEECH_GENERATION_END|>" is added-vocabulary — a
     * special token Neuphonic grafted onto Qwen2's base vocab during
     * fine-tuning. We can't ask for it by `llama_vocab_eos`; we have to
     * walk the vocab and string-compare each piece. ~65k iterations,
     * runs once at Create time, cost is negligible.
     *
     * llama_token_to_piece renders control / special tokens as their
     * literal <|...|> form when special=true, which is what we need to
     * match against the NeuTTS control tokens.
     */
    int32 FindSpecialTokenId(
        const InoAgents::LlamaCpp::FLlamaCppApi& Api,
        const struct llama_vocab* Vocab,
        const char* Needle)
    {
        const int32 N = Api.llama_vocab_n_tokens(Vocab);
        // 64 bytes is comfortably larger than any NeuTTS control token
        // ("<|SPEECH_GENERATION_END|>" is 26 chars; speech tokens like
        // "<|speech_65535|>" are 17).
        char Buf[128];
        const int32 NeedleLen = (int32)FCStringAnsi::Strlen(Needle);

        for (int32 i = 0; i < N; ++i)
        {
            const int32 Len = Api.llama_token_to_piece(
                Vocab, (llama_token)i, Buf, (int32)sizeof(Buf) - 1,
                /*lstrip=*/0, /*special=*/true);
            if (Len == NeedleLen)
            {
                // llama_token_to_piece doesn't null-terminate — compare
                // by length + memcmp only.
                if (FMemory::Memcmp(Buf, Needle, (SIZE_T)Len) == 0)
                {
                    return i;
                }
            }
        }
        return -1;
    }

    /**
     * Translate FInoNeuTtsNanoNativePerformanceOptions into the generic
     * FInoOnnxSessionOptions consumed by FInoOnnxSession::Create.
     *
     * Per-platform provider list:
     *   - Windows: [DirectMl, Cpu] when bPreferDirectMl, else [Cpu]
     *   - Android: [Xnnpack, Cpu] when bUseXnnpack,   else [Cpu]
     *   - Other:  [Cpu]
     *
     * Providers are registered in priority order. If the preferred one
     * fails to register at runtime (no D3D12 device, corrupt DML
     * install, Android AAR missing a kernel, etc.) ORT silently falls
     * through to CPU — the session still loads, just without the
     * accelerator.
     */
    FInoOnnxSessionOptions MakeNeuCodecOptions(
        const FInoNeuTtsNanoNativePerformanceOptions& Perf)
    {
        FInoOnnxSessionOptions Options;

#if PLATFORM_WINDOWS
        if (Perf.bPreferDirectMl)
        {
            Options.ExecutionProviders = {
                EInoOnnxProvider::DirectMl,
                EInoOnnxProvider::Cpu
            };
            Options.DirectMlAdapterIndex = Perf.DirectMlAdapterIndex;
        }
        else
        {
            Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
        }
#elif PLATFORM_ANDROID
        if (Perf.bUseXnnpack)
        {
            Options.ExecutionProviders = {
                EInoOnnxProvider::Xnnpack,
                EInoOnnxProvider::Cpu
            };
        }
        else
        {
            Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
        }
#else
        // Linux / macOS / iOS: CPU-only until we stage prebuilt ORT
        // with platform-specific accelerators.
        Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
#endif

        Options.GraphOptimization        = EInoOnnxGraphOptimizationLevel::All;
        Options.IntraOpThreadCount       = Perf.DecoderIntraOpThreadCount;
        Options.InterOpThreadCount       = Perf.DecoderInterOpThreadCount;
        Options.bEnableProfiling         = Perf.bEnableOrtProfiling;
        // Verbose = 0, Warning (ORT default) = 2. -1 leaves ORT's default in place.
        Options.LogSeverityLevel         = Perf.bEnableVerboseOrtLogging ? 0 : -1;

        return Options;
    }
} // namespace

// ============================================================================
// Create + dtor
// ============================================================================

TUniquePtr<FInoNeuTtsNanoNativeRunner> FInoNeuTtsNanoNativeRunner::Create(
    const FString& BackboneGgufPath,
    const FString& CodecOnnxPath,
    const FInoNeuTtsNanoNativeModelConfig& Config,
    FString& OutError)
{
    const double CreateStartTime = FPlatformTime::Seconds();
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: Create begin (variant=%s, backbone=%s, codec=%s)"),
           *NeuTtsNanoNativeVariantToString(Config.Variant),
           *BackboneGgufPath, *CodecOnnxPath);

    const InoAgents::LlamaCpp::FLlamaCppApi* Api = InoAgents::LlamaCpp::GetApi();
    if (Api == nullptr)
    {
        OutError = TEXT("llama.cpp runtime is not initialised — "
                        "InoAgents::LlamaCpp::GetApi() returned nullptr. "
                        "Check module startup logs.");
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNanoNative: Runner: Create FAILED: %s"), *OutError);
        return nullptr;
    }

    // Friendly pre-check: both files must exist + be non-zero.
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!PF.FileExists(*BackboneGgufPath) || PF.FileSize(*BackboneGgufPath) <= 0)
    {
        OutError = FString::Printf(
            TEXT("Backbone GGUF missing or empty: %s"), *BackboneGgufPath);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNanoNative: Runner: Create FAILED: %s"), *OutError);
        return nullptr;
    }
    if (!PF.FileExists(*CodecOnnxPath) || PF.FileSize(*CodecOnnxPath) <= 0)
    {
        OutError = FString::Printf(
            TEXT("Codec ONNX missing or empty: %s"), *CodecOnnxPath);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNanoNative: Runner: Create FAILED: %s"), *OutError);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Verbose,
           TEXT("NeuTtsNanoNative: Runner: pre-check ok — backbone=%lld bytes, codec=%lld bytes"),
           PF.FileSize(*BackboneGgufPath), PF.FileSize(*CodecOnnxPath));

    // -----------------------------------------------------------------
    // 1. Load the backbone GGUF.
    //
    // ModelParams knobs wired from Config.Performance:
    //   use_mmap   — near-zero-cost load via memory-map (usually leave on)
    //   use_mlock  — lock pages in RAM (opt-in for low-memory hosts)
    // Everything else stays at the llama.cpp defaults.
    // -----------------------------------------------------------------
    struct llama_model_params ModelParams = Api->llama_model_default_params();
    ModelParams.n_gpu_layers = Config.NumGpuLayers;
    ModelParams.use_mmap     = Config.Performance.bUseMmap;
    ModelParams.use_mlock    = Config.Performance.bUseMlock;

    const double BackboneStart = FPlatformTime::Seconds();
    const FTCHARToUTF8 BackboneUtf8(*BackboneGgufPath);
    struct llama_model* NewModel =
        Api->llama_model_load_from_file(BackboneUtf8.Get(), ModelParams);
    const double BackboneMs = (FPlatformTime::Seconds() - BackboneStart) * 1000.0;
    if (NewModel == nullptr)
    {
        OutError = FString::Printf(
            TEXT("llama_model_load_from_file failed for %s "
                 "(check log above for llama.cpp diagnostic)."),
            *BackboneGgufPath);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNanoNative: Runner: Create FAILED (after %.0f ms): %s"),
               BackboneMs, *OutError);
        return nullptr;
    }

    // Log model description for diagnostic purposes.
    {
        char Desc[256] = {};
        const int32 DescLen = Api->llama_model_desc(NewModel, Desc, (size_t)sizeof(Desc));
        if (DescLen > 0 && DescLen < (int32)sizeof(Desc))
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("NeuTtsNanoNative: Runner: backbone loaded in %.0f ms — %s"),
                   BackboneMs, UTF8_TO_TCHAR(Desc));
        }
        else
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("NeuTtsNanoNative: Runner: backbone loaded in %.0f ms"),
                   BackboneMs);
        }
    }

    // -----------------------------------------------------------------
    // 2. Create a context with the requested size.
    //
    // CtxParams knobs wired from Config.Performance:
    //   n_threads         — token-generation thread pool (0 = llama default)
    //   n_threads_batch   — prompt-prefill thread pool (0 = inherit n_threads)
    //   flash_attn_type   — AUTO (let llama.cpp decide) or DISABLED
    // Everything else stays at the llama.cpp defaults (n_ctx gets the
    // requested size, n_batch / n_ubatch / KV-cache dtype all use
    // llama.cpp's conservative defaults, fine for NeuTTS workloads).
    // -----------------------------------------------------------------
    struct llama_context_params CtxParams = Api->llama_context_default_params();
    CtxParams.n_ctx = (uint32_t)Config.NumContextTokens;

    if (Config.Performance.LlmThreadCount > 0)
    {
        CtxParams.n_threads = Config.Performance.LlmThreadCount;
    }
    if (Config.Performance.LlmBatchThreadCount > 0)
    {
        CtxParams.n_threads_batch = Config.Performance.LlmBatchThreadCount;
    }
    CtxParams.flash_attn_type = Config.Performance.bFlashAttention
        ? LLAMA_FLASH_ATTN_TYPE_AUTO
        : LLAMA_FLASH_ATTN_TYPE_DISABLED;

    const double CtxStart = FPlatformTime::Seconds();
    struct llama_context* NewCtx = Api->llama_init_from_model(NewModel, CtxParams);
    const double CtxMs = (FPlatformTime::Seconds() - CtxStart) * 1000.0;
    if (NewCtx == nullptr)
    {
        Api->llama_model_free(NewModel);
        OutError = FString::Printf(
            TEXT("llama_init_from_model failed (n_ctx=%d)."),
            Config.NumContextTokens);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNanoNative: Runner: Create FAILED: %s"), *OutError);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: llama_context initialised in %.0f ms (n_ctx=%d)"),
           CtxMs, Config.NumContextTokens);

    // -----------------------------------------------------------------
    // 3. Resolve the stop-token id.
    // -----------------------------------------------------------------
    const struct llama_vocab* Vocab = Api->llama_model_get_vocab(NewModel);
    const int32 StopId = FindSpecialTokenId(*Api, Vocab, "<|SPEECH_GENERATION_END|>");
    if (StopId < 0)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTtsNanoNative: Runner: <|SPEECH_GENERATION_END|> token not found in vocab. "
                    "Synthesis will fall back to llama_vocab_is_eog + MaxNewTokens cap"));
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNanoNative: Runner: stop token <|SPEECH_GENERATION_END|> resolved as id=%d"),
               StopId);
    }

    // -----------------------------------------------------------------
    // 4. Create the NeuCodec ONNX session.
    //
    // MakeNeuCodecOptions translates Config.Performance into the
    // generic FInoOnnxSessionOptions the InoOnnx layer consumes —
    // execution-provider list (Cpu / DirectMl / Xnnpack), thread counts,
    // profiling + verbose-logging toggles, DirectML adapter index.
    // -----------------------------------------------------------------
    const double CodecStart = FPlatformTime::Seconds();
    FString OrtErr;
    TUniquePtr<FInoOnnxSession> NewSession =
        FInoOnnxSession::Create(
            CodecOnnxPath,
            MakeNeuCodecOptions(Config.Performance),
            &OrtErr);
    const double CodecMs = (FPlatformTime::Seconds() - CodecStart) * 1000.0;
    if (!NewSession)
    {
        Api->llama_free(NewCtx);
        Api->llama_model_free(NewModel);
        OutError = FString::Printf(
            TEXT("FInoOnnxSession::Create failed for %s: %s"),
            *CodecOnnxPath, *OrtErr);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNanoNative: Runner: codec session create FAILED after %.0f ms: %s"),
               CodecMs, *OutError);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: NeuCodec ONNX session created in %.0f ms"),
           CodecMs);

    // -----------------------------------------------------------------
    // 5. Package.
    // -----------------------------------------------------------------
    TUniquePtr<FInoNeuTtsNanoNativeRunner> Runner(new FInoNeuTtsNanoNativeRunner());
    Runner->Model        = NewModel;
    Runner->Context      = NewCtx;
    Runner->CodecSession = MoveTemp(NewSession);
    Runner->StopTokenId  = StopId;

    const double TotalMs = (FPlatformTime::Seconds() - CreateStartTime) * 1000.0;
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: ready in %.0f ms (n_ctx=%u, stop_token_id=%d, "
                "n_gpu_layers=%d, llm_threads=%d/%d (gen/batch), flash_attn=%s, "
                "mmap=%s, mlock=%s)"),
           TotalMs,
           Api->llama_n_ctx(NewCtx), StopId, Config.NumGpuLayers,
           Config.Performance.LlmThreadCount,
           Config.Performance.LlmBatchThreadCount,
           Config.Performance.bFlashAttention ? TEXT("auto") : TEXT("disabled"),
           Config.Performance.bUseMmap  ? TEXT("on") : TEXT("off"),
           Config.Performance.bUseMlock ? TEXT("on") : TEXT("off"));

#if PLATFORM_WINDOWS
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: decoder providers=%s, intra/inter_op=%d/%d, "
                "profiling=%s, verbose_ort=%s"),
           Config.Performance.bPreferDirectMl
               ? TEXT("[DirectMl, Cpu]") : TEXT("[Cpu]"),
           Config.Performance.DecoderIntraOpThreadCount,
           Config.Performance.DecoderInterOpThreadCount,
           Config.Performance.bEnableOrtProfiling      ? TEXT("on") : TEXT("off"),
           Config.Performance.bEnableVerboseOrtLogging ? TEXT("on") : TEXT("off"));
#elif PLATFORM_ANDROID
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: decoder providers=%s, intra/inter_op=%d/%d, "
                "profiling=%s, verbose_ort=%s"),
           Config.Performance.bUseXnnpack
               ? TEXT("[Xnnpack, Cpu]") : TEXT("[Cpu]"),
           Config.Performance.DecoderIntraOpThreadCount,
           Config.Performance.DecoderInterOpThreadCount,
           Config.Performance.bEnableOrtProfiling      ? TEXT("on") : TEXT("off"),
           Config.Performance.bEnableVerboseOrtLogging ? TEXT("on") : TEXT("off"));
#endif

    return Runner;
}

FInoNeuTtsNanoNativeRunner::~FInoNeuTtsNanoNativeRunner()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNanoNative: Runner: dtor — releasing codec session, llama_context, llama_model"));

    const InoAgents::LlamaCpp::FLlamaCppApi* Api = InoAgents::LlamaCpp::GetApi();

    // Codec session first — purely a TUniquePtr reset, no cross-runtime
    // ordering dependency.
    CodecSession.Reset();

    // Then context + model in reverse-construction order.
    if (Api != nullptr)
    {
        if (Context != nullptr)
        {
            Api->llama_free(Context);
            Context = nullptr;
        }
        if (Model != nullptr)
        {
            Api->llama_model_free(Model);
            Model = nullptr;
        }
    }
    else
    {
        // Api gone mid-teardown (module shutdown edge). We've lost the
        // vtable so we can't safely free the llama.cpp handles — leak
        // them rather than call into freed code. The process is tearing
        // down anyway; OS reclaims memory.
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTtsNanoNative: Runner: dtor — llama.cpp vtable is null "
                    "(module shutdown in progress?); llama_model / llama_context "
                    "intentionally leaked — OS will reclaim"));
    }
}
