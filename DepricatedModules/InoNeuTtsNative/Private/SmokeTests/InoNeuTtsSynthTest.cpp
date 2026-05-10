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
	 *   1. Load the configured backbone (first entry in Project Settings,
	 *      or override via the optional second arg) on a worker thread
	 *   2. Load voice from bundled .inv source file (default "jo")
	 *   3. Run synthesis with the supplied text (default "Hello there.")
	 *   4. Save result as WAV under <Project>/Saved/InoNeuTtsTest.wav
	 *   5. Log RTF + sample count
	 *
	 * Args:  [voice]  [backbone DisplayName]  [text...]
	 *   voice    = bundled voice name (jo, dave, greta, juliette, mateo)
	 *   backbone = optional Project Settings backbone DisplayName; if it
	 *              looks like a sentence (contains a space) we treat it
	 *              as the start of the text instead.
	 *   text     = remaining args joined by single spaces
	 */
	void RunSynthTest(const TArray<FString>& Args)
	{
		using namespace InoNeuTtsNative;

		// Parse args
		FString VoiceName     = TEXT("jo");
		FString BackboneName;
		FString Text;

		if (Args.Num() >= 1)
		{
			VoiceName = Args[0];
		}

		int32 NextArg = 1;
		// If the second arg has no spaces, treat it as a backbone name
		// override; if it's punctuated like text, fall through to the
		// text arm. (Heuristic — we can't tell perfectly, but
		// DisplayNames are kebab-case by convention.)
		if (Args.Num() >= 2 && !Args[1].Contains(TEXT(" ")) && !Args[1].EndsWith(TEXT(".")))
		{
			BackboneName = Args[1];
			NextArg = 2;
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
			FString::Printf(TEXT("%s.inv"), *VoiceName));

		FInoNeuTtsVoice Voice;
		if (!FInoNeuTtsVoiceRegistry::LoadFromFile(VoicePath, Voice))
		{
			UE_LOG(LogInoNeuTts, Error,
				TEXT("FAIL: voice '%s' not found at %s"),
				*VoiceName, *VoicePath);
			return;
		}

		const FString GgufPath = ResolveGgufPath(BackboneName);
		const FString OnnxPath = ResolveOnnxDecoderPath();

		UE_LOG(LogInoNeuTts, Display, TEXT("=== Ino.NeuTts.SynthTest ==="));
		UE_LOG(LogInoNeuTts, Display, TEXT("Voice:    %s (lang=%s, codes=%d)"),
			*Voice.Name, *Voice.Language, Voice.RefCodes.Num());
		UE_LOG(LogInoNeuTts, Display, TEXT("Backbone: %s"),
			BackboneName.IsEmpty() ? TEXT("<first entry>") : *BackboneName);
		UE_LOG(LogInoNeuTts, Display, TEXT("Text:     %s"), *Text);

		FInoNeuTtsConfig Config;
		Config.BackboneModelName = BackboneName;
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
		TEXT("Args: [voice=jo] [backbone DisplayName] [text...]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunSynthTest));
}
