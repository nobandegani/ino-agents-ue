// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"

#include "InoNeuTTSVoiceFactory.generated.h"

class UInoNeuTTSVoiceAsset;

/**
 * Editor-only factory that turns a `.inv` source file (plain JSON) into
 * a UInoNeuTTSVoiceAsset.
 *
 * Workflow:
 *
 *   1. Your Python encoder writes the voice data as JSON. Rename
 *      `.json` → `.inv`.
 *   2. Drag the `.inv` into a Content Browser folder. UE matches the
 *      extension against this factory's `Formats` array and calls
 *      FactoryCreateFile.
 *   3. The factory parses the JSON, populates a fresh
 *      UInoNeuTTSVoiceAsset, and records the source path on
 *      AssetImportData so right-click → Reimport works.
 *
 * Source JSON shape (every field optional except RefCodes — the
 * factory fills sensible defaults for missing Name / Language and the
 * runtime tolerates an empty RefText / RefPhones):
 *
 *   {
 *     "Name":      "jo",
 *     "Language":  "en-us",
 *     "RefText":   "Hello, my name is Joe...",
 *     "RefPhones": "h@l'oU D'e@ ...",
 *     "RefCodes":  [5234, 7891, 4099, ...]
 *   }
 *
 * Keys are matched case-insensitively, AND we accept both snake_case
 * (`ref_text`, `ref_codes`) and PascalCase (`RefText`, `RefCodes`) so
 * whatever convention your Python script emits works.
 */
UCLASS(hidecategories = Object)
class INONEUTTSEDITOR_API UInoNeuTTSVoiceFactory : public UFactory, public FReimportHandler
{
    GENERATED_BODY()

public:
    UInoNeuTTSVoiceFactory();

    //~ UFactory
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
    //~ End UFactory

    //~ FReimportHandler
    virtual bool                  CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void                  SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32                 GetPriority() const override { return ImportPriority; }
    //~ End FReimportHandler

private:
    /** Shared parse + populate path. Reads the JSON at
     *  AbsoluteSourcePath, fills the asset's fields, updates
     *  AssetImportData. Returns true on success; on failure logs an
     *  error and leaves the asset's previous contents intact
     *  (important for Reimport — a bad reimport mustn't trash a
     *  previously-working asset). */
    bool LoadInvFileIntoAsset(
        UInoNeuTTSVoiceAsset* Asset,
        const FString&        AbsoluteSourcePath,
        FString&              OutError) const;
};
