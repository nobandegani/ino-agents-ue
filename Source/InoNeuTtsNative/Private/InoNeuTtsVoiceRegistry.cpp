// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsVoiceRegistry.h"
#include "InoNeuTtsLog.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace InoNeuTtsNative
{
	bool FInoNeuTtsVoiceRegistry::LoadFromFile(
		const FString& FilePath,
		FInoNeuTtsVoice& OutVoice)
	{
		// Reset to a known-invalid state up-front so partial parses don't
		// leak old data via OutVoice.
		OutVoice = FInoNeuTtsVoice{};

		FString JsonContents;
		if (!FFileHelper::LoadFileToString(JsonContents, *FilePath))
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("Failed to read voice file '%s'."), *FilePath);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(JsonContents);

		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("Failed to parse voice file '%s' as JSON."), *FilePath);
			return false;
		}

		// Required fields.
		FString Name, Language, RefText;
		const TArray<TSharedPtr<FJsonValue>>* RefCodesArray = nullptr;

		if (!Root->TryGetStringField(TEXT("name"), Name) ||
			!Root->TryGetStringField(TEXT("language"), Language) ||
			!Root->TryGetStringField(TEXT("ref_text"), RefText) ||
			!Root->TryGetArrayField(TEXT("ref_codes"), RefCodesArray))
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("Voice file '%s' missing required field(s) ")
				TEXT("(need name, language, ref_text, ref_codes)."),
				*FilePath);
			return false;
		}

		// Optional pre-baked phonemization. Empty here means the runtime
		// will phonemize ref_text via InoSpeakNG on first synth.
		FString RefPhones;
		Root->TryGetStringField(TEXT("ref_phones"), RefPhones);

		// Convert ref_codes JSON numbers → TArray<int32>. JSON numbers
		// arrive as doubles; round-trip through TryGetNumber(int32&) so a
		// stray non-integer logs a warning instead of silently truncating.
		TArray<int32> RefCodes;
		RefCodes.Reserve(RefCodesArray->Num());
		for (const TSharedPtr<FJsonValue>& Value : *RefCodesArray)
		{
			int32 Code = 0;
			if (Value.IsValid() && Value->TryGetNumber(Code))
			{
				RefCodes.Add(Code);
			}
			else
			{
				UE_LOG(LogInoNeuTts, Warning,
					TEXT("Voice file '%s' has non-integer entry in ref_codes; skipping."),
					*FilePath);
			}
		}

		if (RefCodes.Num() == 0)
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("Voice file '%s' has no valid ref_codes."), *FilePath);
			return false;
		}

		OutVoice.Name      = MoveTemp(Name);
		OutVoice.Language  = MoveTemp(Language);
		OutVoice.RefText   = MoveTemp(RefText);
		OutVoice.RefPhones = MoveTemp(RefPhones);
		OutVoice.RefCodes  = MoveTemp(RefCodes);
		OutVoice.bIsValid  = true;
		return true;
	}

	FString FInoNeuTtsVoiceRegistry::GetBundledVoicesDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("NeuTTS"), TEXT("voices"));
	}

	TArray<FInoNeuTtsVoice> FInoNeuTtsVoiceRegistry::LoadBundledVoices()
	{
		TArray<FInoNeuTtsVoice> Voices;

		const FString Dir = GetBundledVoicesDir();
		if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("Bundled voices dir not found: %s"), *Dir);
			return Voices;
		}

		TArray<FString> Filenames;
		IFileManager::Get().FindFiles(
			Filenames,
			*FPaths::Combine(Dir, TEXT("*.nvoice.json")),
			/*bFiles*/ true,
			/*bDirectories*/ false);

		Voices.Reserve(Filenames.Num());
		for (const FString& Filename : Filenames)
		{
			const FString Path = FPaths::Combine(Dir, Filename);
			FInoNeuTtsVoice Voice;
			if (LoadFromFile(Path, Voice))
			{
				Voices.Add(MoveTemp(Voice));
			}
		}

		return Voices;
	}
}
