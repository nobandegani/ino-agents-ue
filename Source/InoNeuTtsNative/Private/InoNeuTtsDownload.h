// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

namespace InoNeuTtsNative
{
	/**
	 * Per-file download progress. Fires from the HTTP request's I/O
	 * thread. Caller should marshal to game thread before calling
	 * Blueprint delegates.
	 *   BytesReceived: 0..TotalBytes (or 0 if Total unknown)
	 *   TotalBytes: server-reported size, or the entry's FileSizeBytes
	 *               override when set, or 0 if neither is known
	 */
	using FOnDownloadProgress =
		TFunction<void(int64 BytesReceived, int64 TotalBytes)>;

	/**
	 * Final completion. Fires once per call from the HTTP request's
	 * thread. bSuccess=true means the file is on disk at LocalPath
	 * with a verified SHA-256 (if expected provided) and an atomic
	 * rename has happened. bSuccess=false means the call did not
	 * touch LocalPath (the .partial staging file may exist; cleaned
	 * up internally before the callback).
	 */
	using FOnDownloadComplete =
		TFunction<void(bool bSuccess, FString Error)>;

	/**
	 * Download a file from URL to LocalPath. Handles:
	 *   - HEAD probe to discover Content-Length (used as Total when
	 *     ExpectedSizeOverride is 0)
	 *   - GET to a sibling .partial file
	 *   - Optional SHA-256 verification (Empty = skip)
	 *   - Atomic rename .partial -> LocalPath on success
	 *   - Cleanup of .partial on failure
	 *
	 * Thread-safe: the HTTP module is callable from any thread.
	 * Callbacks fire from the HTTP I/O thread; marshal to game thread
	 * if you need UObject access.
	 */
	void DownloadFileAsync(
		const FString& Url,
		const FString& LocalPath,
		const FString& ExpectedSha256Hex,
		int64 ExpectedSizeOverride,
		FOnDownloadProgress OnProgress,
		FOnDownloadComplete OnComplete);

	/**
	 * Compute the SHA-256 hex digest of the file at the given path.
	 * Returns empty string on read failure or if the file doesn't
	 * exist. Lowercase hex, no separators, 64 chars.
	 */
	FString ComputeFileSha256Hex(const FString& FilePath);

	/**
	 * Verify a file's SHA-256 matches the expected hex. Empty
	 * ExpectedHex always returns true (skip). Case-insensitive
	 * comparison.
	 */
	bool VerifyFileSha256(const FString& FilePath, const FString& ExpectedHex);
}
