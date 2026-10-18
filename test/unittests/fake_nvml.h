// State of the fake NVML library of the unit tests (fake_nvml.cpp)

#pragma once

struct FakeNvmlState
{
    unsigned powerMw = 300000;
    unsigned lockedMin = 0, lockedMax = 0;  // 0: not locked
    int coreOffset = 0, memOffset = 0;
    unsigned fanPct = 40;
    bool fanManual[2] = {false, false};
    bool denyWrites = false;  // setters fail like for a user without root
    bool oldDriver = false;   // no nvmlDeviceSetClockOffsets (before r555)
    unsigned newOffsetCalls = 0;
};

extern "C" FakeNvmlState* fake_nvml_state();
