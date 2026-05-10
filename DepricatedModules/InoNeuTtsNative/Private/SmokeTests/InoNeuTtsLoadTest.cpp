// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsCommon.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsRunner.h"
#include "InoNeuTtsTypes.h"

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Onnx/InoOnnxSession.h"

namespace
{
	void RunLoadTest(const TArray<FString>& Args)
	{
		using namespace InoNeuTtsNative;

		FInoNeuTtsConfig Config;
		if (Args.Num() > 0)
		{
			// Optional: pass a backbone DisplayName to pick a specific
			// entry from BackboneModels; otherwise the first entry wins.
			Config.BackboneModelName = Args[0];
		}

		const FString GgufPath = ResolveGgufPath(Config.BackboneModelName);
		const FString OnnxPath = ResolveOnnxDecoderPath(Config.DecoderModelName);

		UE_LOG(LogInoNeuTts, Display, TEXT("=== Ino.NeuTts.LoadTest ==="));
		UE_LOG(LogInoNeuTts, Display, TEXT("Backbone: %s"),
			Config.BackboneModelName.IsEmpty() ? TEXT("<first entry>") : *Config.BackboneModelName);
		UE_LOG(LogInoNeuTts, Display, TEXT("GGUF:     %s"), *GgufPath);
		UE_LOG(LogInoNeuTts, Display, TEXT("ONNX:     %s"), *OnnxPath);

		const double StartTime = FPlatformTime::Seconds();

		// Dispatch the actual load to a worker thread so the editor / PIE
		// game thread stays responsive while the multi-GB GGUF mmap runs.
		Async(EAsyncExecution::ThreadPool, [Config, GgufPath, OnnxPath, StartTime]()
		{
			FString Error;
			TUniquePtr<FInoNeuTtsRunner> Runner = FInoNeuTtsRunner::Create(
				Config, GgufPath, OnnxPath, Error);

			const double Elapsed = FPlatformTime::Seconds() - StartTime;

			AsyncTask(ENamedThreads::GameThread,
				[RunnerPtr = MoveTemp(Runner), Error = MoveTemp(Error), Elapsed]() mutable
			{
				if (RunnerPtr.IsValid())
				{
					UE_LOG(LogInoNeuTts, Display,
						TEXT("PASS: load complete in %.2f s"), Elapsed);
					UE_LOG(LogInoNeuTts, Display,
						TEXT("  description: %s"), *RunnerPtr->GetModelDescription());
					UE_LOG(LogInoNeuTts, Display,
						TEXT("  stop_token : %d"), RunnerPtr->GetStopTokenId());

					if (FInoOnnxSession* Decoder = RunnerPtr->GetDecoder())
					{
						Decoder->LogMetadata();
					}

					// Runner falls out of scope here on the game thread —
					// destructor frees model + ctx + ONNX session cleanly.
				}
				else
				{
					UE_LOG(LogInoNeuTts, Error, TEXT("FAIL: %s"), *Error);
				}
			});
		});
	}

	FAutoConsoleCommand GLoadTest(
		TEXT("Ino.NeuTts.LoadTest"),
		TEXT("Load the NeuTTS GGUF backbone + NeuCodec ONNX decoder. ")
		TEXT("Args: [backbone DisplayName] (default = first entry in Project Settings -> ")
		TEXT("Ino NeuTTS Native -> NeuTTS Backbone). Non-blocking, dispatches to ThreadPool."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadTest));
}
