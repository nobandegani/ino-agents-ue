// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"

#include "InoNeuTtsVoiceFactory.generated.h"

/**
 * Editor-only factory that turns a `.inv` source file (plain JSON,
 * same shape as the legacy `.nvoice.json` format produced offline by
 * `Plugins/InoAgents/NeuTTS/scripts/build-voices.py`) into a
 * UInoNeuTtsVoiceAsset.
 *
 * Drag/drop support: registering "inv" in the Formats array makes UE's
 * Content Browser auto-route any imported `.inv` file to this factory.
 * The user just drags the file into a folder and the asset appears.
 *
 * Re-import: implements FReimportHandler so right-click → Reimport on a
 * voice asset re-parses the source file. AssetImportData on the asset
 * tracks the source path for this. Useful when the offline encoder is
 * re-run with a tweaked reference WAV / transcript.
 *
 * Source JSON shape (every field optional except RefCodes — the
 * factory will accept missing Name/Language/RefText/RefPhones and the
 * runtime fallback paths handle empties):
 *   {
 *     "Name":      "jo",
 *     "Language":  "en-us",
 *     "RefText":   "...",
 *     "RefPhones": "h@l'oU D'e@ ...",
 *     "RefCodes":  [5234, 7891, 4099, ...]
 *   }
 */
UCLASS(hidecategories = Object)
class INONEUTTSNATIVEEDITOR_API UInoNeuTtsVoiceFactory : public UFactory, public FReimportHandler
{
	GENERATED_BODY()

public:
	UInoNeuTtsVoiceFactory();

	// ---- UFactory ----
	virtual UObject* FactoryCreateFile(
		UClass*           InClass,
		UObject*          InParent,
		FName             InName,
		EObjectFlags      Flags,
		const FString&    Filename,
		const TCHAR*      Parms,
		FFeedbackContext* Warn,
		bool&             bOutOperationCanceled) override;

	virtual bool FactoryCanImport(const FString& Filename) override;

	// ---- FReimportHandler ----
	virtual bool   CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void   SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
	virtual int32  GetPriority() const override { return ImportPriority; }

private:
	/**
	 * Shared parse + populate path. Reads the JSON at AbsoluteSourcePath,
	 * fills the asset's fields, updates AssetImportData. Returns true on
	 * success; on failure logs an error to LogInoNeuTtsEditor and leaves
	 * the asset untouched.
	 */
	bool LoadInvFileIntoAsset(
		class UInoNeuTtsVoiceAsset* Asset,
		const FString&              AbsoluteSourcePath,
		FString&                    OutError) const;
};
