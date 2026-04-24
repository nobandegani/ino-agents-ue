// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.
//
// Public-domain SHA-256 core adapted from
// https://github.com/B-Con/crypto-algorithms/blob/master/sha256.c. Reformatted
// for UE style and wrapped in the InoAgents namespace. No behavioural changes
// from the reference implementation — this is a stock FIPS 180-4 SHA-256.

#include "LiteRtLm/InoSha256.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

#include "InoAgentsLog.h"

namespace InoAgents
{

namespace
{

constexpr uint32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

FORCEINLINE uint32 RotR(uint32 X, uint32 N) { return (X >> N) | (X << (32 - N)); }
FORCEINLINE uint32 Ch (uint32 X, uint32 Y, uint32 Z) { return (X & Y) ^ (~X & Z); }
FORCEINLINE uint32 Maj(uint32 X, uint32 Y, uint32 Z) { return (X & Y) ^ (X & Z) ^ (Y & Z); }
FORCEINLINE uint32 Ep0(uint32 X)  { return RotR(X, 2)  ^ RotR(X, 13) ^ RotR(X, 22); }
FORCEINLINE uint32 Ep1(uint32 X)  { return RotR(X, 6)  ^ RotR(X, 11) ^ RotR(X, 25); }
FORCEINLINE uint32 Sig0(uint32 X) { return RotR(X, 7)  ^ RotR(X, 18) ^ (X >> 3); }
FORCEINLINE uint32 Sig1(uint32 X) { return RotR(X, 17) ^ RotR(X, 19) ^ (X >> 10); }

void Transform(uint32 State[8], const uint8 Block[64])
{
    uint32 M[64];
    for (int32 I = 0, J = 0; I < 16; ++I, J += 4)
    {
        M[I] = (uint32(Block[J])     << 24) |
               (uint32(Block[J + 1]) << 16) |
               (uint32(Block[J + 2]) <<  8) |
               (uint32(Block[J + 3]));
    }
    for (int32 I = 16; I < 64; ++I)
    {
        M[I] = Sig1(M[I - 2]) + M[I - 7] + Sig0(M[I - 15]) + M[I - 16];
    }

    uint32 A = State[0], B = State[1], C = State[2], D = State[3];
    uint32 E = State[4], F = State[5], G = State[6], H = State[7];

    for (int32 I = 0; I < 64; ++I)
    {
        const uint32 T1 = H + Ep1(E) + Ch(E, F, G) + K[I] + M[I];
        const uint32 T2 = Ep0(A) + Maj(A, B, C);
        H = G;  G = F;  F = E;  E = D + T1;
        D = C;  C = B;  B = A;  A = T1 + T2;
    }

    State[0] += A; State[1] += B; State[2] += C; State[3] += D;
    State[4] += E; State[5] += F; State[6] += G; State[7] += H;
}

} // namespace

FSha256Hasher::FSha256Hasher()
    : BufferLen(0)
    , BitLen(0)
{
    // FIPS 180-4 SHA-256 initial hash values.
    State[0] = 0x6a09e667; State[1] = 0xbb67ae85;
    State[2] = 0x3c6ef372; State[3] = 0xa54ff53a;
    State[4] = 0x510e527f; State[5] = 0x9b05688c;
    State[6] = 0x1f83d9ab; State[7] = 0x5be0cd19;
    FMemory::Memzero(Buffer, sizeof(Buffer));
}

void FSha256Hasher::Update(const void* InData, SIZE_T Len)
{
    const uint8* P = static_cast<const uint8*>(InData);
    SIZE_T       N = Len;

    // 1) Top up the pending partial block, if any.
    if (BufferLen > 0)
    {
        const SIZE_T Want = FMath::Min<SIZE_T>(64 - BufferLen, N);
        FMemory::Memcpy(Buffer + BufferLen, P, Want);
        BufferLen += static_cast<uint32>(Want);
        P         += Want;
        N         -= Want;

        if (BufferLen == 64)
        {
            Transform(State, Buffer);
            BitLen   += 512;
            BufferLen = 0;
        }
    }

    // 2) Consume full 64-byte blocks straight from the input buffer.
    while (N >= 64)
    {
        Transform(State, P);
        BitLen += 512;
        P      += 64;
        N      -= 64;
    }

    // 3) Stash any tail for the next Update / Finalize.
    if (N > 0)
    {
        FMemory::Memcpy(Buffer, P, N);
        BufferLen = static_cast<uint32>(N);
    }
}

void FSha256Hasher::FinalizeRaw(uint8 OutHash[32])
{
    uint32 I = BufferLen;

    // Pad with 0x80 followed by zeros, leaving room for the 64-bit length.
    if (BufferLen < 56)
    {
        Buffer[I++] = 0x80;
        while (I < 56) { Buffer[I++] = 0; }
    }
    else
    {
        Buffer[I++] = 0x80;
        while (I < 64) { Buffer[I++] = 0; }
        Transform(State, Buffer);
        FMemory::Memzero(Buffer, 56);
    }

    // Append message length in bits, big-endian.
    BitLen += static_cast<uint64>(BufferLen) * 8;
    Buffer[63] = static_cast<uint8>(BitLen);
    Buffer[62] = static_cast<uint8>(BitLen >>  8);
    Buffer[61] = static_cast<uint8>(BitLen >> 16);
    Buffer[60] = static_cast<uint8>(BitLen >> 24);
    Buffer[59] = static_cast<uint8>(BitLen >> 32);
    Buffer[58] = static_cast<uint8>(BitLen >> 40);
    Buffer[57] = static_cast<uint8>(BitLen >> 48);
    Buffer[56] = static_cast<uint8>(BitLen >> 56);
    Transform(State, Buffer);

    // Emit the state as big-endian bytes.
    for (uint32 J = 0; J < 4; ++J)
    {
        const uint32 Shift = 24 - J * 8;
        OutHash[J     ] = static_cast<uint8>((State[0] >> Shift) & 0xff);
        OutHash[J +  4] = static_cast<uint8>((State[1] >> Shift) & 0xff);
        OutHash[J +  8] = static_cast<uint8>((State[2] >> Shift) & 0xff);
        OutHash[J + 12] = static_cast<uint8>((State[3] >> Shift) & 0xff);
        OutHash[J + 16] = static_cast<uint8>((State[4] >> Shift) & 0xff);
        OutHash[J + 20] = static_cast<uint8>((State[5] >> Shift) & 0xff);
        OutHash[J + 24] = static_cast<uint8>((State[6] >> Shift) & 0xff);
        OutHash[J + 28] = static_cast<uint8>((State[7] >> Shift) & 0xff);
    }
}

FString FSha256Hasher::FinalizeHex()
{
    uint8 Digest[32];
    FinalizeRaw(Digest);

    // Lowercase 64-char hex — same format HuggingFace / git-lfs / sha256sum emit.
    static const TCHAR* const HexChars = TEXT("0123456789abcdef");
    TCHAR                     Out[65];
    for (int32 I = 0; I < 32; ++I)
    {
        Out[I * 2    ] = HexChars[(Digest[I] >> 4) & 0xf];
        Out[I * 2 + 1] = HexChars[ Digest[I]       & 0xf];
    }
    Out[64] = 0;
    return FString(Out);
}

FString ComputeFileSha256(const FString& Path)
{
    if (Path.IsEmpty())
    {
        return FString();
    }

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PF.OpenRead(*Path));
    if (!Handle.IsValid())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Subsystem: ComputeFileSha256 failed to open %s"), *Path);
        return FString();
    }

    constexpr int64 kBufSize = 1 << 20; // 1 MB chunks
    TArray<uint8>   Buffer;
    Buffer.SetNumUninitialized(kBufSize);

    FSha256Hasher Hasher;
    int64         Remaining = Handle->Size();
    while (Remaining > 0)
    {
        const int64 ToRead = FMath::Min<int64>(Remaining, kBufSize);
        if (!Handle->Read(Buffer.GetData(), ToRead))
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Subsystem: ComputeFileSha256 read failed at offset %lld of %s"),
                   Handle->Size() - Remaining, *Path);
            return FString();
        }
        Hasher.Update(Buffer.GetData(), static_cast<SIZE_T>(ToRead));
        Remaining -= ToRead;
    }

    return Hasher.FinalizeHex();
}

} // namespace InoAgents
