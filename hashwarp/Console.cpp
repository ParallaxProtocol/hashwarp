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
        {1e12, "TH/s"}, {1e9, "GH/s"}, {1e6, "MH/s"}, {1e3, "kH/s"}, {1, "H/s"}};
    for (const auto& s : scales)
        if (_hashrate >= s.divisor * 0.9995)
            return s;
    return {1e6, "MH/s"};
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

// Characters shown, of UTF-8 text without colors
size_t width(const std::string& _s)
{
    size_t n = 0;
    bool skip = false;
    for (unsigned char c : _s)
    {
        if (c == '\x1b')
            skip = true;
        else if (skip)
            skip = c != 'm';
        else if ((c & 0xc0) != 0x80)
            n++;
    }
    return n;
}

std::string pad(std::string _s, size_t _width, bool _left = false)
{
    size_t w = width(_s);
    if (w >= _width)
        return _s;
    return _left ? _s + std::string(_width - w, ' ') : std::string(_width - w, ' ') + _s;
}

std::string sensors(const TelemetryAccountType& _m)
{
    std::string ret;
    if (_m.sensors.tempC)
        ret += " · " + std::to_string(_m.sensors.tempC) + "°C · fan " +
               std::to_string(_m.sensors.fanP) + "%";
    if (_m.sensors.powerW > 0)
        ret += " · " + fixed(_m.sensors.powerW, 0) + " W";
    if (_m.sensors.powerW > 0 && _m.hashrate > 0)
        ret += " · " + fixed(_m.hashrate / 1e3 / _m.sensors.powerW, 0) + " kH/J";
    return ret;
}
}  // namespace

std::vector<ReportLine> reportLines(const TelemetryType& _t, const PoolStatus& _pool, bool _perGpu)
{
    std::vector<ReportLine> lines;
    Scale sc = scaleFor(std::max<double>(_t.farm.hashrate, _t.farm.hashrate900));

    ReportLine farm;
    farm.source = "miner";
    std::ostringstream ss;
    if (!_pool.connected)
        ss << EthRed "not connected" EthReset " · ";
    ss << EthTealBold << fixed(_t.farm.hashrate / sc.divisor) << " " << sc.unit << EthReset
       << " · 60s " << fixed(_t.farm.hashrate60 / sc.divisor) << " · 15m "
       << fixed(_t.farm.hashrate900 / sc.divisor);
    if (_t.effectiveHashrate > 0)
        ss << " · eff " << fixed(_t.effectiveHashrate / sc.divisor);
    ss << " · " EthWhiteBold << shares(_t.farm.solutions) << EthReset;
    farm.fields = {{"hashrate", _t.farm.hashrate}, {"hashrate60", _t.farm.hashrate60},
        {"hashrate900", _t.farm.hashrate900}, {"accepted", _t.farm.solutions.accepted},
        {"rejected", _t.farm.solutions.rejected}, {"failed", _t.farm.solutions.failed}};
    if (_t.effectiveHashrate > 0)
        farm.fields.push_back({"effective", _t.effectiveHashrate});

    if (!_perGpu && _t.miners.size() == 1)
    {
        const auto& m = _t.miners[0];
        if (m.paused)
            ss << " · " EthYellow "paused" EthReset;
        else if (_t.hwmon)
            ss << sensors(m);
        farm.text = ss.str();
        lines.push_back(farm);
        return lines;
    }
    farm.text = ss.str();
    lines.push_back(farm);
    if (!_perGpu)
        return lines;

    for (size_t i = 0; i < _t.miners.size(); i++)
    {
        const auto& m = _t.miners[i];
        ReportLine gpu;
        gpu.source = (m.prefix == "cp" ? "cpu" : "gpu") + std::to_string(i);
        gpu.fields = {{"gpu", (double)i}, {"hashrate", m.hashrate}, {"paused", m.paused}};
        if (m.paused)
            gpu.text = EthYellow "paused" EthReset;
        else
        {
            gpu.text = fixed(m.hashrate / sc.divisor) + " " + sc.unit;
            if (_t.hwmon)
            {
                gpu.text += sensors(m);
                gpu.fields.push_back({"temp", m.sensors.tempC});
                gpu.fields.push_back({"fan", m.sensors.fanP});
                gpu.fields.push_back({"power", m.sensors.powerW});
            }
        }
        lines.push_back(gpu);
    }
    return lines;
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
    auto temp = [](int _c) { return _c ? std::to_string(_c) + "°C" : std::string("-"); };

    std::ostringstream h;
    h << EthWhiteBold << pad("GPU", 6, true) << pad("Name", 26, true) << pad(u, 10) << pad("60s", 9)
      << pad("15m", 9) << pad("Shares", 12);
    if (_t.hwmon)
    {
        h << pad("Temp", 6);
        if (showHot)
            h << pad("Hot", 6);
        if (showMem)
            h << pad("Mem", 6);
        h << pad("Fan", 5) << pad("Power", 7) << pad("kH/J", 7);
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
        l << pad((m.prefix == "cp" ? "cpu" : "gpu") + std::to_string(i), 6, true)
          << pad(name.substr(0, 25), 26, true);
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
        l << EthWhiteBold << pad("Total", 32, true)
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
