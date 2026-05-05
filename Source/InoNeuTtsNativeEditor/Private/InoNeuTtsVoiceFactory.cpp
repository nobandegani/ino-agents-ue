// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsVoiceFactory.h"
#include "InoNeuTtsNativeEditor.h"
#include "InoNeuTtsVoiceAsset.h"

#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UInoNeuTtsVoiceFactory::UInoNeuTtsVoiceFactory()
{
	bCreateNew  = false;        // can't create from scratch — must come from a .inv source
	bEditAfterNew = true;       // open in editor after import for instant inspection
	bEditorImport = true;
	bText = false;              // we read raw bytes via FactoryCreateFile (JSON is text but
	                            // FactoryCreateFile gives us the path which is more flexible)

	SupportedClass = UInoNeuTtsVoiceAsset::StaticClass();

	// "ext;Description shown in import dialog". UE uses the extension to
	// route .inv files to this factory automatically.
	Formats.Add(TEXT("inv;NeuTTS Voice (JSON)"));
}

bool UInoNeuTtsVoiceFactory::FactoryCanImport(const FString& Filename)
{
	const FString Ext = FPaths::GetExtension(Filename, /*bIncludeDot*/ false).ToLower();
	return Ext == TEXT("inv");
}

UObject* UInoNeuTtsVoiceFactory::FactoryCreateFile(
	UClass*           InClass,
	UObject*          InParent,
	FName             InName,
	EObjectFlags      Flags,
	const FString&    Filename,
	const TCHAR*      /*Parms*/,
	FFeedbackContext* /*Warn*/,
	bool&             bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	UInoNeuTtsVoiceAsset* Asset = NewObject<UInoNeuTtsVoiceAsset>(
		InParent, InClass, InName, Flags);

	FString Error;
	if (!LoadInvFileIntoAsset(Asset, Filename, Error))
	{
		UE_LOG(LogInoNeuTtsEditor, Error,
			TEXT("UInoNeuTtsVoiceFactory: failed to import '%s': %s"),
			*Filename, *Error);
		// Returning nullptr cancels the import; the partial Asset is GC'd.
		return nullptr;
	}

	// Mark dirty so the freshly-imported asset prompts the user to save.
	Asset->MarkPackageDirty();

	UE_LOG(LogInoNeuTtsEditor, Log,
		TEXT("UInoNeuTtsVoiceFactory: imported '%s' -> %s (Name='%s', Lang='%s', RefCodes=%d)"),
		*Filename, *Asset->GetName(),
		*Asset->Name, *Asset->Language, Asset->RefCodes.Num());

	return Asset;
}

// ============================================================================
//  FReimportHandler
// ============================================================================

bool UInoNeuTtsVoiceFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	UInoNeuTtsVoiceAsset* Asset = Cast<UInoNeuTtsVoiceAsset>(Obj);
	if (Asset == nullptr)
	{
		return false;
	}
	if (Asset->AssetImportData != nullptr)
	{
		Asset->AssetImportData->ExtractFilenames(OutFilenames);
		return true;
	}
	// No import-data record (asset was created some other way) — still
	// allow reimport so the user can point us at a source file.
	OutFilenames.Add(FString());
	return true;
}

void UInoNeuTtsVoiceFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UInoNeuTtsVoiceAsset* Asset = Cast<UInoNeuTtsVoiceAsset>(Obj);
	if (Asset != nullptr && Asset->AssetImportData != nullptr && NewReimportPaths.Num() == 1)
	{
		Asset->AssetImportData->UpdateFilenameOnly(NewReimportPaths[0]);
	}
}

EReimportResult::Type UInoNeuTtsVoiceFactory::Reimport(UObject* Obj)
{
	UInoNeuTtsVoiceAsset* Asset = Cast<UInoNeuTtsVoiceAsset>(Obj);
	if (Asset == nullptr || Asset->AssetImportData == nullptr)
	{
		return EReimportResult::Failed;
	}

	const FString SourcePath =
		Asset->AssetImportData->GetFirstFilename();
	if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath))
	{
		UE_LOG(LogInoNeuTtsEditor, Warning,
			TEXT("UInoNeuTtsVoiceFactory: reimport failed — source file missing: '%s'"),
			*SourcePath);
		return EReimportResult::Failed;
	}

	FString Error;
	if (!LoadInvFileIntoAsset(Asset, SourcePath, Error))
	{
		UE_LOG(LogInoNeuTtsEditor, Error,
			TEXT("UInoNeuTtsVoiceFactory: reimport from '%s' failed: %s"),
			*SourcePath, *Error);
		return EReimportResult::Failed;
	}

	Asset->MarkPackageDirty();
	UE_LOG(LogInoNeuTtsEditor, Log,
		TEXT("UInoNeuTtsVoiceFactory: reimported '%s' from '%s' (RefCodes=%d)"),
		*Asset->GetName(), *SourcePath, Asset->RefCodes.Num());
	return EReimportResult::Succeeded;
}

// ============================================================================
//  Shared parse path
// ============================================================================

bool UInoNeuTtsVoiceFactory::LoadInvFileIntoAsset(
	UInoNeuTtsVoiceAsset* Asset,
	const FString&        AbsoluteSourcePath,
	FString&              OutError) const
{
	if (Asset == nullptr)
	{
		OutError = TEXT("Asset is null.");
		return false;
	}

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *AbsoluteSourcePath))
	{
		OutError = FString::Printf(
			TEXT("Could not read file '%s'."), *AbsoluteSourcePath);
		return false;
	}

	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(FileContents);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Could not parse JSON from '%s'."), *AbsoluteSourcePath);
		return false;
	}

	// Pull each field — all optional except RefCodes. Empty strings are
	// fine; runtime fallbacks (live phonemize, infer name from filename)
	// handle them.
	FString  ParsedName;
	FString  ParsedLanguage;
	FString  ParsedRefText;
	FString  ParsedRefPhones;
	const TArray<TSharedPtr<FJsonValue>>* CodesArray = nullptr;

	Json->TryGetStringField(TEXT("Name"),      ParsedName);
	Json->TryGetStringField(TEXT("Language"),  ParsedLanguage);
	Json->TryGetStringField(TEXT("RefText"),   ParsedRefText);
	Json->TryGetStringField(TEXT("RefPhones"), ParsedRefPhones);
	if (!Json->TryGetArrayField(TEXT("RefCodes"), CodesArray))
	{
		OutError = TEXT("Missing required 'RefCodes' array.");
		return false;
	}

	TArray<int32> ParsedRefCodes;
	ParsedRefCodes.Reserve(CodesArray->Num());
	for (const TSharedPtr<FJsonValue>& V : *CodesArray)
	{
		double D = 0.0;
		if (V.IsValid() && V->TryGetNumber(D))
		{
			ParsedRefCodes.Add(static_cast<int32>(D));
		}
	}

	if (ParsedRefCodes.Num() == 0)
	{
		OutError = TEXT("'RefCodes' array was empty or all-non-numeric.");
		return false;
	}

	// Default Name to the source filename stem when the JSON omits it —
	// matches the legacy voice-registry behaviour.
	if (ParsedName.IsEmpty())
	{
		ParsedName = FPaths::GetBaseFilename(AbsoluteSourcePath);
	}
	if (ParsedLanguage.IsEmpty())
	{
		ParsedLanguage = TEXT("en-us");
	}

	// Commit. Done LAST so a parse failure leaves the asset's previous
	// contents intact (relevant for re-import).
	Asset->Name      = MoveTemp(ParsedName);
	Asset->Language  = MoveTemp(ParsedLanguage);
	Asset->RefText   = MoveTemp(ParsedRefText);
	Asset->RefPhones = MoveTemp(ParsedRefPhones);
	Asset->RefCodes  = MoveTemp(ParsedRefCodes);

	if (Asset->AssetImportData != nullptr)
	{
		Asset->AssetImportData->Update(AbsoluteSourcePath);
	}

	return true;
}
