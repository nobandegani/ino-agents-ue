// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "NeuTTS/InoNeuTTSTypes.h"  // EInoNeuTTSBackend

// Forward-declare the LiteRT opaque handles via the same macros LiteRT
// uses internally — keeps this header free of `litert/c/*.h` includes
// so consumers don't drag in the full C API surface.
#ifdef __cplusplus
typedef class LiteRtEnvironmentT*    LiteRtEnvironment;
typedef class LiteRtModelT*          LiteRtModel;
typedef struct LiteRtOptionsT*       LiteRtOptions;
typedef class LiteRtCompiledModelT*  LiteRtCompiledModel;
#else
typedef struct LiteRtEnvironmentT*   LiteRtEnvironment;
typedef struct LiteRtModelT*         LiteRtModel;
typedef struct LiteRtOptionsT*       LiteRtOptions;
typedef struct LiteRtCompiledModelT* LiteRtCompiledModel;
#endif

/**
 * Bare-LiteRT C API wrapper around the NeuCodec decoder `.tflite`.
 *
 * Loads a NeuCodec decoder produced by
 * `Plugins/InoLiteRT/Convert/NeuCodec/scripts/convert_to_tflite.py`,
 * which exports multiple bucketed signatures (`f50`, `f100`, `f200`,
 * `f400`, `f600`, `f1000`) keyed by max frame count. Each signature
 * takes `int64 [1, 1, F]` codes and returns `float32 [1, 1, (F-1)*480]`
 * waveform at 24 kHz.
 *
 * Move-only, exception-free, no UObject. Construction owns the LiteRT
 * environment + model + compiled-model handles for the wrapper's lifetime.
 * Cleanup is automatic on destruction.
 *
 * Thread-safety: `Decode` allocates fresh tensor buffers each call and
 * does not mutate the compiled model — concurrent calls from multiple
 * threads should be safe in theory, but we don't exercise that path
 * (the runner serializes synths). Treat as single-threaded for now.
 */
class FInoNeuTTSDecoderSession
{
public:
    /** Load + compile. Returns empty TUniquePtr on failure; OutError
     *  describes what went wrong.
     *
     *  @param ModelPath  Absolute path to the NeuCodec `.tflite`.
     *  @param Backend    Hardware accelerator. Maps to the
     *                    LiteRtHwAccelerators bit passed to
     *                    LiteRtSetOptionsHardwareAccelerators. */
    static TUniquePtr<FInoNeuTTSDecoderSession> Create(
        const FString& ModelPath,
        EInoNeuTTSBackend Backend,
        FString& OutError);

    ~FInoNeuTTSDecoderSession();

    FInoNeuTTSDecoderSession(const FInoNeuTTSDecoderSession&) = delete;
    FInoNeuTTSDecoderSession& operator=(const FInoNeuTTSDecoderSession&) = delete;

    /** Decode an array of FSQ codes (each in [0, 65535]) into a 24 kHz
     *  mono float32 waveform. Output length = `(Codes.Num()-1) * 480`
     *  samples; the trailing frame is consumed by NeuCodec's iSTFT edge
     *  trim. Pads the input with zeros up to the next bucket size
     *  internally. Returns false + populates OutError if the input
     *  exceeds the max bucket (typically f1000 = 1000 frames). */
    bool Decode(TArrayView<const int32> Codes, TArray<float>& OutWaveform, FString& OutError);

    /** Run one tiny inference through the smallest bucket to pay
     *  XNNPACK delegate setup / kernel JIT once at load time. */
    bool Warmup(FString& OutError);

    int32 GetMaxBucketFrames() const { return MaxBucketFrames; }
    int32 GetSampleRate() const      { return 24000; }
    int32 GetNumChannels() const     { return 1; }

private:
    FInoNeuTTSDecoderSession() = default;
    bool Initialize(const FString& ModelPath, EInoNeuTTSBackend Backend, FString& OutError);

    LiteRtEnvironment    Environment   = nullptr;
    LiteRtModel          Model         = nullptr;
    LiteRtOptions        Options       = nullptr;
    LiteRtCompiledModel  CompiledModel = nullptr;

    /** One entry per `f<N>` signature, sorted ascending by bucket size. */
    struct FSignatureInfo
    {
        int32 SignatureIndex = 0;  // index into the LiteRtModel's signatures array
        int32 BucketFrames   = 0;  // parsed from `f<N>`
    };
    TArray<FSignatureInfo> Signatures;
    int32 MaxBucketFrames = 0;
};
