// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.
//
// Self-contained SHA-256 implementation. UE 5.7 ships FSHA1 + FMD5 (in
// Misc/SecureHash.h) and Blake3 (in Hash/Blake3.h) but no first-class
// SHA-256. We only need SHA-256 here to verify downloaded .litertlm model
// files against the ExpectedSha256 stored per-model in
// UInoAgentsSettings::Models — so rather than pull another runtime
// dependency or prototype an OpenSSL bind, we bundle a public-domain
// reference implementation adapted from Brad Conte's
// https://github.com/B-Con/crypto-algorithms (sha256.c).
//
// The hasher processes full 64-byte blocks straight out of the input
// buffer when possible, so bulk throughput (3+ GB model files) is the
// same order as a memcpy loop.

#pragma once

#include "CoreMinimal.h"

namespace InoAgents
{

/** Incremental SHA-256 hasher. Not thread-safe. */
struct FSha256Hasher
{
    FSha256Hasher();

    /** Feed bytes into the hasher. Call any number of times. */
    void Update(const void* InData, SIZE_T Len);

    /** Finalize and write the 32-byte digest into OutHash. */
    void FinalizeRaw(uint8 OutHash[32]);

    /** Finalize and return a lowercase 64-character hex digest.
     *  Destroys hasher state — don't call Update after Finalize*. */
    FString FinalizeHex();

private:
    uint8  Buffer[64];
    uint32 BufferLen;
    uint64 BitLen;
    uint32 State[8];
};

/**
 * Compute the SHA-256 of a file on disk as a lowercase 64-char hex string.
 *
 * Reads in 1 MB chunks, so memory use is bounded regardless of file size.
 * Returns an empty string if the file cannot be opened or a read fails
 * mid-way (treat that as "verification failed", not "verification skipped").
 *
 * Thread-safety: safe to call from any thread. The caller typically
 * dispatches this onto EAsyncExecution::ThreadPool because a 3+ GB
 * .litertlm file takes several seconds even on fast SSDs.
 */
FString ComputeFileSha256(const FString& Path);

} // namespace InoAgents
