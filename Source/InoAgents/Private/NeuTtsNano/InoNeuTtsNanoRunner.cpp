// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNanoRunner.h"

#include "InoAgentsLog.h"
#include "InoLlamaCppModule.h"              // FLlamaCppApi + GetApi (pulls in llama.h)
#include "Onnx/InoOnnxSession.h"            // FInoOnnxSession::Create
#include "Onnx/InoOnnxTypes.h"              // FInoOnnxSessionOptions + EInoOnnxProvider

#include "Containers/StringConv.h"          // FTCHARToUTF8
#include "HAL/PlatformFileManager.h"        // file-stat for friendly error

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
     * Choose the ONNX Runtime execution-provider list for the NeuCodec
     * decoder. Matches the Chatterbox pattern: CPU-first on Win64 to
     * avoid DirectML kernel-validation issues on large decoders;
     * XNNPACK-preferred on Android for ARM-NEON acceleration.
     *
     * A future milestone can expose this to callers via
     * FInoNeuTtsNanoModelConfig (e.g. bPreferDirectMl flag mirroring
     * Chatterbox) once we've verified which providers work with the
     * NeuCodec model specifically.
     */
    FInoOnnxSessionOptions MakeNeuCodecOptions()
    {
        FInoOnnxSessionOptions Options;

#if PLATFORM_ANDROID
        Options.ExecutionProviders = {
            EInoOnnxProvider::Xnnpack,
            EInoOnnxProvider::Cpu
        };
#else
        // Windows + everything else: CPU-only for v1. DirectML on the
        // NeuCodec decoder is untested; promote to Milestone 4+ after
        // validating kernel coverage.
        Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
#endif
        Options.GraphOptimization = EInoOnnxGraphOptimizationLevel::All;
        return Options;
    }
} // namespace

// ============================================================================
// Create + dtor
// ============================================================================

TUniquePtr<FInoNeuTtsNanoRunner> FInoNeuTtsNanoRunner::Create(
    const FString& BackboneGgufPath,
    const FString& CodecOnnxPath,
    const FInoNeuTtsNanoModelConfig& Config,
    FString& OutError)
{
    const InoAgents::LlamaCpp::FLlamaCppApi* Api = InoAgents::LlamaCpp::GetApi();
    if (Api == nullptr)
    {
        OutError = TEXT("llama.cpp runtime is not initialised — "
                        "InoAgents::LlamaCpp::GetApi() returned nullptr. "
                        "Check module startup logs.");
        return nullptr;
    }

    // Friendly pre-check: both files must exist + be non-zero.
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!PF.FileExists(*BackboneGgufPath) || PF.FileSize(*BackboneGgufPath) <= 0)
    {
        OutError = FString::Printf(
            TEXT("Backbone GGUF missing or empty: %s"), *BackboneGgufPath);
        return nullptr;
    }
    if (!PF.FileExists(*CodecOnnxPath) || PF.FileSize(*CodecOnnxPath) <= 0)
    {
        OutError = FString::Printf(
            TEXT("Codec ONNX missing or empty: %s"), *CodecOnnxPath);
        return nullptr;
    }

    // -----------------------------------------------------------------
    // 1. Load the backbone GGUF.
    // -----------------------------------------------------------------
    struct llama_model_params ModelParams = Api->llama_model_default_params();
    ModelParams.n_gpu_layers = Config.NumGpuLayers;

    const FTCHARToUTF8 BackboneUtf8(*BackboneGgufPath);
    struct llama_model* NewModel =
        Api->llama_model_load_from_file(BackboneUtf8.Get(), ModelParams);
    if (NewModel == nullptr)
    {
        OutError = FString::Printf(
            TEXT("llama_model_load_from_file failed for %s "
                 "(check log above for llama.cpp diagnostic)."),
            *BackboneGgufPath);
        return nullptr;
    }

    // Log model description for diagnostic purposes.
    {
        char Desc[256] = {};
        const int32 DescLen = Api->llama_model_desc(NewModel, Desc, (size_t)sizeof(Desc));
        if (DescLen > 0 && DescLen < (int32)sizeof(Desc))
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("NeuTTS Nano backbone loaded: %s"),
                   UTF8_TO_TCHAR(Desc));
        }
    }

    // -----------------------------------------------------------------
    // 2. Create a context with the requested size.
    // -----------------------------------------------------------------
    struct llama_context_params CtxParams = Api->llama_context_default_params();
    CtxParams.n_ctx = (uint32_t)Config.NumContextTokens;

    struct llama_context* NewCtx = Api->llama_init_from_model(NewModel, CtxParams);
    if (NewCtx == nullptr)
    {
        Api->llama_model_free(NewModel);
        OutError = FString::Printf(
            TEXT("llama_init_from_model failed (n_ctx=%d)."),
            Config.NumContextTokens);
        return nullptr;
    }

    // -----------------------------------------------------------------
    // 3. Resolve the stop-token id.
    // -----------------------------------------------------------------
    const struct llama_vocab* Vocab = Api->llama_model_get_vocab(NewModel);
    const int32 StopId = FindSpecialTokenId(*Api, Vocab, "<|SPEECH_GENERATION_END|>");
    if (StopId < 0)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTTS Nano: <|SPEECH_GENERATION_END|> token not found in vocab. "
                    "Synthesis will fall back to llama_vocab_is_eog + MaxNewTokens cap."));
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTTS Nano: stop token <|SPEECH_GENERATION_END|> resolved as id=%d"),
               StopId);
    }

    // -----------------------------------------------------------------
    // 4. Create the NeuCodec ONNX session.
    // -----------------------------------------------------------------
    FString OrtErr;
    TUniquePtr<FInoOnnxSession> NewSession =
        FInoOnnxSession::Create(CodecOnnxPath, MakeNeuCodecOptions(), &OrtErr);
    if (!NewSession)
    {
        Api->llama_free(NewCtx);
        Api->llama_model_free(NewModel);
        OutError = FString::Printf(
            TEXT("FInoOnnxSession::Create failed for %s: %s"),
            *CodecOnnxPath, *OrtErr);
        return nullptr;
    }

    // -----------------------------------------------------------------
    // 5. Package.
    // -----------------------------------------------------------------
    TUniquePtr<FInoNeuTtsNanoRunner> Runner(new FInoNeuTtsNanoRunner());
    Runner->Model        = NewModel;
    Runner->Context      = NewCtx;
    Runner->CodecSession = MoveTemp(NewSession);
    Runner->StopTokenId  = StopId;

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano Runner ready: n_ctx=%u, stop_token_id=%d, n_gpu_layers=%d"),
           Api->llama_n_ctx(NewCtx), StopId, Config.NumGpuLayers);

    return Runner;
}

FInoNeuTtsNanoRunner::~FInoNeuTtsNanoRunner()
{
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
               TEXT("NeuTTS Nano Runner dtor: llama.cpp vtable is null "
                    "(module shutdown in progress?). llama_model / llama_context "
                    "intentionally leaked — OS will reclaim."));
    }
}
