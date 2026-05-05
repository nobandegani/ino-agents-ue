// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsSubsystem.h"
#include "InoNeuTtsCommon.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsRunner.h"
#include "InoNeuTtsSettings.h"
#include "InoNeuTtsSynthesisWorker.h"
#include "InoNeuTtsVoiceRegistry.h"

// Generic file downloader living in InoNodes — model files (GGUF +
// ONNX) flow through this rather than a per-plugin FHttpModule wrapper.
#include "InoDownloader.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Templates/SharedPointer.h"

void UInoNeuTtsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogInoNeuTts, Log, TEXT("UInoNeuTtsSubsystem initialized."));
}

void UInoNeuTtsSubsystem::Deinitialize()
{
	// Mid-synth shutdown: cancel via the flag, drop the runner
	// reference. The worker holds its own TSharedPtr copy, so the
	// runner lives until the worker's RunSynthesis returns; the
	// AsyncTask completion that fires OnComplete is safely no-op'd
	// by the TWeakObjectPtr-style guard in HandleSynthComplete.
	if (CurrentCancelFlag.IsValid())
	{
		CurrentCancelFlag->store(true);
	}
	Runner.Reset();
	ActiveVoice = FInoNeuTtsVoice();
	ActiveVoiceName.Reset();
	bIsLoading      = false;
	bIsPrimingVoice = false;
	bSynthInFlight  = false;

	UE_LOG(LogInoNeuTts, Log, TEXT("UInoNeuTtsSubsystem deinitialized."));
	Super::Deinitialize();
}

bool UInoNeuTtsSubsystem::IsModelLoaded() const
{
	return Runner.IsValid();
}

void UInoNeuTtsSubsystem::LoadModelAsync(
	const FInoNeuTtsConfig& Config,
	const FInoNeuTtsLoadedDelegate& OnLoaded,
	const FInoNeuTtsDownloadProgressDelegate& OnDownloadProgress)
{
	check(IsInGameThread());

	auto FailFast = [OnLoaded](const FString& Why)
	{
		FInoNeuTtsLoadedDelegate Copy = OnLoaded;
		AsyncTask(ENamedThreads::GameThread, [Copy, Why]()
		{
			Copy.ExecuteIfBound(false, Why);
		});
	};

	if (bIsLoading)
	{
		UE_LOG(LogInoNeuTts, Warning, TEXT("LoadModelAsync: already loading."));
		FailFast(TEXT("Already loading."));
		return;
	}

	// ---- Settings lookup ----
	const UInoNeuTtsNativeSettings* Settings =
		GetDefault<UInoNeuTtsNativeSettings>();
	if (Settings == nullptr)
	{
		FailFast(TEXT("UInoNeuTtsNativeSettings unavailable."));
		return;
	}

	const FInoNeuTtsBackboneEntry* BackboneEntry =
		UInoNeuTtsNativeSettings::FindBackbone(Settings->BackboneModels, Config.BackboneModelName);
	if (BackboneEntry == nullptr)
	{
		FailFast(FString::Printf(
			TEXT("No backbone entry%s%s configured. ")
			TEXT("Open Project Settings -> Plugins -> Ino NeuTTS Native and add at least one entry."),
			Config.BackboneModelName.IsEmpty() ? TEXT("") : TEXT(" named '"),
			Config.BackboneModelName.IsEmpty() ? TEXT("") : *(Config.BackboneModelName + TEXT("'"))));
		return;
	}

	const FInoNeuTtsDecoderEntry* DecoderEntry =
		UInoNeuTtsNativeSettings::FindDecoder(Settings->DecoderModels, Config.DecoderModelName);
	if (DecoderEntry == nullptr)
	{
		FailFast(TEXT("No NeuCodec decoder entry configured. ")
		         TEXT("Open Project Settings -> Plugins -> Ino NeuTTS Native and add a decoder."));
		return;
	}

	// Same backbone already loaded — fire success immediately. (The
	// decoder is identical for every backbone, so the backbone name is
	// the only thing we need to compare against.)
	if (Runner.IsValid() && CurrentBackboneName.Equals(BackboneEntry->DisplayName, ESearchCase::IgnoreCase))
	{
		FInoNeuTtsLoadedDelegate Copy = OnLoaded;
		AsyncTask(ENamedThreads::GameThread, [Copy]()
		{
			Copy.ExecuteIfBound(true, FString());
		});
		return;
	}

	if (Runner.IsValid())
	{
		UE_LOG(LogInoNeuTts, Log,
			TEXT("LoadModelAsync: switching backbone '%s' -> '%s' — releasing previous runner."),
			*CurrentBackboneName, *BackboneEntry->DisplayName);
		Runner.Reset();
		CurrentBackboneName.Reset();

		// Voice cache lived on the previous runner — drop our mirror copy
		// so HasActiveVoice / GetActiveVoiceName don't lie until the
		// caller re-primes against the newly loaded backbone.
		ActiveVoice = FInoNeuTtsVoice();
		ActiveVoiceName.Reset();
	}

	// ---- Build the load job + kick off the async chain ----
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job = MakeShared<FLoadJob, ESPMode::ThreadSafe>();
	Job->Config             = Config;
	Job->GgufPath           = UInoNeuTtsNativeSettings::ResolveLocalPath(BackboneEntry->LocalFileName);
	Job->OnnxPath           = UInoNeuTtsNativeSettings::ResolveLocalPath(DecoderEntry->LocalFileName);
	Job->BackboneName       = BackboneEntry->DisplayName;
	Job->OnLoaded           = OnLoaded;
	Job->OnDownloadProgress = OnDownloadProgress;

	bIsLoading = true;
	EnsureFilesDownloaded(Job);
}

void UInoNeuTtsSubsystem::DownloadModelAsync(
	const FInoNeuTtsConfig& Config,
	const FInoNeuTtsLoadedDelegate& OnComplete,
	const FInoNeuTtsDownloadProgressDelegate& OnDownloadProgress)
{
	check(IsInGameThread());

	auto FailFast = [OnComplete](const FString& Why)
	{
		FInoNeuTtsLoadedDelegate Copy = OnComplete;
		AsyncTask(ENamedThreads::GameThread, [Copy, Why]()
		{
			Copy.ExecuteIfBound(false, Why);
		});
	};

	if (bIsLoading)
	{
		UE_LOG(LogInoNeuTts, Warning,
			TEXT("DownloadModelAsync: a load/download is already in flight."));
		FailFast(TEXT("A load/download is already in flight."));
		return;
	}

	// Resolve Project Settings entries (same lookup as LoadModelAsync;
	// shared validation keeps error messages consistent).
	const UInoNeuTtsNativeSettings* Settings =
		GetDefault<UInoNeuTtsNativeSettings>();
	if (Settings == nullptr)
	{
		FailFast(TEXT("UInoNeuTtsNativeSettings unavailable."));
		return;
	}

	const FInoNeuTtsBackboneEntry* BackboneEntry =
		UInoNeuTtsNativeSettings::FindBackbone(
			Settings->BackboneModels, Config.BackboneModelName);
	if (BackboneEntry == nullptr)
	{
		FailFast(FString::Printf(
			TEXT("No backbone entry%s%s configured. ")
			TEXT("Open Project Settings -> Plugins -> Ino NeuTTS Native and add at least one entry."),
			Config.BackboneModelName.IsEmpty() ? TEXT("") : TEXT(" named '"),
			Config.BackboneModelName.IsEmpty() ? TEXT("") : *(Config.BackboneModelName + TEXT("'"))));
		return;
	}

	const FInoNeuTtsDecoderEntry* DecoderEntry =
		UInoNeuTtsNativeSettings::FindDecoder(
			Settings->DecoderModels, Config.DecoderModelName);
	if (DecoderEntry == nullptr)
	{
		FailFast(TEXT("No NeuCodec decoder entry configured."));
		return;
	}

	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job =
		MakeShared<FLoadJob, ESPMode::ThreadSafe>();
	Job->Config             = Config;
	Job->GgufPath           = UInoNeuTtsNativeSettings::ResolveLocalPath(BackboneEntry->LocalFileName);
	Job->OnnxPath           = UInoNeuTtsNativeSettings::ResolveLocalPath(DecoderEntry->LocalFileName);
	Job->BackboneName       = BackboneEntry->DisplayName;
	Job->OnLoaded           = OnComplete;
	Job->OnDownloadProgress = OnDownloadProgress;
	Job->bDownloadOnly      = true;

	bIsLoading = true;
	EnsureFilesDownloaded(Job);
}

bool UInoNeuTtsSubsystem::IsModelDownloaded(const FInoNeuTtsConfig& Config) const
{
	const UInoNeuTtsNativeSettings* Settings =
		GetDefault<UInoNeuTtsNativeSettings>();
	if (Settings == nullptr)
	{
		return false;
	}

	const FInoNeuTtsBackboneEntry* BackboneEntry =
		UInoNeuTtsNativeSettings::FindBackbone(
			Settings->BackboneModels, Config.BackboneModelName);
	const FInoNeuTtsDecoderEntry* DecoderEntry =
		UInoNeuTtsNativeSettings::FindDecoder(
			Settings->DecoderModels, Config.DecoderModelName);
	if (BackboneEntry == nullptr || DecoderEntry == nullptr)
	{
		return false;
	}

	const FString GgufPath =
		UInoNeuTtsNativeSettings::ResolveLocalPath(BackboneEntry->LocalFileName);
	const FString OnnxPath =
		UInoNeuTtsNativeSettings::ResolveLocalPath(DecoderEntry->LocalFileName);

	IFileManager& FM = IFileManager::Get();

	// Existence + non-zero size. SHA verification is the downloader's
	// job, not this query's — see header docstring.
	auto IsRealFile = [&FM](const FString& Path)
	{
		if (!FM.FileExists(*Path))
		{
			return false;
		}
		return FM.FileSize(*Path) > 0;
	};

	return IsRealFile(GgufPath) && IsRealFile(OnnxPath);
}

void UInoNeuTtsSubsystem::EnsureFilesDownloaded(
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job)
{
	check(IsInGameThread());

	// Re-resolve the entries here (rather than caching pointers in the
	// FLoadJob) — the settings object is a CDO whose entries can be
	// mutated by the editor in between LoadModelAsync's settings lookup
	// and this step. Worst case the entry vanished and we error fast.
	const UInoNeuTtsNativeSettings* Settings = GetDefault<UInoNeuTtsNativeSettings>();
	if (Settings == nullptr)
	{
		FinishLoadJob(Job, false, TEXT("UInoNeuTtsNativeSettings unavailable."));
		return;
	}

	const FInoNeuTtsBackboneEntry* BackboneEntry =
		UInoNeuTtsNativeSettings::FindBackbone(Settings->BackboneModels, Job->Config.BackboneModelName);
	const FInoNeuTtsDecoderEntry* DecoderEntry =
		UInoNeuTtsNativeSettings::FindDecoder(Settings->DecoderModels, Job->Config.DecoderModelName);

	if (BackboneEntry == nullptr || DecoderEntry == nullptr)
	{
		FinishLoadJob(Job, false,
			TEXT("Backbone or decoder entry vanished from Project Settings during load."));
		return;
	}

	// Build the multi-file request batch. The InoNodes downloader handles
	// HEAD probe → GET → .partial staging → atomic rename → optional
	// streaming SHA-256 internally; aggregate progress (per-file +
	// overall %) is delivered through a single FInoDownloadProgress
	// struct so callers don't have to combine two separate progress
	// streams themselves.
	const FString TargetDir = UInoNeuTtsNativeSettings::GetModelsDir();

	TArray<FInoDownloadRequest> Requests;
	Requests.Reserve(2);

	// 1. GGUF backbone
	{
		FInoDownloadRequest& R = Requests.AddDefaulted_GetRef();
		R.Url                 = BackboneEntry->DownloadUrl;
		R.SaveDirectory       = TargetDir;
		R.FileName            = BackboneEntry->LocalFileName;
		R.ExpectedSha256      = BackboneEntry->ExpectedSha256;
		R.ExpectedTotalBytes  = BackboneEntry->FileSizeBytes;
		R.bSkipIfCached       = true;
	}

	// 2. NeuCodec ONNX decoder
	{
		FInoDownloadRequest& R = Requests.AddDefaulted_GetRef();
		R.Url                 = DecoderEntry->DownloadUrl;
		R.SaveDirectory       = TargetDir;
		R.FileName            = DecoderEntry->LocalFileName;
		R.ExpectedSha256      = DecoderEntry->ExpectedSha256;
		R.ExpectedTotalBytes  = DecoderEntry->FileSizeBytes;
		R.bSkipIfCached       = true;
	}

	// Validate URLs are present *unless* both files are already cached
	// (which is the typical post-first-run path). Empty URL on a missing
	// file is a clear configuration error and we fail fast there.
	for (int32 i = 0; i < Requests.Num(); ++i)
	{
		const FString LocalPath = (i == 0) ? Job->GgufPath : Job->OnnxPath;
		if (Requests[i].Url.IsEmpty() && !IFileManager::Get().FileExists(*LocalPath))
		{
			FinishLoadJob(Job, false,
				FString::Printf(
					TEXT("Model file missing and no DownloadUrl configured: %s"),
					*LocalPath));
			return;
		}
	}

	UE_LOG(LogInoNeuTts, Log,
		TEXT("LoadModelAsync: downloading/verifying %d file(s) into %s"),
		Requests.Num(), *TargetDir);

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	InoNodes::Download::DownloadFilesAsync(
		Requests,
		// Progress — already marshalled to the game thread by InoNodes.
		[WeakThis, Job](const FInoDownloadProgress& P)
		{
			if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
			{
				FInoNeuTtsDownloadProgressDelegate Copy = Job->OnDownloadProgress;
				Copy.ExecuteIfBound(P);
			}
		},
		// Batch completion — game thread.
		[WeakThis, Job](const TArray<FInoDownloadResult>& Results)
		{
			UInoNeuTtsSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				// Subsystem gone — silently drop. The downloader has
				// already cleaned up .partial files and released its
				// own state.
				return;
			}

			// Pick out the first failure (if any). DownloadFilesAsync
			// stops on first failure and reports remaining entries with
			// bSuccess=false / "Skipped".
			for (const FInoDownloadResult& R : Results)
			{
				if (!R.bSuccess)
				{
					Self->FinishLoadJob(Job, false,
						FString::Printf(TEXT("Download failed for %s: %s"),
							*R.FileName, *R.ErrorMessage));
					return;
				}
			}

			// DownloadModelAsync path: stop here, fire OnComplete success.
			// LoadModelAsync path: proceed to off-thread model load.
			if (Job->bDownloadOnly)
			{
				Self->FinishLoadJob(Job, true, FString());
			}
			else
			{
				Self->DispatchModelLoad(Job);
			}
		});
}

void UInoNeuTtsSubsystem::DispatchModelLoad(
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job)
{
	check(IsInGameThread());

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[Job, WeakThis]()
	{
		FString Error;

		using FRunner = InoNeuTtsNative::FInoNeuTtsRunner;
		TUniquePtr<FRunner> NewRunner = FRunner::Create(
			Job->Config, Job->GgufPath, Job->OnnxPath, Error);

		TSharedPtr<FRunner, ESPMode::ThreadSafe> SharedRunner;
		if (NewRunner.IsValid())
		{
			SharedRunner = MakeShareable(NewRunner.Release());
		}

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, SharedRunner, Job, Err = MoveTemp(Error)]() mutable
		{
			if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
			{
				if (SharedRunner.IsValid())
				{
					Self->CurrentBackboneName = Job->BackboneName;
				}
				Self->HandleModelLoaded(SharedRunner, Err, Job->OnLoaded);
				// HandleModelLoaded clears bIsLoading + fires OnLoaded.
			}
		});
	});
}

void UInoNeuTtsSubsystem::FinishLoadJob(
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job,
	bool bSuccess,
	FString Error)
{
	check(IsInGameThread());

	bIsLoading = false;

	FInoNeuTtsLoadedDelegate Copy = Job->OnLoaded;
	Copy.ExecuteIfBound(bSuccess, Error);
}

void UInoNeuTtsSubsystem::HandleModelLoaded(
	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> NewRunner,
	FString Error,
	FInoNeuTtsLoadedDelegate Delegate)
{
	check(IsInGameThread());

	bIsLoading = false;

	if (NewRunner.IsValid())
	{
		Runner = NewRunner;
		Delegate.ExecuteIfBound(true, FString());
	}
	else
	{
		Runner.Reset();
		CurrentBackboneName.Reset();
		Delegate.ExecuteIfBound(false, Error);
	}
}

void UInoNeuTtsSubsystem::UnloadModel()
{
	check(IsInGameThread());

	if (CurrentCancelFlag.IsValid())
	{
		CurrentCancelFlag->store(true);
	}
	// Drop our reference. Any worker holding a TSharedPtr copy keeps
	// the runner alive until it finishes; the runner destructor then
	// runs on whichever thread released the last shared ref (typically
	// the game thread, via the AsyncTask completion).
	Runner.Reset();
	CurrentBackboneName.Reset();

	// Active voice cache lived on the runner — drop our mirror copy
	// alongside it so HasActiveVoice / GetActiveVoiceName stay accurate.
	ActiveVoice = FInoNeuTtsVoice();
	ActiveVoiceName.Reset();
}

// ============================================================================
//  Active voice priming (KV-prefix snapshot)
// ============================================================================

bool UInoNeuTtsSubsystem::HasActiveVoice() const
{
	return !ActiveVoiceName.IsEmpty()
		&& Runner.IsValid()
		&& Runner->HasCachedVoice(ActiveVoiceName);
}

void UInoNeuTtsSubsystem::ClearActiveVoice()
{
	check(IsInGameThread());

	if (Runner.IsValid())
	{
		Runner->ClearVoiceCache();
	}
	ActiveVoice = FInoNeuTtsVoice();
	ActiveVoiceName.Reset();
}

void UInoNeuTtsSubsystem::SetActiveVoiceAsync(
	const FInoNeuTtsVoice& Voice,
	const FInoNeuTtsVoiceReadyDelegate& OnReady)
{
	check(IsInGameThread());

	auto FailFast = [&OnReady](const FString& Why)
	{
		FInoNeuTtsVoiceReadyDelegate Copy = OnReady;
		AsyncTask(ENamedThreads::GameThread, [Copy, Why]()
		{
			Copy.ExecuteIfBound(false, Why);
		});
	};

	if (!Runner.IsValid())
	{
		FailFast(TEXT("Model not loaded. Call LoadModelAsync first."));
		return;
	}
	if (bIsPrimingVoice)
	{
		FailFast(TEXT("A SetActiveVoiceAsync call is already in flight."));
		return;
	}
	if (bSynthInFlight)
	{
		FailFast(TEXT("Cannot prime voice while a synth is in flight. ")
		         TEXT("Wait for synth completion or call CancelSynthesis."));
		return;
	}
	if (!Voice.bIsValid || Voice.RefCodes.Num() == 0)
	{
		FailFast(TEXT("Voice is invalid (load via LoadVoiceFromFile or ListBundledVoices)."));
		return;
	}
	if (Voice.Name.IsEmpty())
	{
		FailFast(TEXT("Voice has no Name — required for cache identity."));
		return;
	}

	// Same voice already primed — fire success immediately.
	if (Runner->HasCachedVoice(Voice.Name))
	{
		ActiveVoice     = Voice;
		ActiveVoiceName = Voice.Name;
		FInoNeuTtsVoiceReadyDelegate Copy = OnReady;
		AsyncTask(ENamedThreads::GameThread, [Copy]()
		{
			Copy.ExecuteIfBound(true, FString());
		});
		return;
	}

	bIsPrimingVoice = true;

	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[RunnerCopy, Voice, OnReady, WeakThis]()
	{
		FString Error;
		const bool bOk = RunnerCopy->PrimeVoice(Voice, Error);

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Voice, bOk, Error = MoveTemp(Error), OnReady]() mutable
		{
			UInoNeuTtsSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}

			Self->bIsPrimingVoice = false;

			if (bOk)
			{
				Self->ActiveVoice     = Voice;
				Self->ActiveVoiceName = Voice.Name;
			}

			FInoNeuTtsVoiceReadyDelegate Copy = OnReady;
			Copy.ExecuteIfBound(bOk, Error);
		});
	});
}

void UInoNeuTtsSubsystem::SynthesizeAsync(
	const FString& Text,
	const FInoNeuTtsOptions& Options,
	const FInoNeuTtsSynthesisCompleteDelegate& OnComplete)
{
	check(IsInGameThread());

	auto FailFast = [&OnComplete](const FString& Why)
	{
		FInoNeuTtsResult Bad;
		Bad.bSuccess = false;
		Bad.ErrorMessage = Why;
		FInoNeuTtsSynthesisCompleteDelegate Copy = OnComplete;
		AsyncTask(ENamedThreads::GameThread, [Copy, Bad]()
		{
			Copy.ExecuteIfBound(Bad);
		});
	};

	if (!Runner.IsValid())
	{
		FailFast(TEXT("Model not loaded. Call LoadModelAsync first."));
		return;
	}
	if (ActiveVoiceName.IsEmpty() || !ActiveVoice.bIsValid)
	{
		FailFast(TEXT("No active voice. Call SetActiveVoiceAsync first."));
		return;
	}
	if (bSynthInFlight)
	{
		FailFast(TEXT("Synth already in flight. Wait for completion or call CancelSynthesis."));
		return;
	}
	if (bIsPrimingVoice)
	{
		FailFast(TEXT("Cannot synthesize while a SetActiveVoiceAsync prime is in flight."));
		return;
	}
	if (Text.IsEmpty())
	{
		FailFast(TEXT("Text is empty."));
		return;
	}

	bSynthInFlight = true;
	CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	// Capture by value into the worker — the runner stays alive via the
	// shared_ptr copy even if the subsystem releases its own.
	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelCopy = CurrentCancelFlag;
	const FInoNeuTtsVoice VoiceCopy = ActiveVoice;

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[Text, VoiceCopy, Options, OnComplete, RunnerCopy, CancelCopy, WeakThis]() mutable
	{
		const FInoNeuTtsResult Result = InoNeuTtsNative::RunSynthesis(
			*RunnerCopy, Text, VoiceCopy, Options, CancelCopy.Get());

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Result, OnComplete]() mutable
		{
			if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
			{
				Self->HandleSynthComplete(Result, OnComplete);
			}
			else
			{
				// Subsystem gone (PIE end / game shutdown) — silently drop
				// the result rather than firing into a stale UObject.
			}
		});
	});
}

void UInoNeuTtsSubsystem::HandleSynthComplete(
	FInoNeuTtsResult Result,
	FInoNeuTtsSynthesisCompleteDelegate Delegate)
{
	check(IsInGameThread());

	bSynthInFlight = false;
	CurrentCancelFlag.Reset();

	Delegate.ExecuteIfBound(Result);
}

void UInoNeuTtsSubsystem::SynthesizeStreamAsync(
	const FString& Text,
	const FInoNeuTtsOptions& Options,
	int32 ChunkTokens,
	const FInoNeuTtsAudioChunkDelegate& OnAudioChunk,
	const FInoNeuTtsSynthesisCompleteDelegate& OnComplete)
{
	check(IsInGameThread());

	auto FailFast = [&OnComplete](const FString& Why)
	{
		FInoNeuTtsResult Bad;
		Bad.bSuccess     = false;
		Bad.ErrorMessage = Why;
		FInoNeuTtsSynthesisCompleteDelegate Copy = OnComplete;
		AsyncTask(ENamedThreads::GameThread, [Copy, Bad]()
		{
			Copy.ExecuteIfBound(Bad);
		});
	};

	if (!Runner.IsValid())
	{
		FailFast(TEXT("Model not loaded. Call LoadModelAsync first."));
		return;
	}
	if (ActiveVoiceName.IsEmpty() || !ActiveVoice.bIsValid)
	{
		FailFast(TEXT("No active voice. Call SetActiveVoiceAsync first."));
		return;
	}
	if (bSynthInFlight)
	{
		FailFast(TEXT("Synth already in flight. Wait for completion or call CancelSynthesis."));
		return;
	}
	if (bIsPrimingVoice)
	{
		FailFast(TEXT("Cannot synthesize while a SetActiveVoiceAsync prime is in flight."));
		return;
	}
	if (Text.IsEmpty())
	{
		FailFast(TEXT("Text is empty."));
		return;
	}

	bSynthInFlight    = true;
	CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelCopy = CurrentCancelFlag;
	const FInoNeuTtsVoice VoiceCopy = ActiveVoice;

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[Text, VoiceCopy, Options, ChunkTokens,
		 OnAudioChunk, OnComplete,
		 RunnerCopy, CancelCopy, WeakThis]() mutable
	{
		// Bridge: chunk callback fires from this worker thread; marshal
		// to game thread before invoking the Blueprint delegate.
		InoNeuTtsNative::FInoNeuTtsStreamCallbacks Callbacks;
		Callbacks.OnChunk =
			[WeakThis, OnAudioChunk](TArray<uint8> ChunkBytes, bool bIsFinal)
		{
			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, OnAudioChunk,
				 ChunkBytes = MoveTemp(ChunkBytes), bIsFinal]() mutable
			{
				if (WeakThis.IsValid())
				{
					OnAudioChunk.ExecuteIfBound(ChunkBytes, bIsFinal);
				}
			});
		};

		const FInoNeuTtsResult Result =
			InoNeuTtsNative::RunStreamingSynthesis(
				*RunnerCopy, Text, VoiceCopy, Options,
				ChunkTokens, Callbacks, CancelCopy.Get());

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Result, OnComplete]() mutable
		{
			if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
			{
				Self->HandleSynthComplete(Result, OnComplete);
			}
		});
	});
}

void UInoNeuTtsSubsystem::CancelSynthesis()
{
	check(IsInGameThread());

	if (CurrentCancelFlag.IsValid())
	{
		CurrentCancelFlag->store(true);
	}
}

bool UInoNeuTtsSubsystem::LoadVoiceFromFile(const FString& FilePath, FInoNeuTtsVoice& OutVoice)
{
	return InoNeuTtsNative::FInoNeuTtsVoiceRegistry::LoadFromFile(FilePath, OutVoice);
}

TArray<FInoNeuTtsVoice> UInoNeuTtsSubsystem::ListBundledVoices()
{
	return InoNeuTtsNative::FInoNeuTtsVoiceRegistry::LoadBundledVoices();
}
