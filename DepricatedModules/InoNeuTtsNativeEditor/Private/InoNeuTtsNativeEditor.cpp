// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNativeEditor.h"

DEFINE_LOG_CATEGORY(LogInoNeuTtsEditor);

void FInoNeuTtsNativeEditorModule::StartupModule()
{
	UE_LOG(LogInoNeuTtsEditor, Log, TEXT("InoNeuTtsNativeEditor: StartupModule"));
	// UInoNeuTtsVoiceFactory's UCLASS auto-registers with the asset tools
	// at module load via UE's UFactory class iteration; nothing to do
	// here unless we want to surface an asset category, custom thumbnail,
	// or a New Asset action — none of which we need for v1.
}

void FInoNeuTtsNativeEditorModule::ShutdownModule()
{
	UE_LOG(LogInoNeuTtsEditor, Log, TEXT("InoNeuTtsNativeEditor: ShutdownModule"));
}

IMPLEMENT_MODULE(FInoNeuTtsNativeEditorModule, InoNeuTtsNativeEditor);
