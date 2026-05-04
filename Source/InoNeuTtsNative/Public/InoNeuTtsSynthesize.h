// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "InoNeuTtsTypes.h"

#include "InoNeuTtsSynthesize.generated.h"

/**
 * Latent Blueprint node "NeuTTS Synthesize" — wraps
 * UInoNeuTtsSubsystem::SynthesizeAsync as an exec-pin async action with
 * OnComplete and OnError pins. Caller-side Blueprint usage:
 *
 *     [Get NeuTTS Subsystem]
 *           │
 *           ▼
 *     [NeuTTS Synthesize]──OnComplete──►(use Result.AudioSamples)
 *           ▲              OnError───►(handle Result.ErrorMessage)
 *           │
 *     (Text, Voice, Options inputs)
 */
UCLASS()
class INONEUTTSNATIVE_API UInoNeuTtsSynthesize : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Fires once on success. Result.bSuccess will be true. */
	UPROPERTY(BlueprintAssignable)
	FOnInoNeuTtsSynthesisComplete OnComplete;

	/** Fires once on any failure (load-state, missing voice, cancel, etc).
	 *  Result.bSuccess will be false; Result.ErrorMessage is populated. */
	UPROPERTY(BlueprintAssignable)
	FOnInoNeuTtsSynthesisComplete OnError;

	/**
	 * Static Blueprint factory. The named "BlueprintInternalUseOnly"
	 * meta with WorldContextObject hidden gives us a clean "NeuTTS
	 * Synthesize" node with implicit world wiring.
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly,
		Category = "InoNeuTts",
		meta = (DisplayName = "NeuTTS Synthesize",
		        BlueprintInternalUseOnly = "true",
		        WorldContext = "WorldContextObject"))
	static UInoNeuTtsSynthesize* SynthesizeAsync(
		UObject* WorldContextObject,
		const FString& Text,
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleComplete(const FInoNeuTtsResult& Result);

	UPROPERTY()
	TWeakObjectPtr<UObject> WorldContextObjectPtr;

	UPROPERTY()
	FString StoredText;

	UPROPERTY()
	FInoNeuTtsVoice StoredVoice;

	UPROPERTY()
	FInoNeuTtsOptions StoredOptions;
};
