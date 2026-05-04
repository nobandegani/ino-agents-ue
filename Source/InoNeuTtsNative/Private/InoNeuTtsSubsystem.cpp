// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsSubsystem.h"
#include "InoNeuTtsCommon.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsRunner.h"
#include "InoNeuTtsSynthesisWorker.h"
#include "InoNeuTtsVoiceRegistry.h"

#include "Async/Async.h"
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

	if (bIsLoading)
	{
		UE_LOG(LogInoNeuTts, Warning, TEXT("LoadModelAsync: already loading."));
		FInoNeuTtsLoadedDelegate Copy = OnLoaded;
		AsyncTask(ENamedThreads::GameThread, [Copy]()
		{
			Copy.ExecuteIfBound(false, TEXT("Already loading."));
		});
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

	const FString GgufPath = InoNeuTtsNative::ResolveGgufPath(Config.Variant);
	const FString OnnxPath = InoNeuTtsNative::ResolveOnnxDecoderPath();

	bIsLoading = true;

	TWeakObjectPtr<UInoNeuTtsSubsystem> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[Config, GgufPath, OnnxPath, OnLoaded, WeakThis]()
	{
		FString Error;

		using FRunner = InoNeuTtsNative::FInoNeuTtsRunner;
		TUniquePtr<FRunner> NewRunner = FRunner::Create(
			Config, GgufPath, OnnxPath, Error);

		// Wrap into a thread-safe shared ptr so an in-flight synth
		// can outlive the subsystem's own reference.
		TSharedPtr<FRunner, ESPMode::ThreadSafe> SharedRunner;
		if (NewRunner.IsValid())
		{
			SharedRunner = MakeShareable(NewRunner.Release());
		}

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, SharedRunner, Variant = Config.Variant,
			 Err = MoveTemp(Error), OnLoaded]() mutable
		{
			if (UInoNeuTtsSubsystem* Self = WeakThis.Get())
			{
				Self->HandleModelLoaded(SharedRunner, Variant, Err, OnLoaded);
			}
			// If the subsystem is gone, the SharedRunner falls out of
			// scope here on the game thread and frees cleanly.
		});
	});
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
