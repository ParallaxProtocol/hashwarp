// Overclocking of one GPU: NVML (NVIDIA, Linux and Windows) or amdgpu sysfs (AMD, Linux).
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wrapamdsysfs.h"
#include "wrapnvml.h"

namespace dev
{
/// Values to set on a GPU; the unset ones are left as they are
struct OcSettings
{
    std::optional<unsigned> coreClock;  // MHz: locked core clock (NVIDIA), top core state (AMD)
    std::optional<int> coreOffset;      // MHz (NVIDIA)
    std::optional<int> memOffset;       // MHz, nvidia-settings units (NVIDIA)
    std::optional<unsigned> memClock;   // MHz: top memory state (AMD)
    std::optional<unsigned> powerW;     // power limit in watts
    std::optional<unsigned> powerPct;   // power limit in percent of the default
    std::optional<unsigned> fanPct;     // fan speed, c_fanAuto for automatic control

    static constexpr unsigned c_fanAuto = 101;

    bool empty() const
    {
        return !coreClock && !coreOffset && !memOffset && !memClock && !powerW && !powerPct &&
               !fanPct;
    }
    /// The values of _o override these
    void merge(const OcSettings& _o);
    /// e.g. "core 1500 MHz, mem +2000 MHz, power 300 W"
    std::string str() const;
};

/// What a GPU allows, read before anything is changed
struct OcLimits
{
    unsigned maxCoreClock = 0;              // MHz, 0 if unknown
    int minCoreOffset = 0, maxCoreOffset = 0;
    int minMemOffset = 0, maxMemOffset = 0;
    unsigned minMemClock = 0, maxMemClock = 0;
    unsigned minPowerW = 0, maxPowerW = 0, defaultPowerW = 0;
};

class GpuControl
{
public:
    /// _index: device index of the monitoring library
    static std::unique_ptr<GpuControl> nvml(wrap_nvml_handle* _h, int _index);
    static std::unique_ptr<GpuControl> amd(wrap_amdsysfs_handle* _h, int _index);

    virtual ~GpuControl() = default;

    /// Stable identifier of the GPU (NVIDIA UUID, AMD PCI address and unique_id)
    virtual std::string id() const = 0;
    const OcLimits& limits() const { return m_limits; }

    /// Sets the values of _s, returns a message for each one that couldn't be set.
    /// The first change of each value records the original one, which restore() puts back
    std::vector<std::string> apply(const OcSettings& _s);
    void restore();
    /// Whether apply() changed anything that restore() would undo
    bool changed() const;

protected:
    // Backends: 0 or an error code, described by error()
    virtual int setCoreClock(unsigned _mhz) = 0;  // 0 = default
    virtual int setCoreOffset(int _mhz) = 0;
    virtual int setMemOffset(int _mhz) = 0;
    virtual int setMemClock(unsigned _mhz) = 0;  // 0 = default
    virtual int setPower(unsigned _w) = 0;
    virtual int setFan(unsigned _pct) = 0;  // c_fanAuto = automatic
    virtual std::string error(int _code) const = 0;
    // Current values before the first change, nullopt if the backend can't tell
    virtual std::optional<int> currentCoreOffset() { return std::nullopt; }
    virtual std::optional<int> currentMemOffset() { return std::nullopt; }
    virtual std::optional<unsigned> currentPower() { return std::nullopt; }

    OcLimits m_limits;

private:
    // Original values to restore
    bool m_coreClockSet = false, m_memClockSet = false, m_fanSet = false;
    std::optional<int> m_origCoreOffset, m_origMemOffset;
    std::optional<unsigned> m_origPower;
};

}  // namespace dev
