// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSVoiceFactory.h"

#include "InoNeuTTSVoiceAsset.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UInoNeuTTSVoiceFactory::UInoNeuTTSVoiceFactory()
{
    bCreateNew    = false;  // can't create from scratch — must come from a .inv source
    bEditAfterNew = true;   // open in editor after import for instant inspection
    bEditorImport = true;
    bText         = false;  // we use FactoryCreateFile which gives us the path

    SupportedClass = UInoNeuTTSVoiceAsset::StaticClass();

    // "ext;Description shown in import dialog". UE uses the extension
    // to route .inv files to this factory automatically.
    Formats.Add(TEXT("inv;NeuTTS Voice (JSON)"));
}

bool UInoNeuTTSVoiceFactory::FactoryCanImport(const FString& Filename)
{
    const FString Ext = FPaths::GetExtension(Filename, /*bIncludeDot*/ false).ToLower();
    return Ext == TEXT("inv");
}

UObject* UInoNeuTTSVoiceFactory::FactoryCreateFile(
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

    UInoNeuTTSVoiceAsset* Asset = NewObject<UInoNeuTTSVoiceAsset>(
        InParent, InClass, InName, Flags);

    FString Error;
    if (!LoadInvFileIntoAsset(Asset, Filename, Error))
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTSEditor][Factory] failed to import '%s': %s"),
            *Filename, *Error);
        return nullptr;  // cancel the import; partial asset is GC'd
    }

    Asset->MarkPackageDirty();

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTSEditor][Factory] imported '%s' → %s (Name='%s', Lang='%s', RefCodes=%d)"),
        *Filename, *Asset->GetName(),
        *Asset->Name, *Asset->Language, Asset->RefCodes.Num());

    return Asset;
}

// =====================================================================
//  FReimportHandler
// =====================================================================

bool UInoNeuTTSVoiceFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    UInoNeuTTSVoiceAsset* Asset = Cast<UInoNeuTTSVoiceAsset>(Obj);
    if (!Asset) return false;

    if (Asset->AssetImportData)
    {
        Asset->AssetImportData->ExtractFilenames(OutFilenames);
        return true;
    }
    // No recorded import path — still allow reimport so the user can
    // point us at a source file via the file picker.
    OutFilenames.Add(FString());
    return true;
}

void UInoNeuTTSVoiceFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
    UInoNeuTTSVoiceAsset* Asset = Cast<UInoNeuTTSVoiceAsset>(Obj);
    if (Asset && Asset->AssetImportData && NewReimportPaths.Num() == 1)
    {
        Asset->AssetImportData->UpdateFilenameOnly(NewReimportPaths[0]);
    }
}

EReimportResult::Type UInoNeuTTSVoiceFactory::Reimport(UObject* Obj)
{
    UInoNeuTTSVoiceAsset* Asset = Cast<UInoNeuTTSVoiceAsset>(Obj);
    if (!Asset || !Asset->AssetImportData)
    {
        return EReimportResult::Failed;
    }

    const FString SourcePath = Asset->AssetImportData->GetFirstFilename();
    if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath))
    {
        UE_LOG(LogInoAgents, Warning,
            TEXT("[NeuTTSEditor][Factory] reimport failed — source file missing: '%s'"),
            *SourcePath);
        return EReimportResult::Failed;
    }

    FString Error;
    if (!LoadInvFileIntoAsset(Asset, SourcePath, Error))
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTSEditor][Factory] reimport from '%s' failed: %s"),
            *SourcePath, *Error);
        return EReimportResult::Failed;
    }

    Asset->MarkPackageDirty();
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTSEditor][Factory] reimported '%s' from '%s' (RefCodes=%d)"),
        *Asset->GetName(), *SourcePath, Asset->RefCodes.Num());
    return EReimportResult::Succeeded;
}

// =====================================================================
//  Shared parse path
// =====================================================================

bool UInoNeuTTSVoiceFactory::LoadInvFileIntoAsset(
    UInoNeuTTSVoiceAsset* Asset,
    const FString&        AbsoluteSourcePath,
    FString&              OutError) const
{
    if (!Asset)
    {
        OutError = TEXT("Asset is null.");
        return false;
    }

    FString FileContents;
    if (!FFileHelper::LoadFileToString(FileContents, *AbsoluteSourcePath))
    {
        OutError = FString::Printf(TEXT("Could not read file '%s'."), *AbsoluteSourcePath);
        return false;
    }

    TSharedPtr<FJsonObject> Json;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContents);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
    {
        OutError = FString::Printf(TEXT("Could not parse JSON from '%s'."), *AbsoluteSourcePath);
        return false;
    }

    // Helper: try snake_case key first, fall back to PascalCase.
    // FJsonObject::TryGetStringField is case-sensitive by default —
    // we accept either convention so the Python script's emit style
    // doesn't matter.
    auto ReadString = [&Json](const TCHAR* SnakeKey, const TCHAR* PascalKey, FString& Out)
    {
        if (!Json->TryGetStringField(SnakeKey, Out))
        {
            Json->TryGetStringField(PascalKey, Out);
        }
    };

    FString  ParsedName;
    FString  ParsedLanguage;
    FString  ParsedRefText;
    FString  ParsedRefPhones;
    const TArray<TSharedPtr<FJsonValue>>* CodesArray = nullptr;

    ReadString(TEXT("name"),       TEXT("Name"),      ParsedName);
    ReadString(TEXT("language"),   TEXT("Language"),  ParsedLanguage);
    ReadString(TEXT("ref_text"),   TEXT("RefText"),   ParsedRefText);
    ReadString(TEXT("ref_phones"), TEXT("RefPhones"), ParsedRefPhones);
    if (!Json->TryGetArrayField(TEXT("ref_codes"), CodesArray)
     && !Json->TryGetArrayField(TEXT("RefCodes"),  CodesArray))
    {
        OutError = TEXT("Missing required 'ref_codes' (or 'RefCodes') array.");
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
        OutError = TEXT("'ref_codes' / 'RefCodes' array was empty or all-non-numeric.");
        return false;
    }

    // Defaults: name from filename stem; language to en-us. Mirrors the
    // deprecated voice-registry behavior.
    if (ParsedName.IsEmpty())
    {
        ParsedName = FPaths::GetBaseFilename(AbsoluteSourcePath);
    }
    if (ParsedLanguage.IsEmpty())
    {
        ParsedLanguage = TEXT("en-us");
    }

    // Commit last so a parse failure leaves the asset's previous
    // contents intact (matters for Reimport — bad reimport must NOT
    // trash a previously-working asset).
    Asset->Name      = MoveTemp(ParsedName);
    Asset->Language  = MoveTemp(ParsedLanguage);
    Asset->RefText   = MoveTemp(ParsedRefText);
    Asset->RefPhones = MoveTemp(ParsedRefPhones);
    Asset->RefCodes  = MoveTemp(ParsedRefCodes);

    if (Asset->AssetImportData)
    {
        Asset->AssetImportData->Update(AbsoluteSourcePath);
    }
    return true;
}
