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
	 * End-to-end synth test:
	 *   1. Load Nano (or Air, via second arg) on a worker thread
	 *   2. Load voice from bundled .nvoice.json (default "jo")
	 *   3. Run synthesis with the supplied text (default "Hello there.")
	 *   4. Save result as WAV under <Project>/Saved/InoNeuTtsTest.wav
	 *   5. Log RTF + sample count
	 *
	 * Args:  [voice]  [variant]  [text...]
	 *   voice   = bundled voice name (jo, dave, greta, juliette, mateo)
	 *   variant = nano (default) | air
	 *   text    = remaining args joined by single spaces
	 */
	void RunSynthTest(const TArray<FString>& Args)
	{
		using namespace InoNeuTtsNative;

		// Parse args
		FString VoiceName = TEXT("jo");
		EInoNeuTtsVariant Variant = EInoNeuTtsVariant::Nano;
		FString Text;

		if (Args.Num() >= 1)
		{
			VoiceName = Args[0];
		}

		int32 NextArg = 1;
		if (Args.Num() >= 2)
		{
			const FString Lower = Args[1].ToLower();
			if (Lower == TEXT("nano") || Lower == TEXT("air"))
			{
				Variant = (Lower == TEXT("air")) ? EInoNeuTtsVariant::Air
				                                 : EInoNeuTtsVariant::Nano;
				NextArg = 2;
			}
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

		// Resolve voice file
		const FString VoicesDir = FInoNeuTtsVoiceRegistry::GetBundledVoicesDir();
		const FString VoicePath = FPaths::Combine(VoicesDir,
			FString::Printf(TEXT("%s.nvoice.json"), *VoiceName));

		FInoNeuTtsVoice Voice;
		if (!FInoNeuTtsVoiceRegistry::LoadFromFile(VoicePath, Voice))
		{
			UE_LOG(LogInoNeuTts, Error,
				TEXT("FAIL: voice '%s' not found at %s"),
				*VoiceName, *VoicePath);
			return;
		}

		const FString GgufPath = ResolveGgufPath(Variant);
		const FString OnnxPath = ResolveOnnxDecoderPath();

		UE_LOG(LogInoNeuTts, Display, TEXT("=== Ino.NeuTts.SynthTest ==="));
		UE_LOG(LogInoNeuTts, Display, TEXT("Voice:   %s (lang=%s, codes=%d)"),
			*Voice.Name, *Voice.Language, Voice.RefCodes.Num());
		UE_LOG(LogInoNeuTts, Display, TEXT("Variant: %s"), *VariantToString(Variant));
		UE_LOG(LogInoNeuTts, Display, TEXT("Text:    %s"), *Text);

		FInoNeuTtsConfig Config;
		Config.Variant = Variant;
		FInoNeuTtsOptions Options;

		const FString OutPath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("InoNeuTtsTest.wav"));

		// Dispatch the whole pipeline (model load + synth + WAV save)
		// to a worker thread so the editor / PIE game thread stays
		// responsive throughout.
		Async(EAsyncExecution::ThreadPool,
			[Config, GgufPath, OnnxPath, Voice, Text, Options, OutPath]() mutable
		{
			const double LoadT0 = FPlatformTime::Seconds();

			FString LoadError;
			TUniquePtr<FInoNeuTtsRunner> Runner = FInoNeuTtsRunner::Create(
				Config, GgufPath, OnnxPath, LoadError);

			const double LoadElapsed = FPlatformTime::Seconds() - LoadT0;

			if (!Runner.IsValid())
			{
				AsyncTask(ENamedThreads::GameThread, [Err = MoveTemp(LoadError)]()
				{
					UE_LOG(LogInoNeuTts, Error, TEXT("FAIL (load): %s"), *Err);
				});
				return;
			}

			const FInoNeuTtsResult Result = RunSynthesis(*Runner, Text, Voice, Options);

			// Save WAV from the worker thread; file IO is happy here.
			bool bWavOk = false;
			if (Result.bSuccess)
			{
				bWavOk = UInoAudioFunctionLibrary::WriteInt16PcmBytesAsWav(
					OutPath,
					TArrayView<const uint8>(Result.AudioSamples),
					Result.SampleRate);
			}

			AsyncTask(ENamedThreads::GameThread,
				[Result, OutPath, LoadElapsed, bWavOk]()
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

	FAutoConsoleCommand GSynthTest(
		TEXT("Ino.NeuTts.SynthTest"),
		TEXT("End-to-end NeuTTS synth: loads model + voice, generates audio, ")
		TEXT("saves <Project>/Saved/InoNeuTtsTest.wav. ")
		TEXT("Args: [voice=jo] [nano|air=nano] [text...]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunSynthTest));
}
