#define LOG_SOURCE "hwmon"

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
 * Modified to work with ethminer by
 *
 * Philipp Andreas - github@smurfy.de
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wrapnvml.h"

#if defined(__cplusplus)
extern "C" {
#endif

wrap_nvml_handle* wrap_nvml_create()
{
    wrap_nvml_handle* nvmlh = nullptr;

    /*
     * We use hard-coded library installation locations for the time being...
     * No idea where or if libnvidia-ml.so is installed on MacOS X, a
     * deep scouring of the filesystem on one of the Mac CUDA build boxes
     * I used turned up nothing, so for now it's not going to work on OSX.
     */
#if defined(_WIN32)
    /* Windows */
#define libnvidia_ml "%PROGRAMFILES%/NVIDIA Corporation/NVSMI/nvml.dll"
#elif defined(__linux) && (defined(__i386__) || defined(__ARM_ARCH_7A__))
/* In rpm based linux distributions link name is with extension .1 */
    /* 32-bit linux assumed */
#define libnvidia_ml "libnvidia-ml.so.1"
#elif defined(__linux)
    /* 64-bit linux assumed */
#define libnvidia_ml "libnvidia-ml.so.1"
#else
#define libnvidia_ml ""
#warning "Unrecognized platform: need NVML DLL path for this platform..."
    return nullptr;
#endif

    /* HASHWARP_NVML_LIB: another NVML library (the tests use a fake one) */
    const char* lib = getenv("HASHWARP_NVML_LIB");
#ifdef _WIN32
    /* Current drivers install nvml.dll in System32, older ones in NVSMI */
    void* nvml_dll = wrap_dlopen(lib ? lib : "nvml.dll");
    if (nvml_dll == nullptr && !lib)
    {
        char tmp[512];
        ExpandEnvironmentStringsA(libnvidia_ml, tmp, sizeof(tmp));
        nvml_dll = wrap_dlopen(tmp);
    }
#else
    void* nvml_dll = wrap_dlopen(lib ? lib : libnvidia_ml);
#endif
    if (nvml_dll == nullptr)
    {
        cwarn << "NVML lacks functions hashwarp needs: no NVIDIA sensors";
        return nullptr;
    }
        

    nvmlh = (wrap_nvml_handle*)calloc(1, sizeof(wrap_nvml_handle));

    nvmlh->nvml_dll = nvml_dll;

    nvmlh->nvmlInit = (wrap_nvmlReturn_t(*)(void))wrap_dlsym(nvmlh->nvml_dll, "nvmlInit");
    nvmlh->nvmlDeviceGetCount =
        (wrap_nvmlReturn_t(*)(int*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetCount_v2");
    nvmlh->nvmlDeviceGetHandleByIndex = (wrap_nvmlReturn_t(*)(int, wrap_nvmlDevice_t*))wrap_dlsym(
        nvmlh->nvml_dll, "nvmlDeviceGetHandleByIndex_v2");
    nvmlh->nvmlDeviceGetPciInfo = (wrap_nvmlReturn_t(*)(
        wrap_nvmlDevice_t, wrap_nvmlPciInfo_t*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetPciInfo");
    nvmlh->nvmlDeviceGetName = (wrap_nvmlReturn_t(*)(wrap_nvmlDevice_t, char*, int))wrap_dlsym(
        nvmlh->nvml_dll, "nvmlDeviceGetName");
    nvmlh->nvmlDeviceGetTemperature = (wrap_nvmlReturn_t(*)(wrap_nvmlDevice_t, int,
        unsigned int*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetTemperature");
    nvmlh->nvmlDeviceGetFanSpeed = (wrap_nvmlReturn_t(*)(
        wrap_nvmlDevice_t, unsigned int*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetFanSpeed");
    nvmlh->nvmlDeviceGetPowerUsage = (wrap_nvmlReturn_t(*)(
        wrap_nvmlDevice_t, unsigned int*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetPowerUsage");
    nvmlh->nvmlShutdown = (wrap_nvmlReturn_t(*)())wrap_dlsym(nvmlh->nvml_dll, "nvmlShutdown");
    nvmlh->nvmlDeviceGetClockInfo = (wrap_nvmlReturn_t(*)(wrap_nvmlDevice_t, int,
        unsigned int*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetClockInfo");
    nvmlh->nvmlDeviceGetEnforcedPowerLimit = (wrap_nvmlReturn_t(*)(wrap_nvmlDevice_t,
        unsigned int*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetEnforcedPowerLimit");
    nvmlh->nvmlDeviceGetFieldValues = (wrap_nvmlReturn_t(*)(wrap_nvmlDevice_t, int,
        wrap_nvmlFieldValue_t*))wrap_dlsym(nvmlh->nvml_dll, "nvmlDeviceGetFieldValues");

#define WRAPNVML_SYM(name) *(void**)&nvmlh->name = wrap_dlsym(nvmlh->nvml_dll, #name)
    WRAPNVML_SYM(nvmlErrorString);
    WRAPNVML_SYM(nvmlDeviceGetUUID);
    WRAPNVML_SYM(nvmlDeviceGetMaxClockInfo);
    WRAPNVML_SYM(nvmlDeviceGetPowerManagementLimit);
    WRAPNVML_SYM(nvmlDeviceGetPowerManagementLimitConstraints);
    WRAPNVML_SYM(nvmlDeviceGetPowerManagementDefaultLimit);
    WRAPNVML_SYM(nvmlDeviceSetPowerManagementLimit);
    WRAPNVML_SYM(nvmlDeviceSetGpuLockedClocks);
    WRAPNVML_SYM(nvmlDeviceResetGpuLockedClocks);
    WRAPNVML_SYM(nvmlDeviceGetClockOffsets);
    WRAPNVML_SYM(nvmlDeviceSetClockOffsets);
    WRAPNVML_SYM(nvmlDeviceGetGpcClkVfOffset);
    WRAPNVML_SYM(nvmlDeviceSetGpcClkVfOffset);
    WRAPNVML_SYM(nvmlDeviceGetMemClkVfOffset);
    WRAPNVML_SYM(nvmlDeviceSetMemClkVfOffset);
    WRAPNVML_SYM(nvmlDeviceGetGpcClkMinMaxVfOffset);
    WRAPNVML_SYM(nvmlDeviceGetMemClkMinMaxVfOffset);
    WRAPNVML_SYM(nvmlDeviceGetNumFans);
    WRAPNVML_SYM(nvmlDeviceSetFanSpeed_v2);
    WRAPNVML_SYM(nvmlDeviceSetDefaultFanSpeed_v2);
#undef WRAPNVML_SYM

    if (nvmlh->nvmlInit == nullptr || nvmlh->nvmlShutdown == nullptr ||
        nvmlh->nvmlDeviceGetCount == nullptr || nvmlh->nvmlDeviceGetHandleByIndex == nullptr ||
        nvmlh->nvmlDeviceGetPciInfo == nullptr || nvmlh->nvmlDeviceGetName == nullptr ||
        nvmlh->nvmlDeviceGetTemperature == nullptr || nvmlh->nvmlDeviceGetFanSpeed == nullptr ||
        nvmlh->nvmlDeviceGetPowerUsage == nullptr)
    {
        cwarn << "NVML lacks functions hashwarp needs: no NVIDIA sensors";

        wrap_dlclose(nvmlh->nvml_dll);
        free(nvmlh);
        return nullptr;
    }

    nvmlh->nvmlInit();
    nvmlh->nvmlDeviceGetCount(&nvmlh->nvml_gpucount);

    nvmlh->devs = (wrap_nvmlDevice_t*)calloc(nvmlh->nvml_gpucount, sizeof(wrap_nvmlDevice_t));
    nvmlh->nvml_pci_domain_id = (unsigned int*)calloc(nvmlh->nvml_gpucount, sizeof(unsigned int));
    nvmlh->nvml_pci_bus_id = (unsigned int*)calloc(nvmlh->nvml_gpucount, sizeof(unsigned int));
    nvmlh->nvml_pci_device_id = (unsigned int*)calloc(nvmlh->nvml_gpucount, sizeof(unsigned int));

    /* Obtain GPU device handles we're going to need repeatedly... */
    for (int i = 0; i < nvmlh->nvml_gpucount; i++)
    {
        nvmlh->nvmlDeviceGetHandleByIndex(i, &nvmlh->devs[i]);
    }

    /* Query PCI info for each NVML device, and build table for mapping of */
    /* CUDA device IDs to NVML device IDs and vice versa                   */
    for (int i = 0; i < nvmlh->nvml_gpucount; i++)
    {
        wrap_nvmlPciInfo_t pciinfo;
        nvmlh->nvmlDeviceGetPciInfo(nvmlh->devs[i], &pciinfo);
        nvmlh->nvml_pci_domain_id[i] = pciinfo.domain;
        nvmlh->nvml_pci_bus_id[i] = pciinfo.bus;
        nvmlh->nvml_pci_device_id[i] = pciinfo.device;
    }

    return nvmlh;
}


int wrap_nvml_destroy(wrap_nvml_handle* nvmlh)
{
    nvmlh->nvmlShutdown();

    wrap_dlclose(nvmlh->nvml_dll);
    free(nvmlh);
    return 0;
}

int wrap_nvml_get_gpucount(wrap_nvml_handle* nvmlh, int* gpucount)
{
    *gpucount = nvmlh->nvml_gpucount;
    return 0;
}

int wrap_nvml_get_gpu_name(wrap_nvml_handle* nvmlh, int gpuindex, char* namebuf, int bufsize)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)
        return -1;

    if (nvmlh->nvmlDeviceGetName(nvmlh->devs[gpuindex], namebuf, bufsize) != WRAPNVML_SUCCESS)
        return -1;

    return 0;
}

int wrap_nvml_get_tempC(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* tempC)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)
        return -1;

    if (nvmlh->nvmlDeviceGetTemperature(
            nvmlh->devs[gpuindex], 0u /* NVML_TEMPERATURE_GPU */, tempC) != WRAPNVML_SUCCESS)
        return -1;

    return 0;
}

int wrap_nvml_get_fanpcnt(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* fanpcnt)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)
        return -1;

    if (nvmlh->nvmlDeviceGetFanSpeed(nvmlh->devs[gpuindex], fanpcnt) != WRAPNVML_SUCCESS)
        return -1;

    return 0;
}

int wrap_nvml_get_power_usage(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* milliwatts)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)
        return -1;

    if (nvmlh->nvmlDeviceGetPowerUsage(nvmlh->devs[gpuindex], milliwatts) != WRAPNVML_SUCCESS)
        return -1;

    return 0;
}

int wrap_nvml_get_mem_tempC(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* tempC)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount || !nvmlh->nvmlDeviceGetFieldValues)
        return -1;
    wrap_nvmlFieldValue_t field;
    memset(&field, 0, sizeof(field));
    field.fieldId = 82; /* NVML_FI_DEV_MEMORY_TEMP */
    if (nvmlh->nvmlDeviceGetFieldValues(nvmlh->devs[gpuindex], 1, &field) != WRAPNVML_SUCCESS ||
        field.nvmlReturn != WRAPNVML_SUCCESS)
        return -1;
    /* nvmlValueType_t: 0 double, 1 uint, 2 ulong, 3 ulonglong, 4 slonglong */
    long long v;
    switch (field.valueType)
    {
    case 0:
        v = (long long)field.value.dVal;
        break;
    case 1:
        v = field.value.uiVal;
        break;
    case 2:
        v = (long long)field.value.ulVal;
        break;
    case 4:
        v = field.value.sllVal;
        break;
    default:
        v = (long long)field.value.ullVal;
    }
    if (v <= 0 || v > 200)
        return -1;
    *tempC = (unsigned int)v;
    return 0;
}

int wrap_nvml_get_clocks(
    wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* coreMHz, unsigned int* memMHz)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount || !nvmlh->nvmlDeviceGetClockInfo)
        return -1;
    if (nvmlh->nvmlDeviceGetClockInfo(nvmlh->devs[gpuindex], 0 /* NVML_CLOCK_GRAPHICS */,
            coreMHz) != WRAPNVML_SUCCESS)
        *coreMHz = 0;
    if (nvmlh->nvmlDeviceGetClockInfo(nvmlh->devs[gpuindex], 2 /* NVML_CLOCK_MEM */, memMHz) !=
        WRAPNVML_SUCCESS)
        *memMHz = 0;
    return 0;
}

int wrap_nvml_get_power_limit(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* milliwatts)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount || !nvmlh->nvmlDeviceGetEnforcedPowerLimit)
        return -1;
    if (nvmlh->nvmlDeviceGetEnforcedPowerLimit(nvmlh->devs[gpuindex], milliwatts) !=
        WRAPNVML_SUCCESS)
        return -1;
    return 0;
}

/* nvmlClockOffset_v1_t (r555 drivers and newer) */
typedef struct
{
    unsigned int version;
    int type;   /* nvmlClockType_t */
    int pstate; /* nvmlPstates_t */
    int clockOffsetMHz;
    int minClockOffsetMHz;
    int maxClockOffsetMHz;
} wrap_nvmlClockOffset_t;

static void init_clock_offset(wrap_nvmlClockOffset_t* info, int mem)
{
    memset(info, 0, sizeof(*info));
    info->version = (unsigned int)sizeof(*info) | (1u << 24); /* nvmlClockOffset_v1 */
    info->type = mem ? 2 /* NVML_CLOCK_MEM */ : 0 /* NVML_CLOCK_GRAPHICS */;
    info->pstate = 0; /* NVML_PSTATE_0 */
}

#define WRAPNVML_CHECK(fn)                                      \
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)       \
        return -1;                                              \
    if (!nvmlh->fn)                                             \
        return WRAPNVML_NOT_FOUND;

const char* wrap_nvml_error(wrap_nvml_handle* nvmlh, int code)
{
    if (code == WRAPNVML_NOT_FOUND)
        return "not supported by this driver";
    if (code == WRAPNVML_NO_PERMISSION)
#if defined(_WIN32)
        return "needs administrator rights";
#else
        return "needs root (run hashwarp with sudo)";
#endif
    if (code < 0 || !nvmlh->nvmlErrorString)
        return "unknown error";
    return nvmlh->nvmlErrorString(code);
}

int wrap_nvml_get_uuid(wrap_nvml_handle* nvmlh, int gpuindex, char* buf, int bufsize)
{
    WRAPNVML_CHECK(nvmlDeviceGetUUID)
    return nvmlh->nvmlDeviceGetUUID(nvmlh->devs[gpuindex], buf, (unsigned int)bufsize);
}

int wrap_nvml_get_power_limits(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int* currentMw,
    unsigned int* minMw, unsigned int* maxMw, unsigned int* defaultMw)
{
    WRAPNVML_CHECK(nvmlDeviceGetPowerManagementLimit)
    if (!nvmlh->nvmlDeviceGetPowerManagementLimitConstraints ||
        !nvmlh->nvmlDeviceGetPowerManagementDefaultLimit)
        return WRAPNVML_NOT_FOUND;
    wrap_nvmlDevice_t dev = nvmlh->devs[gpuindex];
    int r = nvmlh->nvmlDeviceGetPowerManagementLimit(dev, currentMw);
    if (r == 0)
        r = nvmlh->nvmlDeviceGetPowerManagementLimitConstraints(dev, minMw, maxMw);
    if (r == 0)
        r = nvmlh->nvmlDeviceGetPowerManagementDefaultLimit(dev, defaultMw);
    return r;
}

int wrap_nvml_set_power_limit(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int milliwatts)
{
    WRAPNVML_CHECK(nvmlDeviceSetPowerManagementLimit)
    return nvmlh->nvmlDeviceSetPowerManagementLimit(nvmlh->devs[gpuindex], milliwatts);
}

int wrap_nvml_get_max_clock(wrap_nvml_handle* nvmlh, int gpuindex, int mem, unsigned int* mhz)
{
    WRAPNVML_CHECK(nvmlDeviceGetMaxClockInfo)
    return nvmlh->nvmlDeviceGetMaxClockInfo(nvmlh->devs[gpuindex], mem ? 2 : 0, mhz);
}

int wrap_nvml_lock_core_clock(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int mhz)
{
    WRAPNVML_CHECK(nvmlDeviceSetGpuLockedClocks)
    if (!mhz)
        return nvmlh->nvmlDeviceResetGpuLockedClocks ?
                   nvmlh->nvmlDeviceResetGpuLockedClocks(nvmlh->devs[gpuindex]) :
                   WRAPNVML_NOT_FOUND;
    return nvmlh->nvmlDeviceSetGpuLockedClocks(nvmlh->devs[gpuindex], mhz, mhz);
}

int wrap_nvml_get_clock_offset(
    wrap_nvml_handle* nvmlh, int gpuindex, int mem, int* offset, int* minOffset, int* maxOffset)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)
        return -1;
    wrap_nvmlDevice_t dev = nvmlh->devs[gpuindex];
    if (nvmlh->nvmlDeviceGetClockOffsets)
    {
        wrap_nvmlClockOffset_t info;
        init_clock_offset(&info, mem);
        int r = nvmlh->nvmlDeviceGetClockOffsets(dev, &info);
        if (r == 0)
        {
            *offset = info.clockOffsetMHz;
            *minOffset = info.minClockOffsetMHz;
            *maxOffset = info.maxClockOffsetMHz;
            return 0;
        }
        if (r != 3 /* NVML_ERROR_NOT_SUPPORTED */)
            return r;
    }
    /* Older drivers (r470 to r550) */
    wrap_nvmlReturn_t (*get)(wrap_nvmlDevice_t, int*) =
        mem ? nvmlh->nvmlDeviceGetMemClkVfOffset : nvmlh->nvmlDeviceGetGpcClkVfOffset;
    wrap_nvmlReturn_t (*range)(wrap_nvmlDevice_t, int*, int*) =
        mem ? nvmlh->nvmlDeviceGetMemClkMinMaxVfOffset : nvmlh->nvmlDeviceGetGpcClkMinMaxVfOffset;
    if (!get || !range)
        return WRAPNVML_NOT_FOUND;
    int r = get(dev, offset);
    return r ? r : range(dev, minOffset, maxOffset);
}

int wrap_nvml_set_clock_offset(wrap_nvml_handle* nvmlh, int gpuindex, int mem, int offset)
{
    if (gpuindex < 0 || gpuindex >= nvmlh->nvml_gpucount)
        return -1;
    wrap_nvmlDevice_t dev = nvmlh->devs[gpuindex];
    if (nvmlh->nvmlDeviceSetClockOffsets)
    {
        wrap_nvmlClockOffset_t info;
        init_clock_offset(&info, mem);
        info.clockOffsetMHz = offset;
        int r = nvmlh->nvmlDeviceSetClockOffsets(dev, &info);
        if (r != 3 /* NVML_ERROR_NOT_SUPPORTED */)
            return r;
    }
    wrap_nvmlReturn_t (*set)(wrap_nvmlDevice_t, int) =
        mem ? nvmlh->nvmlDeviceSetMemClkVfOffset : nvmlh->nvmlDeviceSetGpcClkVfOffset;
    return set ? set(dev, offset) : WRAPNVML_NOT_FOUND;
}

int wrap_nvml_set_fans(wrap_nvml_handle* nvmlh, int gpuindex, unsigned int pct)
{
    WRAPNVML_CHECK(nvmlDeviceGetNumFans)
    if (!nvmlh->nvmlDeviceSetFanSpeed_v2 || !nvmlh->nvmlDeviceSetDefaultFanSpeed_v2)
        return WRAPNVML_NOT_FOUND;
    wrap_nvmlDevice_t dev = nvmlh->devs[gpuindex];
    unsigned int fans = 0;
    int r = nvmlh->nvmlDeviceGetNumFans(dev, &fans);
    for (unsigned int fan = 0; r == 0 && fan < fans; fan++)
        r = pct > 100 ? nvmlh->nvmlDeviceSetDefaultFanSpeed_v2(dev, fan) :
                        nvmlh->nvmlDeviceSetFanSpeed_v2(dev, fan, pct);
    return r;
}

#undef WRAPNVML_CHECK

#if defined(__cplusplus)
}
#endif
