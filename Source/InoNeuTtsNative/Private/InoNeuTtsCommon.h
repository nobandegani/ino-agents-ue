// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"

namespace InoNeuTtsNative
{
	/**
	 * Resolve the path to the GGUF backbone for the given variant.
	 * Returns an empty string if the InoAgents plugin can't be located.
	 *
	 * Layout (matches Plugins/InoAgents/NeuTTS/models/ as downloaded):
	 *   NeuTTS/models/nano-q4-gguf/neutts-nano-Q4_0.gguf
	 *   NeuTTS/models/air-q4-gguf/neutts-air-Q4_0.gguf
	 */
	FString ResolveGgufPath(EInoNeuTtsVariant Variant);

	/**
	 * Resolve the path to the NeuCodec ONNX decoder. The same decoder is
	 * used for both Nano and Air — only the LLM backbone differs.
	 *
	 * Layout: NeuTTS/models/onnx-decoder-int8/model.onnx
	 */
	FString ResolveOnnxDecoderPath();

	/** Convenience: human-readable variant name for logging. */
	FString VariantToString(EInoNeuTtsVariant Variant);
}
