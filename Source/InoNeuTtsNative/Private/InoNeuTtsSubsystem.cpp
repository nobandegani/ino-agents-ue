// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsSubsystem.h"
#include "InoNeuTtsCommon.h"
#include "InoNeuTtsDownload.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsRunner.h"
#include "InoNeuTtsSettings.h"
#include "InoNeuTtsSynthesisWorker.h"
#include "InoNeuTtsVoiceRegistry.h"

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
	bIsLoading     = false;
	bSynthInFlight = false;

	UE_LOG(LogInoNeuTts, Log, TEXT("UInoNeuTtsSubsystem deinitialized."));
	Super::Deinitialize();
}

bool UInoNeuTtsSubsystem::IsModelLoaded() const
{
	return Runner.IsValid();
}

void UInoNeuTtsSubsystem::LoadModelAsync(
	const FInoNeuTtsConfig& Config,
	const FInoNeuTtsLoadedDelegate& OnLoaded)
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

	if (Runner.IsValid() && CurrentVariant == Config.Variant)
	{
		// Same variant already loaded — fire success immediately.
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
			TEXT("LoadModelAsync: switching variant — releasing previous runner."));
		Runner.Reset();
	}

	// ---- Settings lookup ----
	const UInoNeuTtsNativeSettings* Settings =
		GetDefault<UInoNeuTtsNativeSettings>();
	if (Settings == nullptr)
	{
		FailFast(TEXT("UInoNeuTtsNativeSettings unavailable."));
		return;
	}

	const TArray<FInoNeuTtsBackboneEntry>& BackbonePool =
		(Config.Variant == EInoNeuTtsVariant::Air)
			? Settings->AirModels
			: Settings->NanoModels;

	const FInoNeuTtsBackboneEntry* BackboneEntry =
		UInoNeuTtsNativeSettings::FindBackbone(BackbonePool, Config.BackboneModelName);
	if (BackboneEntry == nullptr)
	{
		FailFast(FString::Printf(
			TEXT("No %s backbone entry%s%s configured. ")
			TEXT("Open Project Settings -> Plugins -> Ino NeuTTS Native and add at least one entry."),
			*InoNeuTtsNative::VariantToString(Config.Variant),
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

	// ---- Build the load job + kick off the async chain ----
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job = MakeShared<FLoadJob, ESPMode::ThreadSafe>();
	Job->Config       = Config;
	Job->GgufPath     = UInoNeuTtsNativeSettings::ResolveLocalPath(BackboneEntry->LocalFileName);
	Job->OnnxPath     = UInoNeuTtsNativeSettings::ResolveLocalPath(DecoderEntry->LocalFileName);
	Job->GgufUrl      = BackboneEntry->DownloadUrl;
	Job->OnnxUrl      = DecoderEntry->DownloadUrl;
	Job->GgufFileName = BackboneEntry->LocalFileName;
	Job->OnnxFileName = DecoderEntry->LocalFileName;
	Job->GgufSha      = BackboneEntry->ExpectedSha256;
	Job->OnnxSha      = DecoderEntry->ExpectedSha256;
	Job->GgufSize     = BackboneEntry->FileSizeBytes;
	Job->OnnxSize     = DecoderEntry->FileSizeBytes;
	Job->OnLoaded     = OnLoaded;

	bIsLoading = true;
	EnsureBackboneDownloaded(Job);
}

void UInoNeuTtsSubsystem::EnsureBackboneDownloaded(
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job)
{
	check(IsInGameThread());

	if (IFileManager::Get().FileExists(*Job->GgufPath))
	{
		// Already cached — proceed.
		EnsureDecoderDownloaded(Job);
		return;
	}

	if (Job->GgufUrl.IsEmpty())
	{
		FinishLoadJob(Job, /*bSuccess*/ false,
			FString::Printf(
				TEXT("Backbone file missing and no DownloadUrl configured: %s"),
				*Job->GgufPath));
		return;
	}

	UE_LOG(LogInoNeuTts, Log, TEXT("Backbone not cached, downloading: %s"), *Job->GgufUrl);

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	InoNeuTtsNative::DownloadFileAsync(
		Job->GgufUrl, Job->GgufPath, Job->GgufSha, Job->GgufSize,
		// Progress (HTTP I/O thread)
		[WeakThis, FileName = Job->GgufFileName]
		(int64 Bytes, int64 Total)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, FileName, Bytes, Total]()
			{
				if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
				{
					Self->BroadcastDownloadProgress(FileName, Bytes, Total);
				}
			});
		},
		// Completion (HTTP I/O thread)
		[WeakThis, Job](bool bOk, FString Err)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Job, bOk, Err = MoveTemp(Err)]() mutable
			{
				UInoNeuTtsSubsystem* Self = WeakThis.Get();
				if (Self == nullptr) { return; }

				if (!bOk)
				{
					Self->FinishLoadJob(Job, false, MoveTemp(Err));
					return;
				}
				Self->EnsureDecoderDownloaded(Job);
			});
		});
}

void UInoNeuTtsSubsystem::EnsureDecoderDownloaded(
	TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job)
{
	check(IsInGameThread());

	if (IFileManager::Get().FileExists(*Job->OnnxPath))
	{
		DispatchModelLoad(Job);
		return;
	}

	if (Job->OnnxUrl.IsEmpty())
	{
		FinishLoadJob(Job, /*bSuccess*/ false,
			FString::Printf(
				TEXT("Decoder file missing and no DownloadUrl configured: %s"),
				*Job->OnnxPath));
		return;
	}

	UE_LOG(LogInoNeuTts, Log, TEXT("Decoder not cached, downloading: %s"), *Job->OnnxUrl);

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	InoNeuTtsNative::DownloadFileAsync(
		Job->OnnxUrl, Job->OnnxPath, Job->OnnxSha, Job->OnnxSize,
		[WeakThis, FileName = Job->OnnxFileName]
		(int64 Bytes, int64 Total)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, FileName, Bytes, Total]()
			{
				if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
				{
					Self->BroadcastDownloadProgress(FileName, Bytes, Total);
				}
			});
		},
		[WeakThis, Job](bool bOk, FString Err)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Job, bOk, Err = MoveTemp(Err)]() mutable
			{
				UInoNeuTtsSubsystem* Self = WeakThis.Get();
				if (Self == nullptr) { return; }

				if (!bOk)
				{
					Self->FinishLoadJob(Job, false, MoveTemp(Err));
					return;
				}
				Self->DispatchModelLoad(Job);
			});
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
				Self->HandleModelLoaded(SharedRunner, Job->Config.Variant, Err, Job->OnLoaded);
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

void UInoNeuTtsSubsystem::BroadcastDownloadProgress(
	const FString& FileName, int64 BytesReceived, int64 TotalBytes)
{
	check(IsInGameThread());

	const float Frac = (TotalBytes > 0)
		? FMath::Clamp(static_cast<float>(BytesReceived) / static_cast<float>(TotalBytes), 0.0f, 1.0f)
		: 0.0f;

	OnDownloadProgress.Broadcast(FileName, BytesReceived, TotalBytes, Frac);
}

void UInoNeuTtsSubsystem::HandleModelLoaded(
	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> NewRunner,
	EInoNeuTtsVariant Variant,
	FString Error,
	FInoNeuTtsLoadedDelegate Delegate)
{
	check(IsInGameThread());

	bIsLoading = false;

	if (NewRunner.IsValid())
	{
		Runner = NewRunner;
		CurrentVariant = Variant;
		Delegate.ExecuteIfBound(true, FString());
	}
	else
	{
		Runner.Reset();
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
}

void UInoNeuTtsSubsystem::SynthesizeAsync(
	const FString& Text,
	const FInoNeuTtsVoice& Voice,
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
	if (bSynthInFlight)
	{
		FailFast(TEXT("Synth already in flight. Wait for completion or call CancelSynthesis."));
		return;
	}
	if (Text.IsEmpty())
	{
		FailFast(TEXT("Text is empty."));
		return;
	}
	if (!Voice.bIsValid || Voice.RefCodes.Num() == 0)
	{
		FailFast(TEXT("Voice is invalid (load via LoadVoiceFromFile or ListBundledVoices)."));
		return;
	}

	bSynthInFlight = true;
	CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	// Capture by value into the worker — the runner stays alive via the
	// shared_ptr copy even if the subsystem releases its own.
	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelCopy = CurrentCancelFlag;

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[Text, Voice, Options, OnComplete, RunnerCopy, CancelCopy, WeakThis]() mutable
	{
		const FInoNeuTtsResult Result = InoNeuTtsNative::RunSynthesis(
			*RunnerCopy, Text, Voice, Options, CancelCopy.Get());

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
	const FInoNeuTtsVoice& Voice,
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
	if (bSynthInFlight)
	{
		FailFast(TEXT("Synth already in flight. Wait for completion or call CancelSynthesis."));
		return;
	}
	if (Text.IsEmpty())
	{
		FailFast(TEXT("Text is empty."));
		return;
	}
	if (!Voice.bIsValid || Voice.RefCodes.Num() == 0)
	{
		FailFast(TEXT("Voice is invalid."));
		return;
	}

	bSynthInFlight    = true;
	CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelCopy = CurrentCancelFlag;

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[Text, Voice, Options, ChunkTokens,
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
				*RunnerCopy, Text, Voice, Options,
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
