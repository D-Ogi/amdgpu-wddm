// hip_error.cpp - the per-thread error state, the status translation and the error entry points.
//
// Design docs/design/m16-hip-route-b.md section 4.5: the translation is one table and no
// judgement. BC250HSA_EDEVICELOST is the one code that asks a question first, because HIP tells
// a page fault from a dead context by different error codes.

#include <cstring>

#include "runtime_internal.h"

namespace bc250hip {

static thread_local hipError_t g_last_error = hipSuccess;

hipError_t last_error_peek() { return g_last_error; }

hipError_t last_error_take() {
    const hipError_t err = g_last_error;
    g_last_error = hipSuccess;
    return err;
}

void last_error_set(hipError_t err) { g_last_error = err; }

hipError_t translate(bc250hsa_status status) {
    switch (status) {
    case BC250HSA_OK:              return hipSuccess;
    case BC250HSA_EINVAL:          return hipErrorInvalidValue;
    case BC250HSA_ENOMEM:          return hipErrorOutOfMemory;
    case BC250HSA_ENODEV:          return hipErrorNoDevice;
    case BC250HSA_ETIMEOUT:        return hipErrorLaunchTimeOut;
    case BC250HSA_EUNSUPPORTED:    return hipErrorNotSupported;
    case BC250HSA_EOS:             return hipErrorUnknown;
    case BC250HSA_ENOTFOUND:       return hipErrorInvalidDeviceFunction;
    case BC250HSA_EBUSY:           return hipErrorNotReady;
    case BC250HSA_EDEVICELOST: {
        // A fault with an address is an illegal access of the program. A loss with no fault
        // block is a context that no longer exists.
        State& s = state();
        if (s.dev != nullptr) {
            bc250hsa_fault fault;
            // Zeroed first: the header does not promise that a query fills every field, and a
            // faulted_va of stack rubbish would name an illegal access where the context is
            // only gone. This part has no working GPU reset (fact M53), so the first
            // diagnosis has to be the right one.
            std::memset(&fault, 0, sizeof(fault));
            fault.struct_bytes = static_cast<uint32_t>(sizeof(fault));
            if (bc250hsa_query_fault(s.dev, &fault) == BC250HSA_OK && fault.faulted_va != 0) {
                return hipErrorIllegalAddress;
            }
        }
        return hipErrorContextIsDestroyed;
    }
    default:
        break;
    }
    // Every code at -20 and below is a code object or a bundle that we cannot use.
    if (static_cast<int>(status) <= static_cast<int>(BC250HSA_EBADELF)) {
        return hipErrorInvalidImage;
    }
    return hipErrorUnknown;
}

}  // namespace bc250hip

extern "C" {

hipError_t hipGetLastError(void) { return bc250hip::last_error_take(); }

hipError_t hipPeekAtLastError(void) { return bc250hip::last_error_peek(); }

const char* hipGetErrorName(hipError_t error) {
    switch (error) {
    case hipSuccess:                     return "hipSuccess";
    case hipErrorInvalidValue:           return "hipErrorInvalidValue";
    case hipErrorOutOfMemory:            return "hipErrorOutOfMemory";
    case hipErrorNotInitialized:         return "hipErrorNotInitialized";
    case hipErrorDeinitialized:          return "hipErrorDeinitialized";
    case hipErrorInvalidDevicePointer:   return "hipErrorInvalidDevicePointer";
    case hipErrorInvalidMemcpyDirection: return "hipErrorInvalidMemcpyDirection";
    case hipErrorInvalidImage:           return "hipErrorInvalidImage";
    case hipErrorContextIsDestroyed:     return "hipErrorContextIsDestroyed";
    case hipErrorNoDevice:               return "hipErrorNoDevice";
    case hipErrorInvalidDevice:          return "hipErrorInvalidDevice";
    case hipErrorInvalidDeviceFunction:  return "hipErrorInvalidDeviceFunction";
    case hipErrorIllegalAddress:         return "hipErrorIllegalAddress";
    case hipErrorLaunchTimeOut:          return "hipErrorLaunchTimeOut";
    case hipErrorNotReady:               return "hipErrorNotReady";
    case hipErrorInvalidHandle:          return "hipErrorInvalidHandle";
    case hipErrorNotSupported:           return "hipErrorNotSupported";
    case hipErrorUnknown:                return "hipErrorUnknown";
    default:                             break;
    }
    return "hipErrorUnknown";
}

const char* hipGetErrorString(hipError_t error) {
    switch (error) {
    case hipSuccess:                     return "no error";
    case hipErrorInvalidValue:           return "invalid argument";
    case hipErrorOutOfMemory:            return "out of memory";
    case hipErrorNotInitialized:         return "the runtime is not initialized";
    case hipErrorDeinitialized:          return "the runtime is shut down";
    case hipErrorInvalidDevicePointer:   return "invalid device pointer";
    case hipErrorInvalidMemcpyDirection: return "invalid copy direction";
    case hipErrorInvalidImage:           return "the device code object cannot be used";
    case hipErrorContextIsDestroyed:     return "the device context no longer exists";
    case hipErrorNoDevice:               return "no BC-250 device";
    case hipErrorInvalidDevice:          return "invalid device number";
    case hipErrorInvalidDeviceFunction:  return "invalid device function";
    case hipErrorIllegalAddress:         return "the device read or wrote an illegal address";
    case hipErrorLaunchTimeOut:          return "the kernel did not finish inside the wait bound";
    case hipErrorNotReady:               return "not ready";
    case hipErrorInvalidHandle:          return "invalid handle";
    case hipErrorNotSupported:           return "this build does not support the operation";
    case hipErrorUnknown:                return "unknown error";
    default:                             break;
    }
    return "unknown error";
}

}  // extern "C"
