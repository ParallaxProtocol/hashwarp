// Overclocking of one GPU: NVML (NVIDIA, Linux and Windows) or amdgpu sysfs (AMD, Linux).
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "GpuControl.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <boost/algorithm/string/trim.hpp>

namespace dev
{
void OcSettings::merge(const OcSettings& _o)
{
    if (_o.coreClock)
        coreClock = _o.coreClock;
    if (_o.coreOffset)
        coreOffset = _o.coreOffset;
    if (_o.memOffset)
        memOffset = _o.memOffset;
    if (_o.memClock)
        memClock = _o.memClock;
    // One power limit: the overriding one, in either unit
    if (_o.powerW || _o.powerPct)
    {
        powerW = _o.powerW;
        powerPct = _o.powerPct;
    }
    if (_o.fanPct)
        fanPct = _o.fanPct;
}

std::string OcSettings::str() const
{
    std::ostringstream ss;
    auto sep = [&]() -> std::ostream& {
        if (ss.tellp())
            ss << ", ";
        return ss;
    };
    auto offset = [](int _v) { return (_v >= 0 ? "+" : "") + std::to_string(_v) + " MHz"; };
    auto clock = [](unsigned _v) { return _v ? std::to_string(_v) + " MHz" : "default"; };
    if (coreClock)
        sep() << "core " << clock(*coreClock);
    if (coreOffset)
        sep() << "core " << offset(*coreOffset);
    if (memClock)
        sep() << "mem " << clock(*memClock);
    if (memOffset)
        sep() << "mem " << offset(*memOffset);
    if (powerW)
        sep() << "power " << *powerW << " W";
    if (powerPct)
        sep() << "power " << *powerPct << "%";
    if (fanPct)
        sep() << "fan " << (*fanPct == c_fanAuto ? std::string("auto") : std::to_string(*fanPct) + "%");
    return ss.str();
}

std::vector<std::string> GpuControl::apply(const OcSettings& _s)
{
    std::vector<std::string> errors;
    auto check = [&](int _code, const std::string& _what) {
        if (_code)
            errors.push_back("can't set " + _what + ": " + error(_code));
        return !_code;
    };

    if (_s.powerW || _s.powerPct)
    {
        unsigned w = _s.powerW ? *_s.powerW : m_limits.defaultPowerW * *_s.powerPct / 100;
        if (!_s.powerW && !m_limits.defaultPowerW)
            errors.push_back("can't set the power limit in percent: the default limit is unknown");
        else if (m_limits.maxPowerW && (w < m_limits.minPowerW || w > m_limits.maxPowerW))
            errors.push_back("power limit " + std::to_string(w) + " W is out of the GPU's range " +
                             std::to_string(m_limits.minPowerW) + " - " +
                             std::to_string(m_limits.maxPowerW) + " W");
        else
        {
            auto orig = currentPower();
            if (check(setPower(w), "the power limit") && !m_origPower)
                m_origPower = orig;
        }
    }
    if (_s.coreClock)
    {
        if (check(setCoreClock(*_s.coreClock), "the core clock"))
            m_coreClockSet = *_s.coreClock != 0;
    }
    if (_s.coreOffset)
    {
        auto orig = currentCoreOffset();
        if (check(setCoreOffset(*_s.coreOffset), "the core clock offset") && !m_origCoreOffset)
            m_origCoreOffset = orig;
    }
    if (_s.memClock)
    {
        if (check(setMemClock(*_s.memClock), "the memory clock"))
            m_memClockSet = *_s.memClock != 0;
    }
    if (_s.memOffset)
    {
        auto orig = currentMemOffset();
        if (check(setMemOffset(*_s.memOffset), "the memory clock offset") && !m_origMemOffset)
            m_origMemOffset = orig;
    }
    if (_s.fanPct)
    {
        if (check(setFan(*_s.fanPct), "the fan speed"))
            m_fanSet = *_s.fanPct != OcSettings::c_fanAuto;
    }
    return errors;
}

bool GpuControl::changed() const
{
    return m_coreClockSet || m_memClockSet || m_fanSet || m_origCoreOffset || m_origMemOffset ||
           m_origPower;
}

void GpuControl::keep()
{
    m_fanSet = m_memClockSet = m_coreClockSet = false;
    m_origMemOffset.reset();
    m_origCoreOffset.reset();
    m_origPower.reset();
}

void GpuControl::restore()
{
    if (m_fanSet)
        setFan(OcSettings::c_fanAuto);
    if (m_origMemOffset)
        setMemOffset(*m_origMemOffset);
    if (m_memClockSet)
        setMemClock(0);
    if (m_origCoreOffset)
        setCoreOffset(*m_origCoreOffset);
    if (m_coreClockSet)
        setCoreClock(0);
    if (m_origPower)
        setPower(*m_origPower);
    keep();
}

namespace
{
class NvmlControl : public GpuControl
{
public:
    NvmlControl(wrap_nvml_handle* _h, int _index) : m_h(_h), m_index(_index)
    {
        unsigned mhz = 0;
        if (wrap_nvml_get_max_clock(m_h, m_index, 0, &mhz) == 0)
            m_limits.maxCoreClock = mhz;
        int cur = 0;
        wrap_nvml_get_clock_offset(
            m_h, m_index, 0, &cur, &m_limits.minCoreOffset, &m_limits.maxCoreOffset);
        wrap_nvml_get_clock_offset(
            m_h, m_index, 1, &cur, &m_limits.minMemOffset, &m_limits.maxMemOffset);
        unsigned now = 0, lo = 0, hi = 0, def = 0;
        if (wrap_nvml_get_power_limits(m_h, m_index, &now, &lo, &hi, &def) == 0)
        {
            m_limits.minPowerW = lo / 1000;
            m_limits.maxPowerW = hi / 1000;
            m_limits.defaultPowerW = def / 1000;
        }
    }

    std::string id() const override
    {
        char uuid[96] = {0};
        if (wrap_nvml_get_uuid(m_h, m_index, uuid, sizeof(uuid)) == 0)
            return uuid;
        return "nvml-" + std::to_string(m_index);
    }

protected:
    int setCoreClock(unsigned _mhz) override
    {
        return wrap_nvml_lock_core_clock(m_h, m_index, _mhz);
    }
    int setCoreOffset(int _mhz) override { return wrap_nvml_set_clock_offset(m_h, m_index, 0, _mhz); }
    int setMemOffset(int _mhz) override { return wrap_nvml_set_clock_offset(m_h, m_index, 1, _mhz); }
    int setMemClock(unsigned) override { return c_noMemClock; }
    int setPower(unsigned _w) override { return wrap_nvml_set_power_limit(m_h, m_index, _w * 1000); }
    int setFan(unsigned _pct) override { return wrap_nvml_set_fans(m_h, m_index, _pct); }
    std::string error(int _code) const override
    {
        if (_code == c_noMemClock)
            return "not available on NVIDIA GPUs, use --oc-mem-offset";
        return wrap_nvml_error(m_h, _code);
    }
    std::optional<int> currentCoreOffset() override { return offset(0); }
    std::optional<int> currentMemOffset() override { return offset(1); }
    std::optional<unsigned> currentPower() override
    {
        unsigned now = 0, lo = 0, hi = 0, def = 0;
        if (wrap_nvml_get_power_limits(m_h, m_index, &now, &lo, &hi, &def) != 0)
            return std::nullopt;
        return now / 1000;
    }

private:
    static const int c_noMemClock = 1000;

    std::optional<int> offset(int _mem)
    {
        int cur = 0, lo = 0, hi = 0;
        if (wrap_nvml_get_clock_offset(m_h, m_index, _mem, &cur, &lo, &hi) != 0)
            return std::nullopt;
        return cur;
    }

    wrap_nvml_handle* m_h;
    int m_index;
};

class AmdControl : public GpuControl
{
public:
    AmdControl(wrap_amdsysfs_handle* _h, int _index) : m_h(_h), m_index(_index)
    {
        wrap_amdsysfs_od od;
        if (wrap_amdsysfs_get_od(m_h, m_index, &od) == 0)
        {
            m_limits.maxCoreClock = od.coreMaxMHz;
            m_limits.minMemClock = od.memMinMHz;
            m_limits.maxMemClock = od.memMaxMHz;
            m_limits.coreStateClock = od.coreMHz;
            m_limits.memStateClock = od.memMHz;
        }
        unsigned now = 0, lo = 0, hi = 0, def = 0;
        if (wrap_amdsysfs_get_power_caps(m_h, m_index, &now, &lo, &hi, &def) == 0)
        {
            m_limits.minPowerW = lo / 1000;
            m_limits.maxPowerW = hi / 1000;
            m_limits.defaultPowerW = def ? def / 1000 : now / 1000;
        }
    }

    std::string id() const override
    {
        char pci[32];
        snprintf(pci, sizeof(pci), "%04x:%02x:%02x", m_h->sysfs_pci_domain_id[m_index],
            m_h->sysfs_pci_bus_id[m_index], m_h->sysfs_pci_device_id[m_index]);
        // unique_id (Vega and newer) follows the GPU when it moves to another slot
        std::ifstream ifs(wrap_amdsysfs_root() + "/sys/class/drm/card" +
                          std::to_string(m_h->sysfs_device_id[m_index]) + "/device/unique_id");
        std::string unique;
        std::getline(ifs, unique);
        boost::algorithm::trim(unique);
        return unique.empty() ? std::string("amd-") + pci : "amd-" + unique;
    }

protected:
    int setCoreClock(unsigned _mhz) override
    {
        return _mhz ? wrap_amdsysfs_set_od_clock(m_h, m_index, 0, _mhz) :
                      wrap_amdsysfs_reset_od(m_h, m_index);
    }
    int setCoreOffset(int) override { return c_noOffset; }
    int setMemOffset(int) override { return c_noOffset; }
    int setMemClock(unsigned _mhz) override
    {
        return _mhz ? wrap_amdsysfs_set_od_clock(m_h, m_index, 1, _mhz) :
                      wrap_amdsysfs_reset_od(m_h, m_index);
    }
    int setPower(unsigned _w) override { return wrap_amdsysfs_set_power_cap(m_h, m_index, _w * 1000); }
    int setFan(unsigned _pct) override { return wrap_amdsysfs_set_fan(m_h, m_index, _pct); }
    std::string error(int _code) const override
    {
        if (_code == c_noOffset)
            return "not available on AMD GPUs, use --oc-core-clock and --oc-mem-clock";
        if (_code == EACCES || _code == EPERM)
            return "needs root (run hashwarp with sudo)";
        if (_code == ENOENT)
            return "overdrive is off, add amdgpu.ppfeaturemask=0xffffffff to the kernel command "
                   "line";
        if (_code == EINVAL)
            return "the driver refused the value";
        return _code > 0 ? strerror(_code) : "unknown error";
    }
    std::optional<unsigned> currentPower() override
    {
        unsigned now = 0, lo = 0, hi = 0, def = 0;
        if (wrap_amdsysfs_get_power_caps(m_h, m_index, &now, &lo, &hi, &def) != 0)
            return std::nullopt;
        return now / 1000;
    }

private:
    static const int c_noOffset = 1000;

    wrap_amdsysfs_handle* m_h;
    int m_index;
};
}  // namespace

std::unique_ptr<GpuControl> GpuControl::nvml(wrap_nvml_handle* _h, int _index)
{
    return std::unique_ptr<GpuControl>(new NvmlControl(_h, _index));
}

std::unique_ptr<GpuControl> GpuControl::amd(wrap_amdsysfs_handle* _h, int _index)
{
    return std::unique_ptr<GpuControl>(new AmdControl(_h, _index));
}

}  // namespace dev
