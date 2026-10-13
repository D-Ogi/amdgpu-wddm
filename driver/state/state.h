// Pure, bounded state contract shared by the kernel and x86/x64 user-mode adapters.
// No OS handles, allocation, registry path reconstruction or CRT dependency.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

enum amdgpu_state_status {
    AMDGPU_STATE_OK, AMDGPU_STATE_ABSENT, AMDGPU_STATE_UNKNOWN,
    AMDGPU_STATE_TYPE, AMDGPU_STATE_RANGE, AMDGPU_STATE_ACCESS,
    AMDGPU_STATE_IO, AMDGPU_STATE_SCHEMA, AMDGPU_STATE_INCOMPLETE,
    AMDGPU_STATE_BUFFER, AMDGPU_STATE_INVALID_ARGUMENT
};
enum amdgpu_state_type { AMDGPU_STATE_SZ=1, AMDGPU_STATE_BINARY=3,
    AMDGPU_STATE_DWORD=4, AMDGPU_STATE_MULTI_SZ=7, AMDGPU_STATE_QWORD=11 };
enum amdgpu_state_kind { AMDGPU_STATE_SETTING, AMDGPU_STATE_RECORD, AMDGPU_STATE_VOLATILE };
enum amdgpu_state_source { AMDGPU_STATE_NONE, AMDGPU_STATE_OVERRIDE,
    AMDGPU_STATE_FACTORY, AMDGPU_STATE_CODE };
enum amdgpu_state_mode { AMDGPU_STATE_QUERY_OVERRIDE, AMDGPU_STATE_QUERY_EFFECTIVE,
    AMDGPU_STATE_READ_RECORD };
enum amdgpu_state_access { AMDGPU_STATE_ADMIN, AMDGPU_STATE_DRIVER, AMDGPU_STATE_ADMIN_CLEAR };
enum amdgpu_state_group { AMDGPU_STATE_GROUP_NONE, AMDGPU_STATE_GROUP_CPU,
    AMDGPU_STATE_GROUP_CU, AMDGPU_STATE_GROUP_DPM, AMDGPU_STATE_GROUP_FAN,
    AMDGPU_STATE_GROUP_HWMON, AMDGPU_STATE_GROUP_INTEROP };

typedef struct amdgpu_state_spec {
    const char *path, *name;
    unsigned int kind, type, max_bytes, min, max;
    const unsigned int *allowed;
    unsigned int allowed_count, factory_present, factory, code_present, code;
    unsigned int access, private_read, group, legacy_off;
} AMDGPU_STATE_SPEC;

typedef struct amdgpu_state_value {
    unsigned int status, os_error, type, bytes, source;
} AMDGPU_STATE_VALUE;

// An absent value must return ABSENT; a missing root, denied access, invalid type,
// failed API and incomplete generation must return their distinct error status.
// The callback fills type/bytes on OK. No callback can authorize a default fallback
// by returning OK with an empty payload.
typedef unsigned int (*AMDGPU_STATE_FETCH)(void *context, unsigned int source,
    const AMDGPU_STATE_SPEC *spec, AMDGPU_STATE_VALUE *value, void *data, unsigned int capacity);

unsigned int AmdgpuStateCount(void);
unsigned int AmdgpuStateSchema(void);
const char *AmdgpuStateCatalogId(void);
const AMDGPU_STATE_SPEC *AmdgpuStateAt(unsigned int index);
const AMDGPU_STATE_SPEC *AmdgpuStateFind(const char *path, const char *name);
unsigned int AmdgpuStateValidate(const AMDGPU_STATE_SPEC *spec, unsigned int type,
    const void *data, unsigned int bytes);
unsigned int AmdgpuStateQuery(const AMDGPU_STATE_SPEC *spec, unsigned int mode,
    AMDGPU_STATE_FETCH fetch, void *context, AMDGPU_STATE_VALUE *value,
    void *data, unsigned int capacity);
unsigned int AmdgpuStateMayWrite(const AMDGPU_STATE_SPEC *spec, unsigned int privileged,
    unsigned int driver_writer, unsigned int type, const void *data, unsigned int bytes);
// The immutable Driver Store file is compared with the compiled contract before
// any default is consumed. No parser accepts an alternative catalog or schema.
unsigned int AmdgpuStatePackageBytes(void);
unsigned int AmdgpuStatePackageCheck(const void *data, unsigned int bytes);
unsigned int AmdgpuStatePackageRead(const void *package, unsigned int bytes,
    const AMDGPU_STATE_SPEC *spec, AMDGPU_STATE_VALUE *value, void *data, unsigned int capacity);

#ifdef __cplusplus
}
#endif
