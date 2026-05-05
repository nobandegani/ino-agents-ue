// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsCommon.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsRunner.h"
#include "InoNeuTtsSynthesisWorker.h"
#include "InoNeuTtsTypes.h"
#include "InoNeuTtsVoiceRegistry.h"

#include "Audio/InoAudioFunctionLibrary.h"

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * Streaming synth test:
	 *   - Loads model + voice (default jo + first backbone entry)
	 *   - Runs streaming synth (calls RunStreamingSynthesis directly,
	 *     bypassing the subsystem so we can run without PIE)
	 *   - Logs each chunk's wall-clock arrival, byte size, audio ms
	 *   - Saves concatenated audio as <Project>/Saved/InoNeuTtsStreamTest.wav
	 *   - Reports TTFA + total wall time + RTF
	 *
	 * Args: [voice=jo] [backbone DisplayName] [chunk_tokens=25] [text...]
	 */
	void RunStreamSynthTest(const TArray<FString>& Args)
	{
		using namespace InoNeuTtsNative;

		// ---- arg parsing ----
		FString VoiceName    = TEXT("jo");
		FString BackboneName;
		int32   ChunkTokens  = 25;
		FString Text;

		int32 NextArg = 0;
		if (Args.Num() > NextArg) { VoiceName = Args[NextArg++]; }

		// Optional backbone DisplayName: a single non-numeric token with
		// no spaces and not ending in punctuation (kebab-case is the
		// convention). If we can't tell, fall through to the next arm.
		if (Args.Num() > NextArg
			&& !Args[NextArg].IsNumeric()
			&& !Args[NextArg].Contains(TEXT(" "))
			&& !Args[NextArg].EndsWith(TEXT(".")))
		{
			BackboneName = Args[NextArg++];
		}

		if (Args.Num() > NextArg && Args[NextArg].IsNumeric())
		{
			ChunkTokens = FCString::Atoi(*Args[NextArg]);
			NextArg++;
		}

		if (Args.Num() > NextArg)
		{
			for (int32 i = NextArg; i < Args.Num(); ++i)
			{
				if (!Text.IsEmpty()) { Text += TEXT(" "); }
				Text += Args[i];
			}
		}
		else
		{
			Text = TEXT("Hello there. My name is Andy and I just moved to London.");
		}

		// ---- voice + paths ----
		const FString VoicesDir = FInoNeuTtsVoiceRegistry::GetBundledVoicesDir();
		const FString VoicePath = FPaths::Combine(VoicesDir,
			FString::Printf(TEXT("%s.nvoice.json"), *VoiceName));

		FInoNeuTtsVoice Voice;
		if (!FInoNeuTtsVoiceRegistry::LoadFromFile(VoicePath, Voice))
		{
			UE_LOG(LogInoNeuTts, Error,
				TEXT("FAIL: voice '%s' not found at %s"), *VoiceName, *VoicePath);
			return;
		}

		const FString GgufPath = ResolveGgufPath(BackboneName);
		const FString OnnxPath = ResolveOnnxDecoderPath();

		UE_LOG(LogInoNeuTts, Display, TEXT("=== Ino.NeuTts.StreamSynthTest ==="));
		UE_LOG(LogInoNeuTts, Display, TEXT("Voice:        %s (lang=%s, codes=%d)"),
			*Voice.Name, *Voice.Language, Voice.RefCodes.Num());
		UE_LOG(LogInoNeuTts, Display, TEXT("Backbone:     %s"),
			BackboneName.IsEmpty() ? TEXT("<first entry>") : *BackboneName);
		UE_LOG(LogInoNeuTts, Display, TEXT("Chunk tokens: %d (~%.1f ms)"),
			ChunkTokens, (ChunkTokens * 480.0f) / 24.0f);
		UE_LOG(LogInoNeuTts, Display, TEXT("Text:         %s"), *Text);

		FInoNeuTtsConfig Config;
		Config.BackboneModelName = BackboneName;
		FInoNeuTtsOptions Options;

		const FString OutPath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("InoNeuTtsStreamTest.wav"));

		Async(EAsyncExecution::ThreadPool,
			[Config, GgufPath, OnnxPath, Voice, Text, Options,
			 ChunkTokens, OutPath]() mutable
		{
			const double LoadT0 = FPlatformTime::Seconds();

			FString LoadError;
			TUniquePtr<FInoNeuTtsRunner> Runner = FInoNeuTtsRunner::Create(
				Config, GgufPath, OnnxPath, LoadError);
			const double LoadElapsed = FPlatformTime::Seconds() - LoadT0;

			if (!Runner.IsValid())
			{
				AsyncTask(ENamedThreads::GameThread,
					[Err = MoveTemp(LoadError)]()
				{
					UE_LOG(LogInoNeuTts, Error, TEXT("FAIL (load): %s"), *Err);
				});
				return;
			}

			// Streaming chunk callback fires from this same worker thread
			// inside RunStreamingSynthesis. Capture timing for diagnostics.
			const double SynthT0 = FPlatformTime::Seconds();
			TSharedRef<int32, ESPMode::ThreadSafe> ChunkCount =
				MakeShared<int32, ESPMode::ThreadSafe>(0);
			TSharedRef<double, ESPMode::ThreadSafe> FirstChunkAt =
				MakeShared<double, ESPMode::ThreadSafe>(-1.0);
			TSharedRef<double, ESPMode::ThreadSafe> LastChunkAt =
				MakeShared<double, ESPMode::ThreadSafe>(SynthT0);

			FInoNeuTtsStreamCallbacks Callbacks;
			Callbacks.OnChunk = [SynthT0, ChunkCount, FirstChunkAt, LastChunkAt]
				(TArray<uint8> ChunkBytes, bool bIsFinal)
			{
				const double Now = FPlatformTime::Seconds();
				const int32 Idx = ++(*ChunkCount);
				if (*FirstChunkAt < 0.0)
				{
					*FirstChunkAt = Now;
				}
				const double DeltaMs = (Now - *LastChunkAt) * 1000.0;
				*LastChunkAt = Now;

				const int32 NumSamples = ChunkBytes.Num() / 2;
				const float ChunkMs    = (NumSamples * 1000.0f) / 24000.0f;

				AsyncTask(ENamedThreads::GameThread,
					[Idx, DeltaMs, ChunkMs, NumBytes = ChunkBytes.Num(), bIsFinal]()
				{
					UE_LOG(LogInoNeuTts, Display,
						TEXT("  chunk %2d  %s  delta=%6.1f ms  audio=%5.1f ms  bytes=%d"),
						Idx,
						bIsFinal ? TEXT("final") : TEXT("     "),
						DeltaMs, ChunkMs, NumBytes);
				});
			};

			const FInoNeuTtsResult Result = RunStreamingSynthesis(
				*Runner, Text, Voice, Options,
				ChunkTokens, Callbacks, /*CancelFlag*/ nullptr);

			const double TtfaMs = (*FirstChunkAt > 0.0)
				? (*FirstChunkAt - SynthT0) * 1000.0
				: 0.0;

			bool bWavOk = false;
			if (Result.bSuccess)
			{
				bWavOk = UInoAudioFunctionLibrary::WriteInt16PcmBytesAsWav(
					OutPath,
					TArrayView<const uint8>(Result.AudioSamples),
					Result.SampleRate);
			}

			AsyncTask(ENamedThreads::GameThread,
				[Result, OutPath, LoadElapsed, TtfaMs, bWavOk]()
			{
				UE_LOG(LogInoNeuTts, Display,
					TEXT("Load:    %.2f s"), LoadElapsed);

				if (!Result.bSuccess)
				{
					UE_LOG(LogInoNeuTts, Error,
						TEXT("FAIL (synth): %s"), *Result.ErrorMessage);
					return;
				}

				UE_LOG(LogInoNeuTts, Display,
					TEXT("TTFA:    %.1f ms"), TtfaMs);
				UE_LOG(LogInoNeuTts, Display,
					TEXT("Synth:   %.2f s wall, %.2f s audio (RTF %.2f)"),
					Result.GenerationTimeSeconds,
					Result.DurationSeconds,
					Result.RealTimeFactor);

				if (bWavOk)
				{
					UE_LOG(LogInoNeuTts, Display,
						TEXT("PASS — wrote %s"), *OutPath);
				}
				else
				{
					UE_LOG(LogInoNeuTts, Error,
						TEXT("FAIL — synth ok but WAV write to %s failed"), *OutPath);
				}
			});
		});
	}

	FAutoConsoleCommand GStreamSynthTest(
		TEXT("Ino.NeuTts.StreamSynthTest"),
		TEXT("Streaming NeuTTS synth: per-chunk timing log + final WAV. ")
		TEXT("Args: [voice=jo] [backbone DisplayName] [chunk_tokens=25] [text...]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamSynthTest));
}
