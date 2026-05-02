// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogInoQwen3ASRLiteRT, Log, All);

/**
 * InoQwen3ASRLiteRT — Qwen3-ASR-0.6B speech-to-text via LiteRT.
 *
 * Wraps the int8-quantized 5-second-window export of Qwen3-ASR-0.6B
 * (qwen3_asr_0.6b_5s_i8.tflite, ~794 MB) and exposes a UE subsystem with
 * an async TranscribeAsync(WavPath) API. The model is encoder-decoder with
 * autoregressive decoding — the runner orchestrates the encoder pass + a
 * KV-cached decode loop, then detokenizes with the Qwen3 BPE tokenizer.
 *
 * - LiteRT export: https://huggingface.co/litert-community/Qwen3-ASR-0.6B
 * - Base model:    https://huggingface.co/Qwen/Qwen3-ASR-0.6B
 *
 * The LiteRT runtime DLLs are pre-loaded at PreLoadingScreen by the
 * sibling InoLiteRT plugin, so every LiteRt* C API symbol is callable
 * by the time this module's StartupModule runs.
 */
class FInoQwen3ASRLiteRTModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
