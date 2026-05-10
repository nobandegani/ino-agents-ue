// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

INONEUTTSNATIVEEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogInoNeuTtsEditor, Log, All);

/**
 * Editor-side support module for InoNeuTtsNative. Currently just hosts
 * UInoNeuTtsVoiceFactory which makes `.inv` files first-class importable
 * UAssets. Loaded at PostEngineInit (after the asset-tools / asset-
 * registry modules are up so any future registration hooks are safe).
 */
class FInoNeuTtsNativeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
