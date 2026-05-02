// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRLiteRTSubsystemTest.h"

#include "Qwen3ASR/InoQwen3ASRLiteRTSubsystem.h"
#include "InoQwen3ASRLiteRT.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

void UInoQwen3ASRSubsystemTestObserver::Activate(UInoQwen3ASRLiteRTSubsystem* Subsystem)
{
    check(Subsystem);
    WeakSubsystem = Subsystem;
    T0Activate = FPlatformTime::Seconds();

    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("SubsystemTest: starting LoadModelAsync..."));

    FInoQwen3ASRConfig Config;  // defaults: stock filenames
    FOnInoQwen3ASRModelLoaded Cb;
    Cb.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(
        UInoQwen3ASRSubsystemTestObserver, HandleLoadComplete));
    Subsystem->LoadModelAsync(Config, Cb);
}

void UInoQwen3ASRSubsystemTestObserver::HandleLoadComplete(bool bSuccess, FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - T0Activate;
    if (!bSuccess)
    {
        Finish(TEXT("Load"), false, FString::Printf(
            TEXT("LoadModelAsync failed after %.2fs — %s"), Elapsed, *ErrorMessage));
        return;
    }
    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("SubsystemTest: LoadModelAsync OK in %.2fs. Now TranscribeWavFileAsync('%s')..."),
        Elapsed, *WavPath);

    UInoQwen3ASRLiteRTSubsystem* Subsystem = WeakSubsystem.Get();
    if (!Subsystem)
    {
        Finish(TEXT("Transcribe"), false, TEXT("Subsystem went away between Load and Transcribe."));
        return;
    }

    FOnInoQwen3ASRTranscribeComplete Cb;
    Cb.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(
        UInoQwen3ASRSubsystemTestObserver, HandleTranscribeComplete));
    T0Activate = FPlatformTime::Seconds();
    Subsystem->TranscribeWavFileAsync(WavPath, Cb);
}

void UInoQwen3ASRSubsystemTestObserver::HandleTranscribeComplete(
    bool bSuccess, FInoQwen3ASRTranscribeResult Result, FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - T0Activate;
    if (!bSuccess)
    {
        Finish(TEXT("Transcribe"), false, FString::Printf(
            TEXT("TranscribeWavFileAsync failed after %.2fs — %s"), Elapsed, *ErrorMessage));
        return;
    }

    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("SubsystemTest: timings — mel=%.3fs encode=%.3fs decode=%.3fs total=%.3fs (%d tokens, lang='%s')"),
        Result.MelSeconds, Result.EncodeSeconds, Result.DecodeSeconds, Result.TotalSeconds,
        Result.NumGeneratedTokens, *Result.DetectedLanguage);
    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("SubsystemTest: text (%d chars) ↓"), Result.Text.Len());
    UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("                ===================="));
    UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("                %s"), *Result.Text);
    UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("                ===================="));

    Finish(TEXT("Transcribe"), true, FString());
}

void UInoQwen3ASRSubsystemTestObserver::Finish(const TCHAR* Phase, bool bOk, const FString& Message)
{
    if (bOk)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("SubsystemTest: PASS at %s phase."), Phase);
    }
    else
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("SubsystemTest: FAIL at %s phase — %s"), Phase, *Message);
    }
    // Drop the GC root we acquired in the console command, then mark for GC.
    RemoveFromRoot();
    ConditionalBeginDestroy();
}

namespace
{
    static FString DefaultWavPath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid()) { return FString(); }
        return FPaths::Combine(Plugin->GetBaseDir(),
            TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("samples"), TEXT("test.wav"));
    }

    /**
     * PIE-required end-to-end test of UInoQwen3ASRLiteRTSubsystem. Resolves
     * the subsystem via the active game instance, runs LoadModelAsync, then
     * TranscribeWavFileAsync, and logs the final result. Both stages happen
     * off the game thread; this command returns immediately and the result
     * arrives later via delegate.
     */
    void RunSubsystemTest(const TArray<FString>& Args)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("=== Ino.Qwen3ASRLiteRT.SubsystemTest ==="));

        // Locate any active game instance (PIE or standalone).
        UGameInstance* GameInstance = nullptr;
        if (GEngine)
        {
            for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
            {
                if (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game)
                {
                    GameInstance = Ctx.OwningGameInstance;
                    if (GameInstance) { break; }
                }
            }
        }
        if (!GameInstance)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("SubsystemTest: no active GameInstance — start PIE first."));
            return;
        }
        UInoQwen3ASRLiteRTSubsystem* Subsystem =
            GameInstance->GetSubsystem<UInoQwen3ASRLiteRTSubsystem>();
        if (!Subsystem)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("SubsystemTest: UInoQwen3ASRLiteRTSubsystem unavailable."));
            return;
        }

        const FString WavPath = (Args.Num() > 0) ? Args[0] : DefaultWavPath();
        if (WavPath.IsEmpty() || !FPaths::FileExists(WavPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("SubsystemTest: WAV missing — %s"), *WavPath);
            return;
        }

        UInoQwen3ASRSubsystemTestObserver* Obs =
            NewObject<UInoQwen3ASRSubsystemTestObserver>();
        Obs->WavPath = WavPath;
        Obs->AddToRoot();   // keeps Obs alive across async hops; cleared in Finish().
        Obs->Activate(Subsystem);
    }

    static FAutoConsoleCommand GSubsystemTestCmd(
        TEXT("Ino.Qwen3ASRLiteRT.SubsystemTest"),
        TEXT("PIE-required end-to-end test of UInoQwen3ASRLiteRTSubsystem. "
             "Loads model + tokenizer asynchronously, then transcribes "
             "samples/test.wav (or a path passed as the first arg)."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunSubsystemTest));
}
