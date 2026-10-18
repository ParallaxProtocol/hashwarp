/*
 * A trivial little dlopen()-based wrapper library for the
 * NVIDIA NVML library, to allow runtime discovery of NVML on an
 * arbitrary system.  This is all very hackish and simple-minded, but
 * it serves my immediate needs in the short term until NVIDIA provides
 * a static NVML wrapper library themselves, hopefully in
 * CUDA 6.5 or maybe sometime shortly after.
 *
 * This trivial code is made available under the "new" 3-clause BSD license,
 * and/or any of the GPL licenses you prefer.
 * Feel free to use the code and modify as you see fit.
 *
 * John E. Stone - john.stone@gmail.com
 *
 */

#pragma once

#include "wraphelper.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Ugly hacks to avoid dependencies on the real nvml.h until it starts
 * getting included with the CUDA toolkit or a GDK that's got a known
 * install location, etc.
 */
typedef enum wrap_nvmlReturn_enum { WRAPNVML_SUCCESS = 0 } wrap_nvmlReturn_t;

typedef void* wrap_nvmlDevice_t;

/* our own version of the PCI info struct */
typedef struct
{
    char bus_id_str[16]; /* string form of bus info */
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pci_device_id; /* combined device and vendor id */
    unsigned int pci_subsystem_id;
    unsigned int res0; /* NVML internal use only */
    unsigned int res1;
    unsigned int res2;
    unsigned int res3;
} wrap_nvmlPciInfo_t;


/*
 * Handle to hold the function pointers for the entry points we need,
 * and the shared library itself.
 */
/* Subset of nvmlFieldValue_t used to query field values */
typedef struct
{
    unsigned int fieldId;
    unsigned int scopeId;
    long long timestamp;
    long long latencyUsec;
    int valueType;
    wrap_nvmlReturn_t nvmlReturn;
    union
    {
        double dVal;
        unsigned int uiVal;
        unsigned long ulVal;
        unsigned long long ullVal;
        signed long long sllVal;
    } value;
} wrap_nvmlFieldValue_t;

typedef struct
{
    void* nvml_dll;
    int nvml_gpucount;
    unsigned int* nvml_pci_domain_id;
    unsigned int* nvml_pci_bus_id;
    unsigned int* nvml_pci_device_id;
    wrap_nvmlDevice_t* devs;
    wrap_nvmlReturn_t (*nvmlInit)(void);
    wrap_nvmlReturn_t (*nvmlDeviceGetCount)(int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetHandleByIndex)(int, wrap_nvmlDevice_t*);
    wrap_nvmlReturn_t (*nvmlDeviceGetPciInfo)(wrap_nvmlDevice_t, wrap_nvmlPciInfo_t*);
    wrap_nvmlReturn_t (*nvmlDeviceGetName)(wrap_nvmlDevice_t, char*, int);
    wrap_nvmlReturn_t (*nvmlDeviceGetTemperature)(wrap_nvmlDevice_t, int, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetFanSpeed)(wrap_nvmlDevice_t, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetPowerUsage)(wrap_nvmlDevice_t, unsigned int*);
    wrap_nvmlReturn_t (*nvmlShutdown)(void);
    /* Optional (null if the driver doesn't provide them) */
    wrap_nvmlReturn_t (*nvmlDeviceGetClockInfo)(wrap_nvmlDevice_t, int, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetEnforcedPowerLimit)(wrap_nvmlDevice_t, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetFieldValues)(wrap_nvmlDevice_t, int, wrap_nvmlFieldValue_t*);
    /* Overclocking (optional too) */
    const char* (*nvmlErrorString)(int);
    wrap_nvmlReturn_t (*nvmlDeviceGetUUID)(wrap_nvmlDevice_t, char*, unsigned int);
    wrap_nvmlReturn_t (*nvmlDeviceGetMaxClockInfo)(wrap_nvmlDevice_t, int, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetPowerManagementLimit)(wrap_nvmlDevice_t, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetPowerManagementLimitConstraints)(
        wrap_nvmlDevice_t, unsigned int*, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetPowerManagementDefaultLimit)(wrap_nvmlDevice_t, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceSetPowerManagementLimit)(wrap_nvmlDevice_t, unsigned int);
    wrap_nvmlReturn_t (*nvmlDeviceSetGpuLockedClocks)(wrap_nvmlDevice_t, unsigned int, unsigned int);
    wrap_nvmlReturn_t (*nvmlDeviceResetGpuLockedClocks)(wrap_nvmlDevice_t);
    wrap_nvmlReturn_t (*nvmlDeviceGetClockOffsets)(wrap_nvmlDevice_t, void*);
    wrap_nvmlReturn_t (*nvmlDeviceSetClockOffsets)(wrap_nvmlDevice_t, void*);
    wrap_nvmlReturn_t (*nvmlDeviceGetGpcClkVfOffset)(wrap_nvmlDevice_t, int*);
    wrap_nvmlReturn_t (*nvmlDeviceSetGpcClkVfOffset)(wrap_nvmlDevice_t, int);
    wrap_nvmlReturn_t (*nvmlDeviceGetMemClkVfOffset)(wrap_nvmlDevice_t, int*);
    wrap_nvmlReturn_t (*nvmlDeviceSetMemClkVfOffset)(wrap_nvmlDevice_t, int);
    wrap_nvmlReturn_t (*nvmlDeviceGetGpcClkMinMaxVfOffset)(wrap_nvmlDevice_t, int*, int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetMemClkMinMaxVfOffset)(wrap_nvmlDevice_t, int*, int*);
    wrap_nvmlReturn_t (*nvmlDeviceGetNumFans)(wrap_nvmlDevice_t, unsigned int*);
    wrap_nvmlReturn_t (*nvmlDeviceSetFanSpeed_v2)(wrap_nvmlDevice_t, unsigned int, unsigned int);
    wrap_nvmlReturn_t (*nvmlDeviceSetDefaultFanSpeed_v2)(wrap_nvmlDevice_t, unsigned int);
} wrap_nvml_handle;


wrap_nvml_handle* wrap_nvml_create();
int wrap_nvml_destroy(wrap_nvml_handle* nvmlh);

/*
 * Query the number of GPUs seen by NVML
 */
int wrap_nvml_get_gpucount(wrap_nvml_handle* nvmlh, int* gpucount);

/*
 * query the name of the GPU model from the CUDA device ID
 *
 */
int wrap_nvml_get_gpu_name(wrap_nvml_handle* nvmlh, int gpuindex, char* namebuf, int bufsize);

/*
 * Query the current GPU temperature (Celsius), from the CUDA device ID
 */
int wrap_nvml_get_tempC(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* tempC);

/*
 * Query the current GPU fan speed (percent) from the CUDA device ID
 */
int wrap_nvml_get_fanpcnt(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* fanpcnt);

/*
 * Query the current GPU power usage in milliwatts from the CUDA device ID
 *
 * This feature is only available on recent GPU generations and may be
 * limited in some cases only to Tesla series GPUs.
 * If the query is run on an unsupported GPU, this routine will return -1.
 */
int wrap_nvml_get_power_usage(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* milliwatts);

/* Memory temperature (HBM and some GDDR boards), -1 if not available */
int wrap_nvml_get_mem_tempC(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* tempC);

/* Current graphics and memory clocks in MHz */
int wrap_nvml_get_clocks(
    wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* coreMHz, unsigned int* memMHz);

/* Enforced power limit */
int wrap_nvml_get_power_limit(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* milliwatts);

/*
 * Overclocking. These return 0 on success, otherwise the NVML error code
 * (WRAPNVML_NOT_FOUND when the driver doesn't have the function). The setters
 * need root (Linux) or administrator (Windows) rights
 */
#define WRAPNVML_NO_PERMISSION 4
#define WRAPNVML_NOT_FOUND 13

/* Description of an error code returned by the functions below */
const char* wrap_nvml_error(wrap_nvml_handle* nvmlh, int code);

int wrap_nvml_get_uuid(wrap_nvml_handle* nvmlh, int gpuindex, char* buf, int bufsize);

/* Power limits in milliwatts: current, allowed range and default */
int wrap_nvml_get_power_limits(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* currentMw,
    unsigned int* minMw, unsigned int* maxMw, unsigned int* defaultMw);
int wrap_nvml_set_power_limit(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int milliwatts);

/* Highest core (mem = 0) or memory (mem = 1) clock in MHz */
int wrap_nvml_get_max_clock(wrap_nvml_handle* nvmlh, int gpuindex, int mem, unsigned int* mhz);

/* Keeps the core clock at mhz, 0 lets it boost again */
int wrap_nvml_lock_core_clock(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int mhz);

/* Core (mem = 0) or memory (mem = 1) clock offset of the highest performance state, in
   MHz. Memory offsets are in the units of nvidia-settings: twice MSI Afterburner's */
int wrap_nvml_get_clock_offset(
    wrap_nvml_handle* nvmlh, int gpuindex, int mem, int* offset, int* minOffset, int* maxOffset);
int wrap_nvml_set_clock_offset(wrap_nvml_handle* nvmlh, int gpuindex, int mem, int offset);

/* Speed of all the fans in percent, or back to automatic control when pct > 100 */
int wrap_nvml_set_fans(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int pct);


#if defined(__cplusplus)
}
#endif
