// A fake NVML library for the unit tests: one GPU whose settings live in memory.
// Loaded instead of the driver's through the HASHWARP_NVML_LIB environment variable.

#include "fake_nvml.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
FakeNvmlState g_state;
int g_device;  // its handle

// FAKE_NVML_LOG prints the changes, FAKE_NVML_DENY refuses them, to try hashwarp with it
int denied()
{
    return (g_state.denyWrites || getenv("FAKE_NVML_DENY")) ? 4 /* NVML_ERROR_NO_PERMISSION */ :
                                                              0;
}

void log(const char* _what, long _value)
{
    if (getenv("FAKE_NVML_LOG"))
        fprintf(stderr, "fake-nvml: %s %ld\n", _what, _value);
}
}  // namespace

extern "C" {

FakeNvmlState* fake_nvml_state()
{
    return &g_state;
}

int nvmlInit()
{
    return 0;
}
int nvmlShutdown()
{
    return 0;
}
const char* nvmlErrorString(int _code)
{
    return _code == 2 ? "Invalid Argument" : "Fake error";
}
int nvmlDeviceGetCount_v2(int* _count)
{
    *_count = 1;
    return 0;
}
int nvmlDeviceGetHandleByIndex_v2(int, void** _dev)
{
    *_dev = &g_device;
    return 0;
}
int nvmlDeviceGetPciInfo(void*, unsigned* _info)
{
    // bus_id_str[16], domain, bus, device, ...
    std::memset(_info, 0, 16);
    _info[4] = 0;
    _info[5] = 1;
    _info[6] = 0;
    return 0;
}
int nvmlDeviceGetName(void*, char* _name, int _size)
{
    std::strncpy(_name, "Fake GeForce", _size);
    return 0;
}
int nvmlDeviceGetUUID(void*, char* _uuid, unsigned _size)
{
    std::strncpy(_uuid, "GPU-fake-0001", _size);
    return 0;
}
int nvmlDeviceGetTemperature(void*, int, unsigned* _t)
{
    *_t = 50;
    return 0;
}
int nvmlDeviceGetFanSpeed(void*, unsigned* _pct)
{
    *_pct = g_state.fanPct;
    return 0;
}
int nvmlDeviceGetPowerUsage(void*, unsigned* _mw)
{
    *_mw = 200000;
    return 0;
}
int nvmlDeviceGetMaxClockInfo(void*, int _type, unsigned* _mhz)
{
    *_mhz = _type == 0 ? 3000 : 10500;
    return 0;
}

int nvmlDeviceGetPowerManagementLimit(void*, unsigned* _mw)
{
    *_mw = g_state.powerMw;
    return 0;
}
int nvmlDeviceGetPowerManagementLimitConstraints(void*, unsigned* _min, unsigned* _max)
{
    *_min = 100000;
    *_max = 400000;
    return 0;
}
int nvmlDeviceGetPowerManagementDefaultLimit(void*, unsigned* _mw)
{
    *_mw = 300000;
    return 0;
}
int nvmlDeviceSetPowerManagementLimit(void*, unsigned _mw)
{
    if (denied())
        return denied();
    if (_mw < 100000 || _mw > 400000)
        return 2;  // NVML_ERROR_INVALID_ARGUMENT
    g_state.powerMw = _mw;
    log("power limit mW", _mw);
    return 0;
}

int nvmlDeviceSetGpuLockedClocks(void*, unsigned _min, unsigned _max)
{
    if (denied())
        return denied();
    g_state.lockedMin = _min;
    g_state.lockedMax = _max;
    log("locked core clock", _max);
    return 0;
}
int nvmlDeviceResetGpuLockedClocks(void*)
{
    if (denied())
        return denied();
    g_state.lockedMin = g_state.lockedMax = 0;
    log("locked core clock", 0);
    return 0;
}

struct ClockOffset
{
    unsigned version;
    int type, pstate, offset, minOffset, maxOffset;
};
int nvmlDeviceGetClockOffsets(void*, ClockOffset* _info)
{
    if (g_state.oldDriver)
        return 3;  // NVML_ERROR_NOT_SUPPORTED: the r470 functions are used
    if (_info->version != (sizeof(ClockOffset) | (1u << 24)) || _info->pstate != 0)
        return 2;
    bool mem = _info->type == 2;
    _info->offset = mem ? g_state.memOffset : g_state.coreOffset;
    _info->minOffset = mem ? -2000 : -1000;
    _info->maxOffset = mem ? 6000 : 1000;
    return 0;
}
int nvmlDeviceSetClockOffsets(void*, ClockOffset* _info)
{
    if (g_state.oldDriver)
        return 3;
    if (denied())
        return denied();
    if (_info->version != (sizeof(ClockOffset) | (1u << 24)) || _info->pstate != 0)
        return 2;
    (_info->type == 2 ? g_state.memOffset : g_state.coreOffset) = _info->offset;
    log(_info->type == 2 ? "mem offset" : "core offset", _info->offset);
    g_state.newOffsetCalls++;
    return 0;
}
int nvmlDeviceGetGpcClkVfOffset(void*, int* _offset)
{
    *_offset = g_state.coreOffset;
    return 0;
}
int nvmlDeviceGetMemClkVfOffset(void*, int* _offset)
{
    *_offset = g_state.memOffset;
    return 0;
}
int nvmlDeviceSetGpcClkVfOffset(void*, int _offset)
{
    if (denied())
        return denied();
    g_state.coreOffset = _offset;
    return 0;
}
int nvmlDeviceSetMemClkVfOffset(void*, int _offset)
{
    if (denied())
        return denied();
    g_state.memOffset = _offset;
    return 0;
}
int nvmlDeviceGetGpcClkMinMaxVfOffset(void*, int* _min, int* _max)
{
    *_min = -1000;
    *_max = 1000;
    return 0;
}
int nvmlDeviceGetMemClkMinMaxVfOffset(void*, int* _min, int* _max)
{
    *_min = -2000;
    *_max = 6000;
    return 0;
}

int nvmlDeviceGetNumFans(void*, unsigned* _fans)
{
    *_fans = 2;
    return 0;
}
int nvmlDeviceSetFanSpeed_v2(void*, unsigned _fan, unsigned _pct)
{
    if (denied())
        return denied();
    g_state.fanPct = _pct;
    log("fan", _pct);
    g_state.fanManual[_fan] = true;
    return 0;
}
int nvmlDeviceSetDefaultFanSpeed_v2(void*, unsigned _fan)
{
    if (denied())
        return denied();
    g_state.fanManual[_fan] = false;
    log("fan auto", _fan);
    return 0;
}
}
