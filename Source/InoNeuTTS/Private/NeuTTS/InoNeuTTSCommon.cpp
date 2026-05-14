// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSCommon.h"

#include "InoNeuTTSSettings.h"
#include "NeuTTS/InoNeuTTSTypes.h"

#include "litert/c/litert_common.h"  // kLiteRtHwAccelerator* enum values

namespace InoNeuTTSNative
{

FString ResolveBackbonePath(const FString& NameOrFileName)
{
    const UInoNeuTTSSettings* Settings = UInoNeuTTSSettings::Get();
    if (!Settings) return FString();
    const FInoNeuTTSBackboneEntry* Entry = Settings->FindBackbone(NameOrFileName);
    if (!Entry) return FString();
    return UInoNeuTTSSettings::ResolveLocalPath(Entry->LocalFileName);
}

FString ResolveDecoderPath(const FString& NameOrFileName)
{
    const UInoNeuTTSSettings* Settings = UInoNeuTTSSettings::Get();
    if (!Settings) return FString();
    const FInoNeuTTSDecoderEntry* Entry = Settings->FindDecoder(NameOrFileName);
    if (!Entry) return FString();
    return UInoNeuTTSSettings::ResolveLocalPath(Entry->LocalFileName);
}

FString NormalizePhones(const FString& Phones)
{
    FString Out;
    Out.Reserve(Phones.Len());
    bool bLastWasSpace = true;  // start true → leading whitespace is trimmed
    for (TCHAR C : Phones)
    {
        if (FChar::IsWhitespace(C))
        {
            if (!bLastWasSpace)
            {
                Out += TEXT(' ');
                bLastWasSpace = true;
            }
        }
        else
        {
            Out += C;
            bLastWasSpace = false;
        }
    }
    // Trim a trailing space if the loop's final char was whitespace.
    if (Out.Len() > 0 && Out[Out.Len() - 1] == TEXT(' '))
    {
        Out.RemoveAt(Out.Len() - 1);
    }
    return Out;
}

const char* BackendToLiteRtLmString(EInoNeuTTSBackend Backend)
{
    switch (Backend)
    {
        case EInoNeuTTSBackend::Gpu: return "gpu";
        case EInoNeuTTSBackend::Npu: return "npu";
        case EInoNeuTTSBackend::Cpu:
        default:                     return "cpu";
    }
}

int32 BackendToLiteRtAcceleratorBit(EInoNeuTTSBackend Backend)
{
    switch (Backend)
    {
        case EInoNeuTTSBackend::Gpu: return kLiteRtHwAcceleratorGpu;
        case EInoNeuTTSBackend::Npu: return kLiteRtHwAcceleratorNpu;
        case EInoNeuTTSBackend::Cpu:
        default:                     return kLiteRtHwAcceleratorCpu;
    }
}

int32 ActivationTypeToInt(EInoNeuTTSActivationType ActivationType)
{
    // Per LiteRT-LM's executor_settings_base.h ActivationDataType enum:
    //   F32 = 0, F16 = 1, I16 = 2, I8 = 3
    switch (ActivationType)
    {
        case EInoNeuTTSActivationType::F16: return 1;
        case EInoNeuTTSActivationType::I16: return 2;
        case EInoNeuTTSActivationType::I8:  return 3;
        case EInoNeuTTSActivationType::F32:
        default:                            return 0;
    }
}

} // namespace InoNeuTTSNative
