/*
 * Wrapper for AMD SysFS on linux, using adapted code from amdcovc by matszpk
 *
 * By Philipp Andreas - github@smurfy.de
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#if defined(__linux)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cerrno>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <string>

#include "wrapamdsysfs.h"
#include "wraphelper.h"

std::string wrap_amdsysfs_root()
{
    const char* root = getenv("HASHWARP_SYSFS_ROOT");
    return root ? root : "";
}

static bool getFileContentValue(const char* filename, unsigned int& value)
{
    value = 0;
    std::ifstream ifs(filename, std::ios::binary);
    std::string line;
    std::getline(ifs, line);
    char* p = (char*)line.c_str();
    char* p2;
    errno = 0;
    value = strtoul(p, &p2, 0);
    if (errno != 0)
        return false;
    return (p != p2);
}

wrap_amdsysfs_handle* wrap_amdsysfs_create()
{
    wrap_amdsysfs_handle* sysfsh = nullptr;

#if defined(__linux)
    namespace fs = boost::filesystem;
    std::vector<pciInfo> devices;  // Used to collect devices

    char dbuf[512];
    // Check directory exist
    const std::string root = wrap_amdsysfs_root();
    fs::path drm_dir(root + "/sys/class/drm");
    if (!fs::exists(drm_dir) || !fs::is_directory(drm_dir))
        return nullptr;

    // Regex patterns to identify directory elements
    std::regex cardPattern("^card[0-9]{1,}$");
    std::regex hwmonPattern("^hwmon[0-9]{1,}$");

    // Loop directory contents
    for (fs::directory_iterator dirEnt(drm_dir); dirEnt != fs::directory_iterator(); ++dirEnt)
    {
        // Skip non relevant entries
        if (!fs::is_directory(dirEnt->path()) ||
            !std::regex_match(dirEnt->path().filename().string(), cardPattern))
            continue;

        std::string devName = dirEnt->path().filename().string();
        unsigned int devIndex = std::stoi(devName.substr(4), nullptr, 10);
        unsigned int vendorId = 0;
        unsigned int hwmonIndex = UINT_MAX;

        // Get AMD cards only (vendor 4098)
        fs::path vendor_file(root + "/sys/class/drm/" + devName + "/device/vendor");
        snprintf(dbuf, sizeof(dbuf), "%s/sys/class/drm/%s/device/vendor", root.c_str(),
            devName.c_str());
        if (!fs::exists(vendor_file) || !fs::is_regular_file(vendor_file) ||
            !getFileContentValue(dbuf, vendorId) || vendorId != 4098)
            continue;

        // Check it has dependant hwmon directory
        fs::path hwmon_dir(root + "/sys/class/drm/" + devName + "/device/hwmon");
        if (!fs::exists(hwmon_dir) || !fs::is_directory(hwmon_dir))
            continue;

        // Loop subelements in hwmon directory
        for (fs::directory_iterator hwmonEnt(hwmon_dir); hwmonEnt != fs::directory_iterator();
             ++hwmonEnt)
        {
            // Skip non relevant entries
            if (!fs::is_directory(hwmonEnt->path()) ||
                !std::regex_match(hwmonEnt->path().filename().string(), hwmonPattern))
                continue;

            unsigned int v = std::stoi(hwmonEnt->path().filename().string().substr(5), nullptr, 10);
            hwmonIndex = std::min(hwmonIndex, v);
        }
        if (hwmonIndex == UINT_MAX)
            continue;

        // Detect Pci Id
        fs::path uevent_file(root + "/sys/class/drm/" + devName + "/device/uevent");
        if (!fs::exists(uevent_file) || !fs::is_regular_file(uevent_file))
            continue;

        snprintf(dbuf, sizeof(dbuf), "%s/sys/class/drm/card%d/device/uevent", root.c_str(),
            devIndex);
        std::ifstream ifs(dbuf, std::ios::binary);
        std::string line;
        int PciDomain = -1, PciBus = -1, PciDevice = -1, PciFunction = -1;
        while (std::getline(ifs, line))
        {
            if (line.length() > 24 && line.substr(0, 13) == "PCI_SLOT_NAME")
            {
                std::string pciId = line.substr(14);
                std::vector<std::string> pciIdParts;
                boost::split(pciIdParts, pciId, [](char c) { return (c == ':' || c == '.'); });

                try
                {
                    PciDomain = std::stoi(pciIdParts.at(0), nullptr, 16);
                    PciBus = std::stoi(pciIdParts.at(1), nullptr, 16);
                    PciDevice = std::stoi(pciIdParts.at(2), nullptr, 16);
                    PciFunction = std::stoi(pciIdParts.at(3), nullptr, 16);
                }
                catch (const std::exception&)
                {
                    PciDomain = PciBus = PciDevice = PciFunction = -1;
                }
                break;
            }
        }

        // If we got an error skip
        if (PciDomain == -1)
            continue;

        // We got all information needed
        // push in the list of collected devices
        pciInfo pInfo = pciInfo();
        pInfo.DeviceId = devIndex;
        pInfo.HwMonId = hwmonIndex;
        pInfo.PciDomain = PciDomain;
        pInfo.PciBus = PciBus;
        pInfo.PciDevice = PciDevice;
        devices.push_back(pInfo);
    }

    // Nothing collected - exit
    if (!devices.size())
    {
        cwarn << "Failed to obtain all required AMD file pointers";
        cwarn << "AMD hardware monitoring disabled";
        return nullptr;
    }

    unsigned int gpucount = devices.size();
    sysfsh = (wrap_amdsysfs_handle*)calloc(1, sizeof(wrap_amdsysfs_handle));
    if (sysfsh == nullptr)
    {
        cwarn << "Failed allocate memory";
        cwarn << "AMD hardware monitoring disabled";
        return sysfsh;
    }
    sysfsh->sysfs_gpucount = gpucount;
    sysfsh->sysfs_device_id = (unsigned int*)calloc(gpucount, sizeof(unsigned int));
    sysfsh->sysfs_hwmon_id = (unsigned int*)calloc(gpucount, sizeof(unsigned int));
    sysfsh->sysfs_pci_domain_id = (unsigned int*)calloc(gpucount, sizeof(unsigned int));
    sysfsh->sysfs_pci_bus_id = (unsigned int*)calloc(gpucount, sizeof(unsigned int));
    sysfsh->sysfs_pci_device_id = (unsigned int*)calloc(gpucount, sizeof(unsigned int));

    gpucount = 0;
    for (auto const& device : devices)
    {
        sysfsh->sysfs_device_id[gpucount] = device.DeviceId;
        sysfsh->sysfs_hwmon_id[gpucount] = device.HwMonId;
        sysfsh->sysfs_pci_domain_id[gpucount] = device.PciDomain;
        sysfsh->sysfs_pci_bus_id[gpucount] = device.PciBus;
        sysfsh->sysfs_pci_device_id[gpucount] = device.PciDevice;
        gpucount++;
    }

#endif
    return sysfsh;
}

int wrap_amdsysfs_destroy(wrap_amdsysfs_handle* sysfsh)
{
    free(sysfsh);
    return 0;
}

int wrap_amdsysfs_get_gpucount(wrap_amdsysfs_handle* sysfsh, int* gpucount)
{
    *gpucount = sysfsh->sysfs_gpucount;
    return 0;
}

namespace
{
// Path of a file in the hwmon directory of a GPU
std::string hwmonFile(wrap_amdsysfs_handle* sysfsh, int index, const char* file)
{
    return wrap_amdsysfs_root() + "/sys/class/drm/card" +
           std::to_string(sysfsh->sysfs_device_id[index]) + "/device/hwmon/hwmon" +
           std::to_string(sysfsh->sysfs_hwmon_id[index]) + "/" + file;
}

bool readHwmon(wrap_amdsysfs_handle* sysfsh, int index, const char* file, unsigned int& value)
{
    return getFileContentValue(hwmonFile(sysfsh, index, file).c_str(), value);
}

std::string readLine(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    std::string line;
    std::getline(ifs, line);
    boost::algorithm::trim(line);
    return line;
}

// Temperature (C) of the hwmon sensor labelled _label ("edge", "junction",
// "mem"), or of temp<_fallback> on kernels without labels
unsigned int labelledTempC(wrap_amdsysfs_handle* sysfsh, int index, const char* _label, int _fallback)
{
    bool anyLabel = false;
    for (int i = 1; i <= 3; i++)
    {
        std::string label =
            readLine(hwmonFile(sysfsh, index, ("temp" + std::to_string(i) + "_label").c_str()));
        anyLabel = anyLabel || !label.empty();
        if (label == _label)
        {
            unsigned int milli = 0;
            readHwmon(sysfsh, index, ("temp" + std::to_string(i) + "_input").c_str(), milli);
            return milli / 1000;
        }
    }
    if (anyLabel)
        return 0;
    unsigned int milli = 0;
    readHwmon(sysfsh, index, ("temp" + std::to_string(_fallback) + "_input").c_str(), milli);
    return milli / 1000;
}

bool validIndex(wrap_amdsysfs_handle* sysfsh, int index)
{
    return index >= 0 && index < sysfsh->sysfs_gpucount;
}
}  // namespace

int wrap_amdsysfs_get_tempC(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* tempC)
{
    if (!validIndex(sysfsh, index))
        return -1;
    unsigned int t = labelledTempC(sysfsh, index, "edge", 1);
    if (t > 0)
        *tempC = t;
    return 0;
}

int wrap_amdsysfs_get_extra_tempsC(
    wrap_amdsysfs_handle* sysfsh, int index, unsigned int* junctionC, unsigned int* memC)
{
    if (!validIndex(sysfsh, index))
        return -1;
    *junctionC = labelledTempC(sysfsh, index, "junction", 2);
    *memC = labelledTempC(sysfsh, index, "mem", 3);
    return 0;
}

int wrap_amdsysfs_get_fanpcnt(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* fanpcnt)
{
    if (!validIndex(sysfsh, index))
        return -1;

    unsigned int pwm = 0, pwmMax = 255, pwmMin = 0;
    if (readHwmon(sysfsh, index, "pwm1", pwm))
    {
        readHwmon(sysfsh, index, "pwm1_max", pwmMax);
        readHwmon(sysfsh, index, "pwm1_min", pwmMin);
        if (pwmMax > pwmMin)
        {
            pwm = std::min(std::max(pwm, pwmMin), pwmMax);
            *fanpcnt = (unsigned int)(double(pwm - pwmMin) / double(pwmMax - pwmMin) * 100.0);
            return 0;
        }
    }

    // GPUs whose firmware controls the fan only expose its speed
    unsigned int rpm = 0, rpmMax = 0;
    if (readHwmon(sysfsh, index, "fan1_input", rpm) && readHwmon(sysfsh, index, "fan1_max", rpmMax) &&
        rpmMax > 0)
    {
        *fanpcnt = std::min(100u, (unsigned int)(double(rpm) / double(rpmMax) * 100.0));
        return 0;
    }
    return -1;
}

int wrap_amdsysfs_get_power_usage(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* milliwatts)
{
    if (!validIndex(sysfsh, index))
        return -1;

    // hwmon reports microwatts: power1_average on most GPUs, power1_input on newer ones
    unsigned int micro = 0;
    if ((readHwmon(sysfsh, index, "power1_average", micro) && micro) ||
        (readHwmon(sysfsh, index, "power1_input", micro) && micro))
    {
        *milliwatts = micro / 1000;
        return 0;
    }

    // Old kernels: debugfs (needs root)
    try
    {
        char dbuf[512];
        snprintf(dbuf, sizeof(dbuf), "%s/sys/kernel/debug/dri/%d/amdgpu_pm_info",
            wrap_amdsysfs_root().c_str(), sysfsh->sysfs_device_id[index]);
        std::ifstream ifs(dbuf, std::ios::binary);
        std::string line;
        static const std::regex regex(R"(([\d|\.]+) W \(average GPU\))");
        while (std::getline(ifs, line))
        {
            std::smatch sm;
            if (std::regex_search(line, sm, regex) && sm.size() == 2)
            {
                double watt = atof(sm.str(1).c_str());
                *milliwatts = (unsigned int)(watt * 1000);
                return 0;
            }
        }
    }
    catch (const std::exception& ex)
    {
        cwarn << "Error in amdsysfs_get_power_usage: " << ex.what();
    }
    return -1;
}

int wrap_amdsysfs_get_clocks(
    wrap_amdsysfs_handle* sysfsh, int index, unsigned int* coreMHz, unsigned int* memMHz)
{
    if (!validIndex(sysfsh, index))
        return -1;
    unsigned int hz = 0;
    *coreMHz = readHwmon(sysfsh, index, "freq1_input", hz) ? hz / 1000000 : 0;
    *memMHz = readHwmon(sysfsh, index, "freq2_input", hz) ? hz / 1000000 : 0;
    return 0;
}

int wrap_amdsysfs_get_power_limit(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* milliwatts)
{
    if (!validIndex(sysfsh, index))
        return -1;
    unsigned int micro = 0;
    if (!readHwmon(sysfsh, index, "power1_cap", micro) || !micro)
        return -1;
    *milliwatts = micro / 1000;
    return 0;
}

namespace
{
std::string deviceFile(wrap_amdsysfs_handle* sysfsh, int index, const char* file)
{
    return wrap_amdsysfs_root() + "/sys/class/drm/card" +
           std::to_string(sysfsh->sysfs_device_id[index]) + "/device/" + file;
}

// Writes _value like `echo _value > _path`: 0 or the errno of the write
int writeFile(const std::string& _path, const std::string& _value, bool _append = false)
{
#if defined(__linux)
    int fd = open(_path.c_str(), O_WRONLY | (_append ? O_APPEND : O_TRUNC));
    if (fd < 0)
        return errno;
    std::string line = _value + "\n";
    ssize_t n = write(fd, line.data(), line.size());
    int err = n == (ssize_t)line.size() ? 0 : (n < 0 ? errno : EIO);
    if (close(fd) != 0 && !err)
        err = errno;
    return err;
#else
    (void)_path;
    (void)_value;
    (void)_append;
    return ENOSYS;
#endif
}

// The commands go to pp_od_clk_voltage one by one (appended: the fake trees of the tests
// keep them)
int writeOd(wrap_amdsysfs_handle* sysfsh, int index, const std::string& _command)
{
    return writeFile(deviceFile(sysfsh, index, "pp_od_clk_voltage"), _command, true);
}

// Highest state of an OD_SCLK or OD_MCLK section: index, MHz and, on GPUs that take
// a voltage with the clock (Polaris, Vega), mV
struct OdState
{
    int index = -1;
    unsigned int mhz = 0;
    unsigned int mv = 0;
};

struct OdTable
{
    OdState core, mem;
    unsigned int coreMin = 0, coreMax = 0, memMin = 0, memMax = 0;
};

bool readOd(wrap_amdsysfs_handle* sysfsh, int index, OdTable& _table)
{
    std::ifstream ifs(deviceFile(sysfsh, index, "pp_od_clk_voltage"));
    if (!ifs)
        return false;
    static const std::regex state(R"(^\s*(\d+):\s*(\d+)\s*[Mm][Hh]z(?:\s+(\d+)\s*mV)?)");
    static const std::regex range(R"(^\s*(SCLK|MCLK):\s*(\d+)\s*[Mm][Hh]z\s+(\d+)\s*[Mm][Hh]z)");
    std::string section, line;
    bool any = false;
    while (std::getline(ifs, line))
    {
        std::smatch m;
        if (line.compare(0, 3, "OD_") == 0)
        {
            section = line.substr(0, line.find(':'));
            continue;
        }
        if ((section == "OD_SCLK" || section == "OD_MCLK") && std::regex_search(line, m, state))
        {
            OdState& s = section == "OD_SCLK" ? _table.core : _table.mem;
            int i = std::stoi(m.str(1));
            if (i >= s.index)
            {
                s.index = i;
                s.mhz = (unsigned int)std::stoul(m.str(2));
                s.mv = m[3].matched ? (unsigned int)std::stoul(m.str(3)) : 0;
                any = true;
            }
        }
        else if (section == "OD_RANGE" && std::regex_search(line, m, range))
        {
            bool core = m.str(1) == "SCLK";
            (core ? _table.coreMin : _table.memMin) = (unsigned int)std::stoul(m.str(2));
            (core ? _table.coreMax : _table.memMax) = (unsigned int)std::stoul(m.str(3));
        }
    }
    return any;
}
}  // namespace

int wrap_amdsysfs_get_od(wrap_amdsysfs_handle* sysfsh, int index, wrap_amdsysfs_od* od)
{
    if (!validIndex(sysfsh, index))
        return -1;
    OdTable t;
    if (!readOd(sysfsh, index, t))
        return ENOENT;
    od->coreMHz = t.core.mhz;
    od->memMHz = t.mem.mhz;
    od->coreMinMHz = t.coreMin;
    od->coreMaxMHz = t.coreMax;
    od->memMinMHz = t.memMin;
    od->memMaxMHz = t.memMax;
    return 0;
}

int wrap_amdsysfs_set_od_clock(wrap_amdsysfs_handle* sysfsh, int index, int mem, unsigned int mhz)
{
    if (!validIndex(sysfsh, index))
        return -1;
    OdTable t;
    if (!readOd(sysfsh, index, t))
        return ENOENT;
    const OdState& s = mem ? t.mem : t.core;
    if (s.index < 0)
        return ENOENT;
    std::string cmd = std::string(mem ? "m " : "s ") + std::to_string(s.index) + " " +
                      std::to_string(mhz) + (s.mv ? " " + std::to_string(s.mv) : "");
    int err = writeOd(sysfsh, index, cmd);
    return err ? err : writeOd(sysfsh, index, "c");
}

int wrap_amdsysfs_reset_od(wrap_amdsysfs_handle* sysfsh, int index)
{
    if (!validIndex(sysfsh, index))
        return -1;
    int err = writeOd(sysfsh, index, "r");
    return err ? err : writeOd(sysfsh, index, "c");
}

int wrap_amdsysfs_get_power_caps(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* currentMw,
    unsigned int* minMw, unsigned int* maxMw, unsigned int* defaultMw)
{
    if (!validIndex(sysfsh, index))
        return -1;
    unsigned int cap = 0, lo = 0, hi = 0, def = 0;
    if (!readHwmon(sysfsh, index, "power1_cap", cap) || !cap)
        return ENOENT;
    readHwmon(sysfsh, index, "power1_cap_min", lo);
    readHwmon(sysfsh, index, "power1_cap_max", hi);
    readHwmon(sysfsh, index, "power1_cap_default", def);
    *currentMw = cap / 1000;
    *minMw = lo / 1000;
    *maxMw = hi / 1000;
    *defaultMw = def / 1000;
    return 0;
}

int wrap_amdsysfs_set_power_cap(wrap_amdsysfs_handle* sysfsh, int index, unsigned int milliwatts)
{
    if (!validIndex(sysfsh, index))
        return -1;
    return writeFile(hwmonFile(sysfsh, index, "power1_cap"), std::to_string(milliwatts * 1000ull));
}

int wrap_amdsysfs_set_fan(wrap_amdsysfs_handle* sysfsh, int index, unsigned int pct)
{
    if (!validIndex(sysfsh, index))
        return -1;
    // pwm1_enable: 1 manual, 2 automatic
    if (pct > 100)
        return writeFile(hwmonFile(sysfsh, index, "pwm1_enable"), "2");
    unsigned int pwmMax = 255, pwmMin = 0;
    readHwmon(sysfsh, index, "pwm1_max", pwmMax);
    readHwmon(sysfsh, index, "pwm1_min", pwmMin);
    int err = writeFile(hwmonFile(sysfsh, index, "pwm1_enable"), "1");
    if (err)
        return err;
    unsigned int pwm = pwmMin + (unsigned int)((pwmMax - pwmMin) * pct / 100.0 + 0.5);
    return writeFile(hwmonFile(sysfsh, index, "pwm1"), std::to_string(pwm));
}
