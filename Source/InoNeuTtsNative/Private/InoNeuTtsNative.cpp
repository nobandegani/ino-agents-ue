// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNative.h"
#include "InoNeuTtsLog.h"

DEFINE_LOG_CATEGORY(LogInoNeuTts);

#define LOCTEXT_NAMESPACE "FInoNeuTtsNativeModule"

void FInoNeuTtsNativeModule::StartupModule()
{
	// Sibling plugins (InoLlama, InoOnnx, InoSpeakNG) pre-load their native
	// libraries at LoadingPhase=PreLoadingScreen, so by the time this Default-
	// phase StartupModule runs they are all ready to call. We don't load the
	// NeuTTS GGUF or NeuCodec ONNX here — that's explicit via
	// UInoNeuTtsSubsystem::LoadModelAsync once a game instance exists.
	UE_LOG(LogInoNeuTts, Log, TEXT("InoNeuTtsNative module started."));
}

void FInoNeuTtsNativeModule::ShutdownModule()
{
	UE_LOG(LogInoNeuTts, Log, TEXT("InoNeuTtsNative module shutting down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FInoNeuTtsNativeModule, InoNeuTtsNative)
