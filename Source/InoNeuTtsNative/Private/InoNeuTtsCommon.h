// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"

namespace InoNeuTtsNative
{
	/**
	 * Resolve the local on-disk path for a NeuTTS backbone (Nano or Air).
	 *
	 * Looks up the matching entry in UInoNeuTtsNativeSettings (NanoModels
	 * for Variant=Nano, AirModels for Variant=Air). If ModelName is empty
	 * the first entry in the array is used; otherwise the entry whose
	 * DisplayName matches case-insensitively.
	 *
	 * Returns:
	 *   <FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/<LocalFileName>
	 * if an entry exists; empty string otherwise.
	 *
	 * Does NOT check whether the file is on disk — callers that depend
	 * on the file (the runner) should ensure it exists or download it
	 * first.
	 */
	FString ResolveGgufPath(EInoNeuTtsVariant Variant, const FString& ModelName = FString());

	/**
	 * Resolve the local on-disk path for the NeuCodec ONNX decoder.
	 * Same shape as ResolveGgufPath — looks up DecoderModels in settings.
	 */
	FString ResolveOnnxDecoderPath(const FString& ModelName = FString());

	/** Human-readable variant name for logging. */
	FString VariantToString(EInoNeuTtsVariant Variant);
}
