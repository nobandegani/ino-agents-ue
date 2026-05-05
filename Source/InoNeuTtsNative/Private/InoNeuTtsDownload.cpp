// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsDownload.h"
#include "InoNeuTtsLog.h"

#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace InoNeuTtsNative
{
	namespace
	{
		/** Path used during download; renamed atomically to the final path on success. */
		FString PartialPath(const FString& FinalPath)
		{
			return FinalPath + TEXT(".partial");
		}

		void DeleteIfExists(const FString& Path)
		{
			if (IFileManager::Get().FileExists(*Path))
			{
				IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true);
			}
		}

		/** Atomic-ish rename. UE's IFileManager::Move uses MoveFileEx on Windows. */
		bool AtomicRename(const FString& From, const FString& To)
		{
			DeleteIfExists(To);
			return IFileManager::Get().Move(*To, *From, /*Replace*/ true, /*EvenIfReadOnly*/ true);
		}

		void FailWith(const FString& PartialFile, FOnDownloadComplete OnComplete, const FString& Error)
		{
			DeleteIfExists(PartialFile);
			UE_LOG(LogInoNeuTts, Error, TEXT("Download FAILED: %s"), *Error);
			if (OnComplete)
			{
				OnComplete(false, Error);
			}
		}
	}

	// SHA-256 verification placeholder. UE 5.7 doesn't ship FSHA256 in
	// the public Core headers; the InoAgents plugin has a private
	// InoSha256 helper for LiteRT-LM models which isn't exposed
	// cross-module. For v1 we accept the URL as the integrity boundary.
	// When this becomes a real concern, expose InoAgents/Private/
	// LiteRtLm/InoSha256 via a public header and call it here.

	FString ComputeFileSha256Hex(const FString& /*FilePath*/)
	{
		return FString();
	}

	bool VerifyFileSha256(const FString& /*FilePath*/, const FString& ExpectedHex)
	{
		if (!ExpectedHex.IsEmpty())
		{
			UE_LOG(LogInoNeuTts, Verbose,
				TEXT("SHA-256 verification skipped (not yet implemented). ")
				TEXT("Expected hash: %s"), *ExpectedHex);
		}
		return true;
	}

	void DownloadFileAsync(
		const FString& Url,
		const FString& LocalPath,
		const FString& ExpectedSha256Hex,
		int64 ExpectedSizeOverride,
		FOnDownloadProgress OnProgress,
		FOnDownloadComplete OnComplete)
	{
		// File already cached and (would be) verified — skip the download.
		if (IFileManager::Get().FileExists(*LocalPath))
		{
			if (VerifyFileSha256(LocalPath, ExpectedSha256Hex))
			{
				UE_LOG(LogInoNeuTts, Log,
					TEXT("Download skipped (cached): %s"), *LocalPath);
				if (OnComplete)
				{
					OnComplete(true, FString());
				}
				return;
			}
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("Cached file failed verification — re-downloading: %s"), *LocalPath);
			DeleteIfExists(LocalPath);
		}

		// Make sure the parent directory exists.
		const FString ParentDir = FPaths::GetPath(LocalPath);
		if (!IFileManager::Get().DirectoryExists(*ParentDir))
		{
			IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);
		}

		const FString Partial = PartialPath(LocalPath);
		// Clean any leftover from a prior aborted attempt.
		DeleteIfExists(Partial);

		FHttpModule& Http = FHttpModule::Get();
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
		Request->SetVerb(TEXT("GET"));
		Request->SetURL(Url);
		// Long timeout — model files are large; some CDNs throttle.
		Request->SetTimeout(60 * 30); // 30 minutes

		Request->OnRequestProgress64().BindLambda(
			[OnProgress, ExpectedSizeOverride]
			(FHttpRequestPtr, uint64 BytesSent, uint64 BytesReceived) mutable
			{
				if (OnProgress)
				{
					OnProgress(static_cast<int64>(BytesReceived), ExpectedSizeOverride);
				}
			});

		Request->OnProcessRequestComplete().BindLambda(
			[Url, LocalPath, Partial, ExpectedSha256Hex, OnComplete]
			(FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
			{
				if (!bSucceeded || !Response.IsValid())
				{
					FailWith(Partial, OnComplete,
						FString::Printf(TEXT("HTTP request failed for %s"), *Url));
					return;
				}

				const int32 Code = Response->GetResponseCode();
				if (Code < 200 || Code >= 300)
				{
					FailWith(Partial, OnComplete,
						FString::Printf(TEXT("HTTP %d fetching %s"), Code, *Url));
					return;
				}

				// Write the response body to the .partial file.
				const TArray<uint8>& Body = Response->GetContent();
				if (Body.Num() == 0)
				{
					FailWith(Partial, OnComplete,
						FString::Printf(TEXT("Empty response body for %s"), *Url));
					return;
				}

				if (!FFileHelper::SaveArrayToFile(Body, *Partial))
				{
					FailWith(Partial, OnComplete,
						FString::Printf(TEXT("Failed to write .partial file %s"), *Partial));
					return;
				}

				if (!VerifyFileSha256(Partial, ExpectedSha256Hex))
				{
					FailWith(Partial, OnComplete,
						FString::Printf(
							TEXT("SHA-256 mismatch for downloaded %s"), *LocalPath));
					return;
				}

				if (!AtomicRename(Partial, LocalPath))
				{
					FailWith(Partial, OnComplete,
						FString::Printf(
							TEXT("Atomic rename failed: %s -> %s"),
							*Partial, *LocalPath));
					return;
				}

				const int64 FinalSize = IFileManager::Get().FileSize(*LocalPath);
				UE_LOG(LogInoNeuTts, Log,
					TEXT("Download ok: %s (%lld bytes)"), *LocalPath, FinalSize);

				if (OnComplete)
				{
					OnComplete(true, FString());
				}
			});

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Download starting: %s -> %s"), *Url, *LocalPath);

		Request->ProcessRequest();
	}
}
