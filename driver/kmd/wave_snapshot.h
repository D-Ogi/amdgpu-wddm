/* GFX10 wave observation ABI. No kernel types or pointers cross this interface.
 * A live read does not halt waves: fields are sequential samples, not an atomic
 * context image. Raw values and identity/status rechecks remain available. */
#ifndef BC250_WAVE_SNAPSHOT_H
#define BC250_WAVE_SNAPSHOT_H
typedef unsigned int BC250_WAVE_U32;
typedef int BC250_WAVE_I32;
typedef unsigned long long BC250_WAVE_U64;

#define BC250_WAVE_ABI_VERSION 1u
#define BC250_WAVE_MAX_RECORDS 2048u
#define BC250_WAVE_KIND_LIVE 0u
#define BC250_WAVE_KIND_PRE_KILL 1u
#define BC250_WAVE_COMPLETE 1u
#define BC250_WAVE_TRUNCATED 2u
#define BC250_WAVE_READ_ERROR 4u
#define BC250_WAVE_BUDGET_EXPIRED 8u

enum BC250_WAVE_REGISTER {
    Bc250WaveStatus, Bc250WavePcLo, Bc250WavePcHi,
    Bc250WaveExecLo, Bc250WaveExecHi, Bc250WaveHwId1, Bc250WaveHwId2,
    Bc250WaveInstDw0, Bc250WaveGprAlloc, Bc250WaveLdsAlloc, Bc250WaveTrapsts,
    Bc250WaveIbSts, Bc250WaveIbSts2, Bc250WaveIbDbg1, Bc250WaveM0,
    Bc250WaveMode, Bc250WaveRegisterCount
};

typedef struct BC250_WAVE_RECORD {
    BC250_WAVE_U32 Se, Sh, Wgp, Simd, Slot;
    BC250_WAVE_U32 StatusAfter, HwId1After, HwId2After;
    BC250_WAVE_U32 Raw[Bc250WaveRegisterCount];
} BC250_WAVE_RECORD;

typedef struct BC250_WAVE_HEADER {
    BC250_WAVE_U32 AbiVersion, Bytes, DriverVersion, Kind;
    BC250_WAVE_I32 Result;
    BC250_WAVE_U32 Flags, WaveCount, SlotsVisited;
    BC250_WAVE_U32 SeCount, ShPerSe, WgpPerSh, SimdPerWgp;
    BC250_WAVE_U32 WaveSlotsPerSimd, WgpVisited, InvalidReads, Reserved0;
    BC250_WAVE_U64 StartTime100ns, EndTime100ns;
    BC250_WAVE_U32 Seq, Fence, Vmid, FaultOffset;
    BC250_WAVE_U32 WgpMask[2][2];
    BC250_WAVE_U32 Reserved[4];
} BC250_WAVE_HEADER;

typedef struct BC250_WAVE_SNAPSHOT {
    BC250_WAVE_HEADER Header;
    BC250_WAVE_RECORD Waves[BC250_WAVE_MAX_RECORDS];
} BC250_WAVE_SNAPSHOT;

/* Complete response, always initialized by the miniport. Op=0 takes a live
 * snapshot. Saved recovery snapshots are read from HangRecovery in the registry. */
#define BC250_ESCAPE_GET_WAVES 33u
typedef struct BC250_ESCAPE_WAVES {
    BC250_WAVE_U32 Magic, Command, Version, Status;
    BC250_WAVE_I32 NtStatus;
    BC250_WAVE_U32 AbiVersion, Op, Reserved;
    BC250_WAVE_SNAPSHOT Snapshot;
} BC250_ESCAPE_WAVES;
#endif
