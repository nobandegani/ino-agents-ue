// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "NeuTtsNano/InoNeuTtsNanoSubsystem.h"

#include "InoAgentsLog.h"
#include "InoAgentsSettings.h"

// Private NeuTtsNano implementation headers. Fully visible here so the
// forward-declared TUniquePtr<> members' deleter can instantiate
// correctly (see "out-of-line special members" block below).
#include "InoNeuTtsNanoRunner.h"
#include "InoNeuTtsNanoSynthesisWorker.h"
#include "InoNeuTtsNanoVoiceRegistry.h"

#include "Async/Async.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ============================================================================
// Out-of-line special members
//
// The subsystem owns TUniquePtr<FInoNeuTtsNanoRunner>,
// TUniquePtr<FInoNeuTtsNanoSynthesisWorker>, and
// TUniquePtr<FInoNeuTtsNanoVoiceRegistry>, all of which are
// forward-declared in the public header. UHT's generated .gen.cpp would
// otherwise emit the implicit default ctor + FVTableHelper ctor + dtor
// inline and fail to compile (C4150 "cannot delete pointer to
// incomplete type") because it doesn't include the private
// NeuTtsNano/ headers. Defining them here, where the full types are
// visible, resolves the TDefaultDelete instantiation cleanly.
// Same trick UInoChatterboxTtsSubsystem + UInoLiteRtLmConversation use.
// ============================================================================

UInoNeuTtsNanoSubsystem::UInoNeuTtsNanoSubsystem() = default;

UInoNeuTtsNanoSubsystem::UInoNeuTtsNanoSubsystem(FVTableHelper& Helper)
    : Super(Helper)
{
}

UInoNeuTtsNanoSubsystem::~UInoNeuTtsNanoSubsystem() = default;

// ============================================================================
// Internal helpers (anonymous namespace — .cpp-local)
// ============================================================================

namespace
{
    /**
     * Build the two-file download queue for a NeuTTS Nano variant.
     *
     *   1. Backbone GGUF — {BackboneRepo}/resolve/{BackboneRev}/{BackboneFile}
     *   2. Codec ONNX    — {CodecRepo}/resolve/{CodecRev}/{CodecFile}
     *
     * Both required (bRequired=true). NeuTTS has no optional files like
     * Chatterbox's .onnx_data companions, so the queue is simpler.
     */
    TArray<FInoNeuTtsNanoDownloadFile> BuildDownloadQueue(
        const FInoNeuTtsNanoModelEntry& Entry,
        const FString&                  TargetDir)
    {
        using FFile = FInoNeuTtsNanoDownloadFile;

        // Trim trailing slashes on repo URLs so the composed URLs don't
        // end up with doubles (HF tolerates it but looks ugly in logs).
        auto StripTrailing = [](FString In) -> FString
        {
            while (In.EndsWith(TEXT("/"))) { In.LeftChopInline(1); }
            return In;
        };

        const FString BackboneRepo = StripTrailing(Entry.BackboneHuggingFaceRepoUrl);
        const FString CodecRepo    = StripTrailing(Entry.CodecHuggingFaceRepoUrl);
        const FString BackboneRev  = Entry.BackboneRevision.IsEmpty() ? TEXT("main") : Entry.BackboneRevision;
        const FString CodecRev     = Entry.CodecRevision.IsEmpty()    ? TEXT("main") : Entry.CodecRevision;

        TArray<FFile> Queue;

        Queue.Add(FFile{
            /*Url*/        FString::Printf(TEXT("%s/resolve/%s/%s"),
                               *BackboneRepo, *BackboneRev, *Entry.BackboneFileName),
            /*TargetPath*/ FPaths::Combine(TargetDir, Entry.BackboneFileName),
            /*bRequired*/  true,
        });

        Queue.Add(FFile{
            /*Url*/        FString::Printf(TEXT("%s/resolve/%s/%s"),
                               *CodecRepo, *CodecRev, *Entry.CodecFileName),
            /*TargetPath*/ FPaths::Combine(TargetDir, Entry.CodecFileName),
            /*bRequired*/  true,
        });

        return Queue;
    }
} // namespace

// ============================================================================
// Subsystem lifecycle
// ============================================================================

void UInoNeuTtsNanoSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init state — no model loaded, no download in flight.
    bModelLoaded  = false;
    bLoadInFlight = false;
    LoadedVariant = EInoNeuTtsNanoBackboneVariant::Q4;

    // Create the voice registry and try to load the plugin's baked-in
    // default voice. The committed JSON ships as a placeholder (empty
    // ref_codes) until someone regenerates it via the Python encoder;
    // we still register it so GetAvailableVoiceNames returns "Default"
    // and the Milestone 4 synthesis worker can emit a clear
    // "regenerate voice" error rather than a silent failure.
    VoiceRegistry = MakeUnique<FInoNeuTtsNanoVoiceRegistry>();

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (Plugin.IsValid())
    {
        const FString VoicePath = FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("NeuTtsNano/Resources/default_voice.nvoice.json"));

        FString VoiceErr;
        if (VoiceRegistry->RegisterFromJsonFile(VoicePath, FName(TEXT("Default")), VoiceErr))
        {
            const FInoNeuTtsNanoVoice* Default = VoiceRegistry->Find(FName(TEXT("Default")));
            if (Default != nullptr && Default->IsPlaceholder())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("NeuTTS Nano: default voice is a PLACEHOLDER (empty ref_codes). "
                            "Synthesis will fail until regenerated. See "
                            "Plugins/InoAgents/NeuTtsNano/README.md."));
            }
            else if (Default != nullptr)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("NeuTTS Nano: loaded default voice \"%s\" "
                            "(%d ref codes, %d-char ref_text, %d-char ref_phones)."),
                       *Default->DisplayName,
                       Default->RefCodes.Num(),
                       Default->RefText.Len(),
                       Default->RefPhones.Len());
            }
        }
        else
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("NeuTTS Nano: failed to load default voice JSON: %s"), *VoiceErr);
        }
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTTS Nano: IPluginManager::FindPlugin(\"InoAgents\") returned "
                    "invalid — default voice not loaded."));
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoNeuTtsNanoSubsystem::Initialize — ready (no model loaded, "
                "%d voice(s) registered)"),
           VoiceRegistry->Num());
}

void UInoNeuTtsNanoSubsystem::Deinitialize()
{
    // Cancel any in-flight download so the HTTP completion callback
    // doesn't try to write to a file that's about to be gone.
    if (DownloadRequest.IsValid())
    {
        DownloadRequest->CancelRequest();
        DownloadRequest.Reset();
    }

    UnloadModel();

    // Voice registry doesn't hold any native resources; free after
    // the worker + runner so any Milestone 4 synthesis-in-flight that
    // still holds a VoiceRegistry pointer is already joined.
    VoiceRegistry.Reset();

    Super::Deinitialize();
}

// ============================================================================
// Public API
// ============================================================================

void UInoNeuTtsNanoSubsystem::LoadModelAsync(
    const FInoNeuTtsNanoModelConfig& Config,
    const FOnInoNeuTtsNanoModelLoaded& OnLoaded)
{
    check(IsInGameThread());

    // Guard: only one load at a time.
    if (bLoadInFlight)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTTS Nano LoadModelAsync ignored — another load is in flight."));
        OnLoaded.ExecuteIfBound(false, TEXT("Another load is already in flight."));
        return;
    }

    // Fast path: the same variant is already loaded. No-op + success.
    if (bModelLoaded && LoadedVariant == Config.Variant)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTTS Nano LoadModelAsync — variant %s already loaded."),
               *NeuTtsNanoVariantToString(Config.Variant));
        OnLoaded.ExecuteIfBound(true, FString());
        return;
    }

    // Different variant requested while one is loaded — switch: unload
    // first, then fall through to the load path.
    if (bModelLoaded)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTTS Nano LoadModelAsync — switching variants, unloading %s first."),
               *NeuTtsNanoVariantToString(LoadedVariant));
        UnloadModel();
    }

    // Look up the settings entry for the requested variant.
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    if (Settings == nullptr)
    {
        OnLoaded.ExecuteIfBound(false, TEXT("UInoAgentsSettings unavailable."));
        return;
    }
    const FInoNeuTtsNanoModelEntry* Entry = Settings->FindNeuTtsNanoModel(Config.Variant);
    if (Entry == nullptr)
    {
        OnLoaded.ExecuteIfBound(false, FString::Printf(
            TEXT("No UInoAgentsSettings entry for NeuTTS Nano variant '%s'. "
                 "Add one under Edit → Project Settings → Plugins → InoAgents → NeuTTS Nano."),
            *NeuTtsNanoVariantToString(Config.Variant)));
        return;
    }

    bLoadInFlight   = true;
    LoadedVariant   = Config.Variant;
    PendingConfig   = Config;
    PendingOnLoaded = OnLoaded;

    // Fast path: both files already on disk → skip download, dispatch
    // the loader immediately.
    if (IsModelDownloaded(Config.Variant))
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTTS Nano LoadModelAsync — variant %s already on disk, skipping download."),
               *NeuTtsNanoVariantToString(Config.Variant));
        DispatchLoadWorker();
        return;
    }

    // Slow path: start the multi-file download.
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano LoadModelAsync — variant %s not on disk, starting download."),
           *NeuTtsNanoVariantToString(Config.Variant));
    StartDownload();
}

void UInoNeuTtsNanoSubsystem::UnloadModel()
{
    check(IsInGameThread());

    // 1. If a download is in flight, finish it with a cancellation
    //    error first. This fires PendingOnLoaded with bSuccess=false
    //    and clears bLoadInFlight.
    if (bLoadInFlight && DownloadQueue.Num() > 0)
    {
        FinishDownloadError(TEXT("Cancelled by UnloadModel."));
    }

    // 2. Stop + join worker thread BEFORE freeing Runner, since the
    //    worker holds borrowed pointers into Runner's native resources.
    //    ~FInoNeuTtsNanoSynthesisWorker signals stop + WaitForCompletion
    //    (see InoNeuTtsNanoSynthesisWorker.cpp dtor).
    if (Worker.IsValid())
    {
        Worker->SignalCancel();  // abandon any in-flight synthesis
        Worker.Reset();           // joins the thread
    }

    // 3. Free Runner — dtor releases codec session, llama_context,
    //    and llama_model in reverse-construction order.
    if (Runner.IsValid())
    {
        Runner.Reset();
    }

    bModelLoaded = false;
}

TArray<FName> UInoNeuTtsNanoSubsystem::GetAvailableVoiceNames() const
{
    if (!VoiceRegistry.IsValid())
    {
        return TArray<FName>();
    }
    return VoiceRegistry->GetAvailableVoiceNames();
}

void UInoNeuTtsNanoSubsystem::SynthesizeAsync(
    const FString& PhonemesText,
    FName VoiceName,
    const FInoNeuTtsNanoSynthesisOptions& Options,
    const FOnInoNeuTtsNanoSynthesisComplete& OnComplete)
{
    check(IsInGameThread());

    // Validate state before enqueuing. Everything below returns
    // OnComplete(false, "...") synchronously to keep the "exactly one
    // OnComplete fire per call" invariant intact.
    auto FailImmediately = [&](const TCHAR* Reason)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTtsNano SynthesizeAsync early-failed: %s"), Reason);
        OnComplete.ExecuteIfBound(false, TArray<uint8>(), 24000, FString(Reason));
    };

    if (!bModelLoaded)
    {
        FailImmediately(TEXT("Model not loaded. Call LoadModelAsync first "
                             "and wait for its OnLoaded(true) callback."));
        return;
    }
    if (!Worker.IsValid() || !Runner.IsValid() || !VoiceRegistry.IsValid())
    {
        FailImmediately(TEXT("Internal state incomplete (worker/runner/voice-registry missing)."));
        return;
    }
    if (PhonemesText.IsEmpty())
    {
        FailImmediately(TEXT("PhonemesText is empty. v1 requires pre-phonemized IPA input."));
        return;
    }

    // Package + enqueue — worker dequeues on its dedicated thread and
    // fires OnComplete asynchronously via AsyncTask(GameThread).
    FInoNeuTtsNanoPendingSynth Pending;
    Pending.PhonemesText = PhonemesText;
    Pending.VoiceName    = VoiceName.IsNone() ? FName(TEXT("Default")) : VoiceName;
    Pending.Options      = Options;
    Pending.OnComplete   = OnComplete;

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano SynthesizeAsync: queued (voice=%s, phonemes=%d chars, "
                "max_new=%d, top_k=%d, temp=%.2f, seed=%d)"),
           *Pending.VoiceName.ToString(),
           Pending.PhonemesText.Len(),
           Pending.Options.MaxNewTokens,
           Pending.Options.TopK,
           Pending.Options.Temperature,
           Pending.Options.Seed);

    Worker->Enqueue(MoveTemp(Pending));
}

void UInoNeuTtsNanoSubsystem::CancelSynthesis()
{
    check(IsInGameThread());
    if (Worker.IsValid())
    {
        Worker->SignalCancel();
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano CancelSynthesis: signalled worker."));
    }
}

bool UInoNeuTtsNanoSubsystem::IsModelDownloaded(EInoNeuTtsNanoBackboneVariant Variant) const
{
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    if (Settings == nullptr)
    {
        return false;
    }
    const FInoNeuTtsNanoModelEntry* Entry = Settings->FindNeuTtsNanoModel(Variant);
    if (Entry == nullptr)
    {
        return false;
    }

    const FString Dir = NeuTtsNanoResolveModelDir(Variant);
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

    const FString BackbonePath = FPaths::Combine(Dir, Entry->BackboneFileName);
    const FString CodecPath    = FPaths::Combine(Dir, Entry->CodecFileName);

    // Also require non-zero file size — catches a .partial that got
    // renamed incorrectly, or a prior aborted download that left a
    // zero-byte stub.
    const bool bBackboneOk =
        PF.FileExists(*BackbonePath) && PF.FileSize(*BackbonePath) > 0;
    const bool bCodecOk =
        PF.FileExists(*CodecPath) && PF.FileSize(*CodecPath) > 0;

    return bBackboneOk && bCodecOk;
}

void UInoNeuTtsNanoSubsystem::CancelDownload()
{
    check(IsInGameThread());
    if (DownloadQueue.Num() == 0)
    {
        return;
    }
    if (DownloadRequest.IsValid())
    {
        DownloadRequest->CancelRequest();
        DownloadRequest.Reset();
    }
    FinishDownloadError(TEXT("Cancelled by caller."));
}

// ============================================================================
// Download flow
//
// This block ports the Chatterbox multi-file download pattern
// (Plugins/InoAgents/Source/InoAgents/Private/Chatterbox/
//  InoChatterboxTtsSubsystem.cpp, StartDownload → StartHeadProbe →
//  StartNextFileDownload → HandleDownloadComplete chain) minus the
// optional-file / 404-tolerance branch that NeuTTS doesn't need.
//
// Duplication is tech debt; a future refactor will factor both into a
// shared Private/InoHttpDownload/ helper.
// ============================================================================

void UInoNeuTtsNanoSubsystem::StartDownload()
{
    check(IsInGameThread());

    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    const FInoNeuTtsNanoModelEntry* Entry =
        Settings ? Settings->FindNeuTtsNanoModel(PendingConfig.Variant) : nullptr;
    if (Entry == nullptr)
    {
        FinishDownloadError(FString::Printf(
            TEXT("NeuTTS Nano download: no settings entry for variant %s."),
            *NeuTtsNanoVariantToString(PendingConfig.Variant)));
        return;
    }

    const FString TargetDir = NeuTtsNanoResolveModelDir(PendingConfig.Variant);
    IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/ true);

    DownloadQueue = BuildDownloadQueue(*Entry, TargetDir);
    if (DownloadQueue.Num() == 0)
    {
        FinishDownloadError(TEXT("NeuTTS Nano download: empty queue."));
        return;
    }

    DownloadCursor   = 0;
    bDownloadProbing = true;

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano StartDownload: variant=%s, %d files "
                "(backbone=%s, codec=%s)"),
           *NeuTtsNanoVariantToString(PendingConfig.Variant),
           DownloadQueue.Num(),
           *Entry->BackboneHuggingFaceRepoUrl,
           *Entry->CodecHuggingFaceRepoUrl);

    StartHeadProbe();
}

void UInoNeuTtsNanoSubsystem::StartHeadProbe()
{
    check(IsInGameThread());

    // End of HEAD phase → start GET phase.
    if (DownloadCursor >= DownloadQueue.Num())
    {
        int32 KnownSizes = 0;
        int64 KnownTotal = 0;
        for (const FInoNeuTtsNanoDownloadFile& F : DownloadQueue)
        {
            if (F.ExpectedBytes > 0)
            {
                ++KnownSizes;
                KnownTotal += F.ExpectedBytes;
            }
        }
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTTS Nano HEAD phase complete — %d/%d files reported size "
                    "(sum of known=%.1f MB). %s"),
               KnownSizes, DownloadQueue.Num(),
               (double)KnownTotal / (1024.0 * 1024.0),
               KnownSizes == DownloadQueue.Num()
                   ? TEXT("Aggregate byte-weighted percent will be exact.")
                   : TEXT("Some sizes unknown; percent falls back to file-count."));

        bDownloadProbing = false;
        DownloadCursor   = 0;
        StartNextFileDownload();
        return;
    }

    const FInoNeuTtsNanoDownloadFile& File = DownloadQueue[DownloadCursor];

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(File.Url);
    DownloadRequest->SetVerb(TEXT("HEAD"));
    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoNeuTtsNanoSubsystem::HandleHeadComplete);

    UE_LOG(LogInoAgents, Verbose,
           TEXT("NeuTTS Nano HEAD %d/%d: %s"),
           DownloadCursor + 1, DownloadQueue.Num(), *File.Url);

    DownloadRequest->ProcessRequest();
}

void UInoNeuTtsNanoSubsystem::HandleHeadComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    check(IsInGameThread());
    DownloadRequest.Reset();

    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        // Teardown while probe was in flight — bail silently; the
        // cancel path has already fired PendingOnLoaded.
        return;
    }

    FInoNeuTtsNanoDownloadFile& File = DownloadQueue[DownloadCursor];
    const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;

    if (bSucceeded && Response.IsValid() && Code >= 200 && Code < 400)
    {
        const FString Len = Response->GetHeader(TEXT("Content-Length"));
        if (!Len.IsEmpty())
        {
            const int64 Parsed = FCString::Atoi64(*Len);
            if (Parsed > 0)
            {
                File.ExpectedBytes = Parsed;
            }
        }
        UE_LOG(LogInoAgents, Verbose,
               TEXT("NeuTTS Nano HEAD %d/%d: ok, ExpectedBytes=%lld"),
               DownloadCursor + 1, DownloadQueue.Num(), File.ExpectedBytes);
    }
    else if (bSucceeded && Code == 404)
    {
        // All NeuTTS files are required — a 404 here is fatal.
        FinishDownloadError(FString::Printf(
            TEXT("NeuTTS Nano download: required file 404 on HEAD: %s"),
            *File.Url));
        return;
    }
    else
    {
        // HEAD flaked (network error, 5xx, CDN stripped Content-Length,
        // etc.). Non-fatal — fall through with ExpectedBytes unknown;
        // the GET will either succeed (and we report bytes-only
        // progress) or fail conclusively.
        UE_LOG(LogInoAgents, Verbose,
               TEXT("NeuTTS Nano HEAD %d/%d: non-fatal probe failure (code=%d); "
                    "continuing with unknown total size"),
               DownloadCursor + 1, DownloadQueue.Num(), Code);
    }

    ++DownloadCursor;
    StartHeadProbe();
}

void UInoNeuTtsNanoSubsystem::StartNextFileDownload()
{
    check(IsInGameThread());

    if (DownloadCursor >= DownloadQueue.Num())
    {
        FinishDownloadSuccess();
        return;
    }

    FInoNeuTtsNanoDownloadFile& File = DownloadQueue[DownloadCursor];
    File.BytesWritten = 0;

    // Open .partial for writing. OpenWrite(..., bAppend=false) truncates
    // any leftover from a prior aborted run — correct behaviour.
    const FString PartialPath = File.TargetPath + TEXT(".partial");
    if (DownloadFileHandle != nullptr)
    {
        delete DownloadFileHandle;
        DownloadFileHandle = nullptr;
    }
    DownloadFileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*PartialPath);
    if (DownloadFileHandle == nullptr)
    {
        FinishDownloadError(FString::Printf(
            TEXT("NeuTTS Nano download: failed to open %s for writing"),
            *PartialPath));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano GET %d/%d: %s (%lld bytes expected)"),
           DownloadCursor + 1, DownloadQueue.Num(),
           *File.Url, File.ExpectedBytes);

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(File.Url);
    DownloadRequest->SetVerb(TEXT("GET"));
    DownloadRequest->SetHeader(TEXT("Accept"), TEXT("*/*"));

    DownloadRequest->OnHeaderReceived().BindUObject(
        this, &UInoNeuTtsNanoSubsystem::HandleDownloadHeader);
    DownloadRequest->OnRequestProgress64().BindUObject(
        this, &UInoNeuTtsNanoSubsystem::HandleDownloadProgress);
    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoNeuTtsNanoSubsystem::HandleDownloadComplete);

    DownloadRequest->ProcessRequest();
}

void UInoNeuTtsNanoSubsystem::HandleDownloadProgress(
    FHttpRequestPtr /*Request*/, uint64 /*BytesSent*/, uint64 BytesReceived)
{
    check(IsInGameThread());
    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;
    }

    DownloadQueue[DownloadCursor].BytesWritten =
        (int64)FMath::Min<uint64>(BytesReceived, (uint64)INT64_MAX);
    BroadcastDownloadProgress();
}

void UInoNeuTtsNanoSubsystem::HandleDownloadHeader(
    FHttpRequestPtr /*Request*/,
    const FString& HeaderName,
    const FString& HeaderValue)
{
    check(IsInGameThread());
    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;
    }

    if (!HeaderName.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase))
    {
        return;
    }

    const int64 Parsed = FCString::Atoi64(*HeaderValue);
    if (Parsed <= 0)
    {
        return;
    }

    FInoNeuTtsNanoDownloadFile& File = DownloadQueue[DownloadCursor];
    if (File.ExpectedBytes > 0 && File.ExpectedBytes == Parsed)
    {
        return;   // HEAD already told us; nothing new.
    }
    if (File.ExpectedBytes <= 0)
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("NeuTTS Nano GET %d/%d: learned Content-Length=%lld from GET response "
                    "(HEAD didn't give us one)"),
               DownloadCursor + 1, DownloadQueue.Num(), Parsed);
    }
    File.ExpectedBytes = Parsed;
    BroadcastDownloadProgress();
}

void UInoNeuTtsNanoSubsystem::HandleDownloadComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    check(IsInGameThread());
    DownloadRequest.Reset();

    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;   // teardown during request
    }

    FInoNeuTtsNanoDownloadFile& File = DownloadQueue[DownloadCursor];
    const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;

    // Hard errors — both files required, any non-200 is fatal.
    if (!bSucceeded || !Response.IsValid() || Code != 200)
    {
        FinishDownloadError(FString::Printf(
            TEXT("NeuTTS Nano GET %d/%d failed: %s (HTTP %d)"),
            DownloadCursor + 1, DownloadQueue.Num(), *File.Url, Code));
        return;
    }

    // Success: write + atomic rename.
    const TArray<uint8>& Content = Response->GetContent();
    if (DownloadFileHandle == nullptr)
    {
        FinishDownloadError(TEXT("NeuTTS Nano GET: file handle closed before write."));
        return;
    }
    if (Content.Num() > 0)
    {
        DownloadFileHandle->Write(Content.GetData(), Content.Num());
    }
    delete DownloadFileHandle;
    DownloadFileHandle = nullptr;

    const FString PartialPath = File.TargetPath + TEXT(".partial");
    if (!IFileManager::Get().Move(
            *File.TargetPath, *PartialPath, /*Replace=*/ true))
    {
        IFileManager::Get().Delete(*PartialPath);
        FinishDownloadError(FString::Printf(
            TEXT("NeuTTS Nano GET: failed to rename %s → %s"),
            *PartialPath, *File.TargetPath));
        return;
    }

    File.BytesWritten = Content.Num();
    File.bDone        = true;
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano GET %d/%d: OK, %lld bytes → %s"),
           DownloadCursor + 1, DownloadQueue.Num(),
           File.BytesWritten, *File.TargetPath);

    BroadcastDownloadProgress();

    ++DownloadCursor;
    StartNextFileDownload();
}

void UInoNeuTtsNanoSubsystem::BroadcastDownloadProgress()
{
    // Same two-strategy approach Chatterbox uses:
    //   1. Byte-weighted (preferred) when every size is known.
    //   2. File-count with current-file fractional credit otherwise.
    // AggregateReceived (real bytes on disk) always accurate.
    // TotalBytes reported only when strategy 1 applies; -1 otherwise.

    const int32 FileCount = DownloadQueue.Num();
    if (FileCount == 0)
    {
        OnDownloadProgress.Broadcast(0.0f, 0, -1);
        return;
    }

    int64 AggregateReceived   = 0;
    int64 AggregateTotalKnown = 0;
    int32 DoneFiles           = 0;
    bool  bAllSizesKnown      = true;

    for (int32 i = 0; i < FileCount; ++i)
    {
        const FInoNeuTtsNanoDownloadFile& F = DownloadQueue[i];
        if (F.bDone)
        {
            ++DoneFiles;
            AggregateReceived   += F.BytesWritten;
            AggregateTotalKnown += (F.ExpectedBytes > 0 ? F.ExpectedBytes : F.BytesWritten);
            continue;
        }
        if (i == DownloadCursor)
        {
            AggregateReceived += F.BytesWritten;
        }
        if (F.ExpectedBytes > 0)
        {
            AggregateTotalKnown += F.ExpectedBytes;
        }
        else
        {
            bAllSizesKnown = false;
        }
    }

    float Percent = 0.0f;
    int64 TotalBytes = -1;

    if (bAllSizesKnown && AggregateTotalKnown > 0)
    {
        Percent = FMath::Clamp(
            (float)((double)AggregateReceived * 100.0 / (double)AggregateTotalKnown),
            0.0f, 100.0f);
        TotalBytes = AggregateTotalKnown;
    }
    else
    {
        // File-count fallback. Current file's fractional credit =
        // BytesWritten / ExpectedBytes (if known) or 0.5 (unknown).
        float CurrentFraction = 0.0f;
        if (DownloadCursor >= 0 && DownloadCursor < FileCount
            && !DownloadQueue[DownloadCursor].bDone)
        {
            const FInoNeuTtsNanoDownloadFile& Cur = DownloadQueue[DownloadCursor];
            if (Cur.ExpectedBytes > 0)
            {
                CurrentFraction = FMath::Clamp(
                    (float)((double)Cur.BytesWritten / (double)Cur.ExpectedBytes),
                    0.0f, 1.0f);
            }
            else if (Cur.BytesWritten > 0)
            {
                CurrentFraction = 0.5f;   // we're mid-transfer but don't know the total
            }
        }
        Percent = FMath::Clamp(
            100.0f * ((float)DoneFiles + CurrentFraction) / (float)FileCount,
            0.0f, 100.0f);
    }

    OnDownloadProgress.Broadcast(Percent, AggregateReceived, TotalBytes);
}

void UInoNeuTtsNanoSubsystem::FinishDownloadSuccess()
{
    check(IsInGameThread());
    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano download complete — all %d files staged."),
           DownloadQueue.Num());
    CleanupDownload();
    DispatchLoadWorker();
}

void UInoNeuTtsNanoSubsystem::FinishDownloadError(const FString& Err)
{
    check(IsInGameThread());
    UE_LOG(LogInoAgents, Error, TEXT("NeuTTS Nano download FAILED: %s"), *Err);

    // Read PendingOnLoaded into a local and clear it BEFORE firing so
    // a handler that re-calls LoadModelAsync doesn't double-fire.
    FOnInoNeuTtsNanoModelLoaded Cb = PendingOnLoaded;
    PendingOnLoaded.Unbind();
    bLoadInFlight = false;

    CleanupDownload();

    Cb.ExecuteIfBound(false, Err);
}

void UInoNeuTtsNanoSubsystem::CleanupDownload()
{
    if (DownloadFileHandle != nullptr)
    {
        delete DownloadFileHandle;
        DownloadFileHandle = nullptr;

        // Delete any leftover .partial — Chatterbox does this too.
        if (DownloadQueue.IsValidIndex(DownloadCursor))
        {
            IFileManager::Get().Delete(
                *(DownloadQueue[DownloadCursor].TargetPath + TEXT(".partial")));
        }
    }
    DownloadRequest.Reset();
    DownloadQueue.Reset();
    DownloadCursor   = INDEX_NONE;
    bDownloadProbing = false;
}

// ============================================================================
// Model loader (Milestone 3 — real async ThreadPool dispatch)
//
// Dispatches the heavy work (llama_model_load_from_file + llama_init_from_model
// + FInoOnnxSession::Create) to a ThreadPool thread so the game thread
// doesn't hitch during the ~2-5 s load. AsyncTask marshals the result
// back to the game thread where we stash the Runner into a TUniquePtr
// and fire PendingOnLoaded.
//
// TWeakObjectPtr guards against the subsystem being destroyed while the
// load is in flight — if that happens, the AsyncTask lambda sees
// WeakSelf.Get() == nullptr and silently drops the Runner (its dtor
// cleans up the native resources).
// ============================================================================

void UInoNeuTtsNanoSubsystem::DispatchLoadWorker()
{
    check(IsInGameThread());

    // Resolve file paths we'll hand to the ThreadPool task.
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    const FInoNeuTtsNanoModelEntry* Entry =
        Settings ? Settings->FindNeuTtsNanoModel(PendingConfig.Variant) : nullptr;
    if (Entry == nullptr)
    {
        // Shouldn't happen — LoadModelAsync already verified this — but
        // be defensive.
        FString Err = FString::Printf(
            TEXT("Settings entry disappeared during load for variant %s."),
            *NeuTtsNanoVariantToString(PendingConfig.Variant));
        UE_LOG(LogInoAgents, Error, TEXT("NeuTTS Nano load FAILED: %s"), *Err);

        FOnInoNeuTtsNanoModelLoaded Cb = PendingOnLoaded;
        PendingOnLoaded.Unbind();
        bLoadInFlight = false;
        Cb.ExecuteIfBound(false, Err);
        return;
    }

    const FString Dir          = NeuTtsNanoResolveModelDir(PendingConfig.Variant);
    const FString BackbonePath = FPaths::Combine(Dir, Entry->BackboneFileName);
    const FString CodecPath    = FPaths::Combine(Dir, Entry->CodecFileName);
    const FInoNeuTtsNanoModelConfig ConfigCopy = PendingConfig;

    TWeakObjectPtr<UInoNeuTtsNanoSubsystem> WeakSelf(this);

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTTS Nano DispatchLoadWorker: async load (backbone=%s, codec=%s, "
                "n_gpu_layers=%d, n_ctx=%d)"),
           *BackbonePath, *CodecPath,
           ConfigCopy.NumGpuLayers, ConfigCopy.NumContextTokens);

    const double LoadStartTime = FPlatformTime::Seconds();

    Async(EAsyncExecution::ThreadPool,
        [WeakSelf, BackbonePath, CodecPath, ConfigCopy, LoadStartTime]() mutable
        {
            // -- ThreadPool thread --
            FString LocalErr;
            TUniquePtr<FInoNeuTtsNanoRunner> NewRunner =
                FInoNeuTtsNanoRunner::Create(
                    BackbonePath, CodecPath, ConfigCopy, LocalErr);

            const double LoadElapsed = FPlatformTime::Seconds() - LoadStartTime;

            // -- Back to the game thread to stash + dispatch --
            AsyncTask(ENamedThreads::GameThread,
                [WeakSelf, Runner = MoveTemp(NewRunner),
                 LocalErr = MoveTemp(LocalErr), LoadElapsed]() mutable
                {
                    UInoNeuTtsNanoSubsystem* Self = WeakSelf.Get();
                    if (Self == nullptr)
                    {
                        // Subsystem torn down while the load was in
                        // flight. Runner's dtor will clean up cleanly
                        // as the temporary goes out of scope.
                        UE_LOG(LogInoAgents, Warning,
                               TEXT("NeuTTS Nano load completed but subsystem is gone; "
                                    "discarding Runner."));
                        return;
                    }

                    FOnInoNeuTtsNanoModelLoaded Cb = Self->PendingOnLoaded;
                    Self->PendingOnLoaded.Unbind();
                    Self->bLoadInFlight = false;

                    if (!Runner)
                    {
                        UE_LOG(LogInoAgents, Error,
                               TEXT("NeuTTS Nano load FAILED after %.2f s: %s"),
                               LoadElapsed, *LocalErr);
                        Cb.ExecuteIfBound(false, LocalErr);
                        return;
                    }

                    // Happy path — stash Runner, start worker skeleton,
                    // flip loaded flag.
                    Self->Runner       = MoveTemp(Runner);
                    Self->Worker       = MakeUnique<FInoNeuTtsNanoSynthesisWorker>(
                        WeakSelf,
                        Self->Runner.Get(),
                        Self->VoiceRegistry.Get());
                    Self->bModelLoaded = true;

                    UE_LOG(LogInoAgents, Log,
                           TEXT("NeuTTS Nano load OK in %.2f s — Runner + Worker ready."),
                           LoadElapsed);
                    Cb.ExecuteIfBound(true, FString());
                });
        });
}
