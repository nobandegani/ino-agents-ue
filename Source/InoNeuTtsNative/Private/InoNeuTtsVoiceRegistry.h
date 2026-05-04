// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"

namespace InoNeuTtsNative
{
	/**
	 * Static helpers for loading FInoNeuTtsVoice instances from the
	 * .nvoice.json files produced by Plugins/InoAgents/NeuTTS/scripts/
	 * build-voices.py.
	 *
	 * Voice files contain pre-encoded NeuCodec speech tokens (ref_codes)
	 * plus the reference text. Phonemization of ref_text into ref_phones
	 * is deferred to first synth (lazy via the InoSpeakNG plugin).
	 */
	class FInoNeuTtsVoiceRegistry
	{
	public:
		/**
		 * Parse one .nvoice.json file into a voice. Returns false (with
		 * a diagnostic log) and leaves OutVoice's bIsValid=false if the
		 * file is missing, malformed, or missing required fields.
		 *
		 * Required JSON keys: name, language, ref_text, ref_codes (int array).
		 * Optional:           ref_phones.
		 */
		static bool LoadFromFile(const FString& FilePath, FInoNeuTtsVoice& OutVoice);

		/**
		 * Resolves Plugins/InoAgents/NeuTTS/voices/ via IPluginManager.
		 * Returns empty string if the InoAgents plugin can't be located.
		 */
		static FString GetBundledVoicesDir();

		/**
		 * Scans GetBundledVoicesDir() for *.nvoice.json files and loads
		 * each. Invalid files are logged and skipped. Returns the set
		 * of voices that loaded cleanly.
		 */
		static TArray<FInoNeuTtsVoice> LoadBundledVoices();
	};
}
