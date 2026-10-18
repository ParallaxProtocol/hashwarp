// Hashwarp overclocking settings: --oc-* options, profiles and tuning results.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Oc.h"

#include <json/json.h>

#include <boost/filesystem.hpp>

#include <fstream>
#include <set>
#include <stdexcept>

namespace hashwarp
{
namespace
{
const std::set<std::string> c_gpuProfiles = {"stock", "efficient", "tuned"};
const std::set<std::string> c_profileKeys = {
    "core-clock", "core-offset", "mem-offset", "mem-clock", "power", "fan"};

// The _gpu-th value of a per GPU list ("" if none or x)
std::string pick(const std::vector<std::string>& _values, size_t _gpu)
{
    std::string v;
    if (_values.size() == 1)
        v = _values[0];
    else if (_gpu < _values.size())
        v = _values[_gpu];
    return (v == "x" || v == "-") ? "" : v;
}

long number(const std::string& _value, const std::string& _what, long _min, long _max)
{
    size_t end = 0;
    long v = 0;
    try
    {
        v = std::stol(_value, &end);
    }
    catch (const std::exception&)
    {
        end = 0;
    }
    if (!end || end != _value.size() || v < _min || v > _max)
        throw std::invalid_argument(_what + " must be a number in [" + std::to_string(_min) +
                                    " .. " + std::to_string(_max) + "], not '" + _value + "'");
    return v;
}

// Sets the setting _key (an option name without oc-) of _s from _value
void set(dev::OcSettings& _s, const std::string& _key, const std::string& _value,
    const std::string& _what)
{
    if (_value.empty())
        return;
    if (_key == "core-clock")
    {
        long v = number(_value, _what, 0, 5000);
        if (v && v < 100)
            throw std::invalid_argument(_what + " must be 0 or at least 100 MHz");
        _s.coreClock = (unsigned)v;
    }
    else if (_key == "core-offset")
        _s.coreOffset = (int)number(_value, _what, -2000, 2000);
    else if (_key == "mem-offset")
        _s.memOffset = (int)number(_value, _what, -5000, 10000);
    else if (_key == "mem-clock")
    {
        long v = number(_value, _what, 0, 10000);
        if (v && v < 100)
            throw std::invalid_argument(_what + " must be 0 or at least 100 MHz");
        _s.memClock = (unsigned)v;
    }
    else if (_key == "power")
    {
        if (_value.back() == '%')
        {
            _s.powerPct = (unsigned)number(_value.substr(0, _value.size() - 1), _what, 10, 150);
            _s.powerW.reset();
        }
        else
        {
            _s.powerW = (unsigned)number(_value, _what, 10, 2000);
            _s.powerPct.reset();
        }
    }
    else if (_key == "fan")
        _s.fanPct = _value == "auto" ? dev::OcSettings::c_fanAuto :
                                       (unsigned)number(_value, _what, 0, 100);
}

std::string jsonText(const Json::Value& _v)
{
    if (_v.isString())
        return _v.asString();
    if (_v.isIntegral())
        return std::to_string(_v.asInt64());
    throw std::invalid_argument("must be a number or a string");
}

Json::Value parseProfiles(const std::string& _json)
{
    if (_json.empty())
        return Json::Value(Json::objectValue);
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(_json);
    Json::Value root;
    if (!Json::parseFromStream(builder, in, &root, &errors) || !root.isObject())
        throw std::invalid_argument("oc-profiles must be a JSON object of profiles");
    return root;
}

// A profile of oc-profiles
dev::OcSettings userProfile(const Json::Value& _profiles, const std::string& _name)
{
    if (!_profiles.isMember(_name))
        throw std::invalid_argument("Unknown overclocking profile '" + _name +
                                    "': not stock, efficient, tuned or one of oc-profiles");
    const Json::Value& p = _profiles[_name];
    if (!p.isObject())
        throw std::invalid_argument("Overclocking profile '" + _name + "' must be an object");
    dev::OcSettings s;
    for (const auto& key : p.getMemberNames())
    {
        std::string what = "oc-profiles." + _name + "." + key;
        if (!c_profileKeys.count(key))
            throw std::invalid_argument("Unknown setting " + what + " (core-clock, core-offset, "
                                                                    "mem-offset, mem-clock, power, fan)");
        try
        {
            set(s, key, jsonText(p[key]), what);
        }
        catch (const std::invalid_argument& ex)
        {
            std::string msg = ex.what();
            throw std::invalid_argument(msg.compare(0, what.size(), what) ? what + " " + msg : msg);
        }
    }
    return s;
}

std::string today()
{
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return buf;
}
}  // namespace

OcRequest ocRequest(const Options& _o, size_t _gpu)
{
    OcRequest r;
    std::string profile = pick(_o.ocProfile, _gpu);
    if (c_gpuProfiles.count(profile))
        r.gpuProfile = profile;
    else if (!profile.empty())
        r.settings = userProfile(parseProfiles(_o.ocProfiles), profile);

    dev::OcSettings opts;
    set(opts, "core-clock", pick(_o.ocCoreClock, _gpu), "--oc-core-clock");
    set(opts, "core-offset", pick(_o.ocCoreOffset, _gpu), "--oc-core-offset");
    set(opts, "mem-offset", pick(_o.ocMemOffset, _gpu), "--oc-mem-offset");
    set(opts, "mem-clock", pick(_o.ocMemClock, _gpu), "--oc-mem-clock");
    set(opts, "power", pick(_o.ocPower, _gpu), "--oc-power");
    set(opts, "fan", pick(_o.ocFan, _gpu), "--oc-fan");
    r.settings.merge(opts);
    return r;
}

void validateOc(const Options& _o)
{
    Json::Value profiles = parseProfiles(_o.ocProfiles);
    for (const auto& name : profiles.getMemberNames())
    {
        if (c_gpuProfiles.count(name))
            throw std::invalid_argument(
                "oc-profiles can't redefine the built-in profile '" + name + "'");
        userProfile(profiles, name);
    }
    size_t gpus = 1;
    for (const auto* list : {&_o.ocProfile, &_o.ocCoreClock, &_o.ocCoreOffset, &_o.ocMemOffset,
             &_o.ocMemClock, &_o.ocPower, &_o.ocFan})
        gpus = std::max(gpus, list->size());
    for (size_t i = 0; i < gpus; i++)
        ocRequest(_o, i);
}

TuneResults loadTuneResults(const std::string& _path)
{
    TuneResults results;
    std::ifstream in(_path);
    if (!in)
        return results;
    Json::CharReaderBuilder builder;
    std::string errors;
    Json::Value root;
    if (!Json::parseFromStream(builder, in, &root, &errors) || !root.isObject())
        throw std::runtime_error("Can't read the tuning results " + _path + ": " + errors);
    for (const auto& id : root.getMemberNames())
    {
        const Json::Value& g = root[id];
        TunedGpu t;
        t.name = g.get("name", "").asString();
        t.mode = g.get("mode", "all").asString();
        t.hashrate = g.get("hashrate", 0).asDouble();
        t.powerW = g.get("power-w", 0).asDouble();
        t.date = g.get("date", "").asString();
        try
        {
            for (const char* key : {"core-clock", "mem-offset", "mem-clock"})
                if (g.isMember(key))
                    set(t.settings, key, jsonText(g[key]), _path + ": " + id + "." + key);
        }
        catch (const std::invalid_argument& ex)
        {
            throw std::runtime_error(ex.what());
        }
        results[id] = t;
    }
    return results;
}

void saveTuneResults(const std::string& _path, const TuneResults& _results)
{
    Json::Value root(Json::objectValue);
    for (const auto& r : _results)
    {
        Json::Value g(Json::objectValue);
        const TunedGpu& t = r.second;
        g["name"] = t.name;
        g["mode"] = t.mode;
        if (t.settings.coreClock)
            g["core-clock"] = *t.settings.coreClock;
        if (t.settings.memOffset)
            g["mem-offset"] = *t.settings.memOffset;
        if (t.settings.memClock)
            g["mem-clock"] = *t.settings.memClock;
        g["hashrate"] = Json::Value(Json::UInt64(t.hashrate));
        g["power-w"] = Json::Value(int(t.powerW + 0.5));
        g["date"] = t.date.empty() ? today() : t.date;
        root[r.first] = g;
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "    ";
    std::ofstream out(_path);
    if (!out)
        throw std::runtime_error("Can't write " + _path);
    out << Json::writeString(builder, root) << "\n";
    if (!out)
        throw std::runtime_error("Can't write " + _path);
}

std::string tuneFilePath(const Options& _o)
{
    if (!_o.ocTuneFile.empty())
        return _o.ocTuneFile;
    std::string config = findDefaultConfig();
    if (config.empty())
        config = defaultConfigPath();
    return (boost::filesystem::path(config).parent_path() / "hashwarp-oc.json").string();
}

std::optional<dev::OcSettings> gpuProfile(const std::string& _profile, const TuneResults& _tuned,
    const std::string& _gpuId, const dev::OcLimits& _limits, std::string& _note)
{
    dev::OcSettings s;
    auto it = _tuned.find(_gpuId);
    if (_profile == "stock")
    {
        s.coreClock = 0;
        if (_limits.minMemOffset || _limits.maxMemOffset)
        {
            s.coreOffset = 0;
            s.memOffset = 0;
        }
        else
            s.memClock = 0;
        if (_limits.defaultPowerW)
            s.powerPct = 100;
        s.fanPct = dev::OcSettings::c_fanAuto;
        _note = "stock settings";
        return s;
    }
    if (_profile == "efficient")
    {
        if (it != _tuned.end() && it->second.settings.coreClock)
        {
            s.coreClock = it->second.settings.coreClock;
            _note = "tuned core clock of " + it->second.date;
        }
        else
        {
            s.powerPct = c_efficientPowerPct;
            _note = "not tuned: hashwarp --oc-tune=core finds the most efficient core clock";
        }
        return s;
    }
    // tuned
    if (it == _tuned.end())
    {
        _note = "not tuned yet, run hashwarp --oc-tune";
        return std::nullopt;
    }
    _note = "tuned on " + it->second.date;
    return it->second.settings;
}

}  // namespace hashwarp
