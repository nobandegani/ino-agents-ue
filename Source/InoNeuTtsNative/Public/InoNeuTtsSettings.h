// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "InoNeuTtsSettings.generated.h"

// ============================================================================
//  Per-entry structs
// ============================================================================

/**
 * One downloadable NeuTTS backbone (GGUF) variant entry. Used for both
 * Nano and Air arrays — the structure is identical; the array's array
 * (NanoModels vs AirModels) determines the variant.
 *
 * The file lands at:
 *   <FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/<LocalFileName>
 *
 * If the file is already present at that path, no download happens and
 * DownloadUrl is unused. ExpectedSha256 is checked after download (and
 * also against an existing file when present, so a corrupt cached file
 * is re-downloaded automatically).
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsBackboneEntry
{
	GENERATED_BODY()

	/**
	 * Identifier used at runtime to pick this entry from the array.
	 * `FInoNeuTtsConfig::BackboneModelName` matches against this. If
	 * the config name is empty, the first entry in the array is used.
	 *
	 * Convention: a short kebab-case name like "neutts-nano-Q4_0-en"
	 * or "neutts-air-Q8_0".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString DisplayName;

	/**
	 * HTTPS URL to fetch the GGUF file from. Anything FHttpModule can
	 * GET works (HuggingFace, S3, your CDN). Public URLs only — no
	 * auth handling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString DownloadUrl;

	/**
	 * Filename to save as locally. The runtime concatenates this with
	 * <persistent>/InoAgents/NeuTTS/ to get the full path. Must end
	 * in .gguf for llama.cpp to recognize it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString LocalFileName;

	/**
	 * Hex-encoded SHA-256 hash for integrity verification. Optional but
	 * strongly recommended. Empty = skip verification (download is
	 * trusted by URL only).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString ExpectedSha256;

	/**
	 * Total file size in bytes. Used for the OnDownloadProgress
	 * percentage when the server doesn't return Content-Length on
	 * GET. 0 = probe via a HEAD request before downloading.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	int64 FileSizeBytes = 0;

	/**
	 * eSpeak language code this backbone was trained on (en-us, de,
	 * fr-fr, es). Drives input phonemization and voice/language
	 * pairing. NeuTTS Air is English-only; Nano has multilingual
	 * variants.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString Language = TEXT("en-us");

	/**
	 * Quantization label. Informational only — doesn't change runtime
	 * behavior (the dtype is intrinsic to the GGUF file). Common values:
	 * "Q4_0", "Q8_0", "FP16".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString Quantization = TEXT("Q4_0");
};

/**
 * One downloadable NeuCodec ONNX decoder entry. Same path resolution as
 * the backbone entries (under <persistent>/InoAgents/NeuTTS/). The same
 * decoder is used for both Nano and Air — only the LM backbone differs.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsDecoderEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString DownloadUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString LocalFileName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	FString ExpectedSha256;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
	int64 FileSizeBytes = 0;
};

// ============================================================================
//  Settings class — Project Settings → Plugins → Ino NeuTTS Native
// ============================================================================

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "InoNeuTtsNative"))
class INONEUTTSNATIVE_API UInoNeuTtsNativeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UInoNeuTtsNativeSettings();

	//~ UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	//~ End UDeveloperSettings interface

	/** Available NeuTTS Nano backbones. Empty by default — populate with download URLs. */
	UPROPERTY(EditAnywhere, Config, Category = "NeuTTS Nano")
	TArray<FInoNeuTtsBackboneEntry> NanoModels;

	/** Available NeuTTS Air backbones. */
	UPROPERTY(EditAnywhere, Config, Category = "NeuTTS Air")
	TArray<FInoNeuTtsBackboneEntry> AirModels;

	/** Available NeuCodec ONNX decoders. Same decoder works for both Nano + Air. */
	UPROPERTY(EditAnywhere, Config, Category = "NeuCodec Decoder")
	TArray<FInoNeuTtsDecoderEntry> DecoderModels;

	// ---- Static helpers ----

	/**
	 * Directory where downloaded model files are stored:
	 *   <FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/
	 *
	 * Per-user, sandboxed on mobile, persists across project reinstalls.
	 */
	static FString GetModelsDir();

	/**
	 * Resolve a downloaded model's full local path:
	 *   <GetModelsDir()> / <LocalFileName>
	 */
	static FString ResolveLocalPath(const FString& LocalFileName);

	/**
	 * Pick a backbone entry matching DesiredName. If DesiredName is empty,
	 * returns the first entry. Returns nullptr if the array is empty or
	 * no entry matches the given name.
	 */
	static const FInoNeuTtsBackboneEntry* FindBackbone(
		const TArray<FInoNeuTtsBackboneEntry>& Pool,
		const FString& DesiredName);

	/** Same shape, for decoder entries. */
	static const FInoNeuTtsDecoderEntry* FindDecoder(
		const TArray<FInoNeuTtsDecoderEntry>& Pool,
		const FString& DesiredName);
};
