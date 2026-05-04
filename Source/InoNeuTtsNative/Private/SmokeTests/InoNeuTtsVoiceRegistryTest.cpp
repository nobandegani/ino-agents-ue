// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsLog.h"
#include "InoNeuTtsTypes.h"
#include "InoNeuTtsVoiceRegistry.h"

#include "HAL/IConsoleManager.h"

namespace
{
	void RunVoiceRegistryTest(const TArray<FString>& /*Args*/)
	{
		using namespace InoNeuTtsNative;

		UE_LOG(LogInoNeuTts, Display, TEXT("=== Ino.NeuTts.VoiceRegistryTest ==="));

		const FString VoicesDir = FInoNeuTtsVoiceRegistry::GetBundledVoicesDir();
		UE_LOG(LogInoNeuTts, Display, TEXT("Voices dir: %s"), *VoicesDir);

		const TArray<FInoNeuTtsVoice> Voices =
			FInoNeuTtsVoiceRegistry::LoadBundledVoices();

		UE_LOG(LogInoNeuTts, Display,
			TEXT("Loaded %d bundled voice(s):"), Voices.Num());

		for (const FInoNeuTtsVoice& Voice : Voices)
		{
			UE_LOG(LogInoNeuTts, Display,
				TEXT("  %-10s  language=%-6s  codes=%4d  ref_text=\"%s\""),
				*Voice.Name,
				*Voice.Language,
				Voice.RefCodes.Num(),
				*Voice.RefText.Left(60));
		}

		if (Voices.Num() > 0)
		{
			UE_LOG(LogInoNeuTts, Display, TEXT("PASS"));
		}
		else
		{
			UE_LOG(LogInoNeuTts, Error,
				TEXT("FAIL: no voices loaded. Did you run ")
				TEXT("Plugins/InoAgents/NeuTTS/scripts/build-voices.py?"));
		}
	}

	FAutoConsoleCommand GVoiceRegistryTest(
		TEXT("Ino.NeuTts.VoiceRegistryTest"),
		TEXT("Loads all bundled .nvoice.json files and logs name + language + code count."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunVoiceRegistryTest));
}
