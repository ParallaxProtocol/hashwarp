// Hashwarp console output: periodic summary line and statistics table.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Console.h"

#include <libdevcore/CommonData.h>
#include <libdevcore/Terminal.h>

#include <iomanip>
#include <sstream>

using namespace dev;
using namespace dev::eth;

namespace hashwarp
{
namespace
{
// Blocks of an XHash epoch and Parallax's target block time
const int c_epochLength = 720;
const int c_blockSeconds = 600;

struct Scale
{
    double divisor;
    const char* unit;
};

// One unit for a whole line or table, picked from the largest value
Scale scaleFor(double _hashrate)
{
    static const Scale scales[] = {
        {1e12, "Th/s"}, {1e9, "Gh/s"}, {1e6, "Mh/s"}, {1e3, "Kh/s"}, {1, "h/s"}};
    for (const auto& s : scales)
        if (_hashrate >= s.divisor * 0.9995)
            return s;
    return {1e6, "Mh/s"};
}

std::string fixed(double _v, int _precision = 2)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(_precision) << _v;
    return ss.str();
}

std::string uptime(std::chrono::steady_clock::time_point _start)
{
    auto s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _start)
                 .count();
    std::ostringstream ss;
    if (s >= 86400)
        ss << s / 86400 << "d ";
    ss << (s % 86400) / 3600 << ":" << std::setw(2) << std::setfill('0') << (s % 3600) / 60;
    return ss.str();
}

std::string shares(const SolutionAccountType& _s)
{
    std::string ret = "A" + std::to_string(_s.accepted);
    if (_s.rejected)
        ret += " R" + std::to_string(_s.rejected);
    if (_s.failed)
        ret += " F" + std::to_string(_s.failed);
    if (_s.wasted)
        ret += " W" + std::to_string(_s.wasted);
    return ret;
}

std::string pad(std::string _s, size_t _width, bool _left = false)
{
    if (_s.size() >= _width)
        return _s;
    return _left ? _s + std::string(_width - _s.size(), ' ') :
                   std::string(_width - _s.size(), ' ') + _s;
}
}  // namespace

std::string summaryLine(const TelemetryType& _t, const PoolStatus& _pool)
{
    std::ostringstream ss;
    if (!_pool.connected)
        ss << EthRed "Not connected" EthReset " ";

    Scale sc = scaleFor(std::max<double>(_t.farm.hashrate, _t.farm.hashrate900));
    ss << EthTealBold << fixed(_t.farm.hashrate / sc.divisor) << " " << sc.unit << EthReset
       << " (60s " << fixed(_t.farm.hashrate60 / sc.divisor) << ", 15m "
       << fixed(_t.farm.hashrate900 / sc.divisor) << ")";
    if (_t.effectiveHashrate > 0)
        ss << " eff " << fixed(_t.effectiveHashrate / sc.divisor);
    ss << " " EthWhiteBold << shares(_t.farm.solutions) << EthReset;

    for (size_t i = 0; i < _t.miners.size(); i++)
    {
        const auto& m = _t.miners[i];
        ss << (i ? ", " : " - ") << m.prefix << i << " "
           << (m.paused ? EthYellow "paused" EthReset : fixed(m.hashrate / sc.divisor));
        if (_t.hwmon && m.sensors.tempC)
            ss << " " << m.sensors.tempC << "C";
    }
    return ss.str();
}

std::vector<std::string> statsTable(
    const TelemetryType& _t, const std::vector<GpuStatus>& _gpus, const PoolStatus& _pool)
{
    std::vector<std::string> lines;
    Scale sc = scaleFor(std::max<double>(_t.farm.hashrate, _t.farm.hashrate900));
    std::string u = sc.unit;

    // Optional columns, shown when any GPU reports them
    bool showHot = false, showMem = false, showClocks = false;
    for (const auto& m : _t.miners)
    {
        showHot = showHot || m.sensors.hotspotC;
        showMem = showMem || m.sensors.memTempC;
        showClocks = showClocks || m.sensors.coreMHz;
    }
    auto temp = [](int _c) { return _c ? std::to_string(_c) + "C" : std::string("-"); };

    std::ostringstream h;
    h << EthWhiteBold << pad("GPU", 4, true) << pad("Name", 26, true) << pad(u, 10) << pad("60s", 9)
      << pad("15m", 9) << pad("Shares", 12);
    if (_t.hwmon)
    {
        h << pad("Temp", 6);
        if (showHot)
            h << pad("Hot", 6);
        if (showMem)
            h << pad("Mem", 6);
        h << pad("Fan", 5) << pad("Power", 7) << pad("kh/W", 7);
        if (showClocks)
            h << pad("Clocks", 12);
    }
    h << EthReset;
    lines.push_back(h.str());

    double totalPower = 0;
    for (size_t i = 0; i < _t.miners.size(); i++)
    {
        const auto& m = _t.miners[i];
        std::string name = i < _gpus.size() ? _gpus[i].name : "";
        std::ostringstream l;
        l << pad(m.prefix + std::to_string(i), 4, true) << pad(name.substr(0, 25), 26, true);
        if (m.paused)
        {
            std::string reason = i < _gpus.size() ? _gpus[i].pauseReason : "";
            l << EthYellow << pad("paused", 10) << EthReset << pad("", 18) << "  " << reason;
            lines.push_back(l.str());
            continue;
        }
        l << pad(fixed(m.hashrate / sc.divisor), 10) << pad(fixed(m.hashrate60 / sc.divisor), 9)
          << pad(fixed(m.hashrate900 / sc.divisor), 9) << pad(shares(m.solutions), 12);
        if (_t.hwmon)
        {
            l << pad(temp(m.sensors.tempC), 6);
            if (showHot)
                l << pad(temp(m.sensors.hotspotC), 6);
            if (showMem)
                l << pad(temp(m.sensors.memTempC), 6);
            l << pad(m.sensors.tempC ? std::to_string(m.sensors.fanP) + "%" : "-", 5)
              << pad(m.sensors.powerW > 0 ? fixed(m.sensors.powerW, 0) + "W" : "-", 7)
              << pad(m.sensors.powerW > 0 ? fixed(m.hashrate / 1e3 / m.sensors.powerW, 0) : "-",
                     7);
            if (showClocks)
                l << pad(m.sensors.coreMHz ? std::to_string(m.sensors.coreMHz) + "/" +
                                                 std::to_string(m.sensors.memMHz) :
                                             "-",
                    12);
            totalPower += m.sensors.powerW;
        }
        lines.push_back(l.str());
    }

    if (_t.miners.size() > 1)
    {
        std::ostringstream l;
        l << EthWhiteBold << pad("Total", 30, true)
          << pad(fixed(_t.farm.hashrate / sc.divisor), 10)
          << pad(fixed(_t.farm.hashrate60 / sc.divisor), 9)
          << pad(fixed(_t.farm.hashrate900 / sc.divisor), 9) << pad(shares(_t.farm.solutions), 12);
        if (_t.hwmon && totalPower > 0)
            l << pad("", 11 + (showHot ? 6 : 0) + (showMem ? 6 : 0))
              << pad(fixed(totalPower, 0) + "W", 7)
              << pad(fixed(_t.farm.hashrate / 1e3 / totalPower, 0), 7);
        l << EthReset;
        lines.push_back(l.str());
    }

    std::ostringstream f;
    if (_pool.connected)
        f << "Pool " << _pool.host << " diff " << dev::getFormattedHashes(_pool.difficulty);
    else
        f << EthRed "Not connected" EthReset;
    if (_t.effectiveHashrate > 0)
        f << ", effective " << fixed(_t.effectiveHashrate / sc.divisor) << " " << u;
    f << ", up " << uptime(_t.start);
    if (_pool.epoch >= 0)
    {
        f << ", epoch " << _pool.epoch;
        if (_pool.block >= 0)
        {
            int left = c_epochLength - _pool.block % c_epochLength;
            f << " (next in " << left << " blocks, ~" << fixed(left * c_blockSeconds / 3600.0, 0)
              << " h)";
        }
    }
    lines.push_back(f.str());
    return lines;
}

}  // namespace hashwarp
