// Overclocking through a fake NVML library and a fake amdgpu sysfs tree

#include <libhwmon/GpuControl.h>

#include "fake_nvml.h"

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace dev;
namespace fs = boost::filesystem;

namespace
{
bool hasMessage(const std::vector<std::string>& _messages, const std::string& _part)
{
    for (const auto& m : _messages)
        if (m.find(_part) != std::string::npos)
            return true;
    return false;
}

struct NvmlControlTest : public ::testing::Test
{
    wrap_nvml_handle* h = nullptr;
    FakeNvmlState* state = nullptr;

    void SetUp() override
    {
        setenv("HASHWARP_NVML_LIB", FAKE_NVML_LIB, 1);
        h = wrap_nvml_create();
        ASSERT_NE(h, nullptr);
        // The same library as the one NVML was loaded from
        void* lib = dlopen(FAKE_NVML_LIB, RTLD_NOW);
        state = ((FakeNvmlState * (*)()) dlsym(lib, "fake_nvml_state"))();
        *state = FakeNvmlState();
    }
    void TearDown() override
    {
        if (h)
            wrap_nvml_destroy(h);
        unsetenv("HASHWARP_NVML_LIB");
    }
};
}  // namespace

TEST_F(NvmlControlTest, limits_and_id)
{
    auto c = GpuControl::nvml(h, 0);
    EXPECT_EQ(c->id(), "GPU-fake-0001");
    EXPECT_EQ(c->limits().maxCoreClock, 3000u);
    EXPECT_EQ(c->limits().minMemOffset, -2000);
    EXPECT_EQ(c->limits().maxMemOffset, 6000);
    EXPECT_EQ(c->limits().maxCoreOffset, 1000);
    EXPECT_EQ(c->limits().minPowerW, 100u);
    EXPECT_EQ(c->limits().maxPowerW, 400u);
    EXPECT_EQ(c->limits().defaultPowerW, 300u);
}

TEST_F(NvmlControlTest, apply_and_restore)
{
    state->memOffset = 500;  // set before hashwarp started: restored to it
    auto c = GpuControl::nvml(h, 0);
    OcSettings s;
    s.coreClock = 1500;
    s.coreOffset = -100;
    s.memOffset = 2000;
    s.powerPct = 80;
    s.fanPct = 70;
    EXPECT_TRUE(c->apply(s).empty());
    EXPECT_EQ(state->lockedMin, 1500u);
    EXPECT_EQ(state->lockedMax, 1500u);
    EXPECT_EQ(state->coreOffset, -100);
    EXPECT_EQ(state->memOffset, 2000);
    EXPECT_EQ(state->powerMw, 240000u);
    EXPECT_EQ(state->fanPct, 70u);
    EXPECT_TRUE(state->fanManual[0] && state->fanManual[1]);
    EXPECT_GT(state->newOffsetCalls, 0u);
    EXPECT_TRUE(c->changed());

    // A second change keeps the original values
    s = OcSettings();
    s.memOffset = 2500;
    EXPECT_TRUE(c->apply(s).empty());

    c->restore();
    EXPECT_EQ(state->lockedMax, 0u);
    EXPECT_EQ(state->coreOffset, 0);
    EXPECT_EQ(state->memOffset, 500);
    EXPECT_EQ(state->powerMw, 300000u);
    EXPECT_FALSE(state->fanManual[0] || state->fanManual[1]);
    EXPECT_FALSE(c->changed());
}

TEST_F(NvmlControlTest, drivers_before_r555)
{
    state->oldDriver = true;
    auto c = GpuControl::nvml(h, 0);
    EXPECT_EQ(c->limits().maxMemOffset, 6000);
    OcSettings s;
    s.memOffset = 1000;
    EXPECT_TRUE(c->apply(s).empty());
    EXPECT_EQ(state->memOffset, 1000);
    EXPECT_EQ(state->newOffsetCalls, 0u);
    c->restore();
    EXPECT_EQ(state->memOffset, 0);
}

TEST_F(NvmlControlTest, errors_are_reported)
{
    auto c = GpuControl::nvml(h, 0);
    OcSettings s;
    s.powerW = 450;    // above the maximum
    s.memClock = 900;  // AMD only
    auto errors = c->apply(s);
    EXPECT_TRUE(hasMessage(errors, "out of the GPU's range 100 - 400 W"));
    EXPECT_TRUE(hasMessage(errors, "use --oc-mem-offset"));
    EXPECT_EQ(state->powerMw, 300000u);

    state->denyWrites = true;
    s = OcSettings();
    s.coreClock = 1500;
    errors = c->apply(s);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].find("needs root"), std::string::npos);
    EXPECT_FALSE(c->changed());  // nothing to restore
}

TEST(ocsettings, merge_and_str)
{
    OcSettings profile;
    profile.coreClock = 1400;
    profile.powerPct = 70;
    profile.memOffset = 1000;
    OcSettings explicitOpts;
    explicitOpts.powerW = 250;
    explicitOpts.memOffset = 1500;
    profile.merge(explicitOpts);
    EXPECT_EQ(*profile.coreClock, 1400u);
    EXPECT_EQ(*profile.memOffset, 1500);
    EXPECT_EQ(*profile.powerW, 250u);
    EXPECT_FALSE(profile.powerPct);  // one power limit
    EXPECT_EQ(profile.str(), "core 1400 MHz, mem +1500 MHz, power 250 W");
    OcSettings fan;
    fan.fanPct = OcSettings::c_fanAuto;
    EXPECT_EQ(fan.str(), "fan auto");
    EXPECT_TRUE(OcSettings().empty());
}

namespace
{
void write(const fs::path& _p, const std::string& _content)
{
    fs::create_directories(_p.parent_path());
    std::ofstream(_p.string()) << _content << "\n";
}

std::string read(const fs::path& _p)
{
    std::ifstream ifs(_p.string());
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

struct AmdControlTest : public ::testing::Test
{
    fs::path root, dev0, hw0, dev1;
    wrap_amdsysfs_handle* h = nullptr;

    void SetUp() override
    {
        root = fs::temp_directory_path() / fs::unique_path("hashwarp-oc-%%%%-%%%%");
        fs::path drm = root / "sys/class/drm";

        // card0: RDNA2 overdrive table
        dev0 = drm / "card0/device";
        write(dev0 / "vendor", "0x1002");
        write(dev0 / "uevent", "PCI_SLOT_NAME=0000:03:00.0");
        write(dev0 / "unique_id", "7a1c2b3d4e5f6071");
        write(dev0 / "pp_od_clk_voltage",
            "OD_SCLK:\n0: 500Mhz\n1: 2615Mhz\nOD_MCLK:\n0: 97Mhz\n1: 1000MHz\n"
            "OD_VDDGFX_OFFSET:\n0mV\nOD_RANGE:\nSCLK:     500Mhz       3150Mhz\n"
            "MCLK:     674Mhz       1200Mhz");
        hw0 = dev0 / "hwmon/hwmon2";
        write(hw0 / "temp1_input", "55000");
        write(hw0 / "pwm1", "128");
        write(hw0 / "pwm1_enable", "2");
        write(hw0 / "pwm1_min", "0");
        write(hw0 / "pwm1_max", "255");
        write(hw0 / "power1_cap", "200000000");
        write(hw0 / "power1_cap_min", "100000000");
        write(hw0 / "power1_cap_max", "250000000");
        write(hw0 / "power1_cap_default", "220000000");

        // card1: Vega/Polaris table, with a voltage per state, overdrive only
        dev1 = drm / "card1/device";
        write(dev1 / "vendor", "0x1002");
        write(dev1 / "uevent", "PCI_SLOT_NAME=0000:0a:00.0");
        write(dev1 / "pp_od_clk_voltage",
            "OD_SCLK:\n0:        300MHz        750mV\n7:       1340MHz       1150mV\n"
            "OD_MCLK:\n0:        300MHz        750mV\n2:       2000MHz        950mV\n"
            "OD_RANGE:\nSCLK:     300MHz       2000MHz\nMCLK:     300MHz       2250MHz");
        write(dev1 / "hwmon/hwmon5/temp1_input", "60000");

        setenv("HASHWARP_SYSFS_ROOT", root.string().c_str(), 1);
        h = wrap_amdsysfs_create();
        ASSERT_NE(h, nullptr);
    }

    void TearDown() override
    {
        if (h)
            wrap_amdsysfs_destroy(h);
        unsetenv("HASHWARP_SYSFS_ROOT");
        fs::remove_all(root);
    }

    int indexOfBus(unsigned _bus)
    {
        for (int i = 0; i < h->sysfs_gpucount; i++)
            if (h->sysfs_pci_bus_id[i] == _bus)
                return i;
        return -1;
    }
};
}  // namespace

TEST_F(AmdControlTest, rdna_apply_and_restore)
{
    auto c = GpuControl::amd(h, indexOfBus(3));
    EXPECT_EQ(c->id(), "amd-7a1c2b3d4e5f6071");
    EXPECT_EQ(c->limits().maxCoreClock, 3150u);
    EXPECT_EQ(c->limits().maxMemClock, 1200u);
    EXPECT_EQ(c->limits().coreStateClock, 2615u);
    EXPECT_EQ(c->limits().memStateClock, 1000u);
    EXPECT_EQ(c->limits().defaultPowerW, 220u);
    EXPECT_EQ(c->limits().maxPowerW, 250u);

    OcSettings s;
    s.coreClock = 1800;
    s.memClock = 1100;
    s.powerW = 180;
    s.fanPct = 60;
    EXPECT_TRUE(c->apply(s).empty());
    std::string od = read(dev0 / "pp_od_clk_voltage");
    EXPECT_NE(od.find("s 1 1800\nc\nm 1 1100\nc\n"), std::string::npos);
    EXPECT_EQ(read(hw0 / "power1_cap"), "180000000\n");
    EXPECT_EQ(read(hw0 / "pwm1_enable"), "1\n");
    EXPECT_EQ(read(hw0 / "pwm1"), "153\n");

    c->restore();
    od = read(dev0 / "pp_od_clk_voltage");
    EXPECT_NE(od.find("m 1 1100\nc\nr\nc\n"), std::string::npos);
    EXPECT_EQ(read(hw0 / "power1_cap"), "200000000\n");
    EXPECT_EQ(read(hw0 / "pwm1_enable"), "2\n");
}

TEST_F(AmdControlTest, vega_keeps_the_voltage)
{
    auto c = GpuControl::amd(h, indexOfBus(10));
    EXPECT_EQ(c->id(), "amd-0000:0a:00");
    OcSettings s;
    s.memClock = 2100;
    s.coreClock = 1250;
    EXPECT_TRUE(c->apply(s).empty());
    std::string od = read(dev1 / "pp_od_clk_voltage");
    EXPECT_NE(od.find("s 7 1250 1150\nc\nm 2 2100 950\nc\n"), std::string::npos);
}

TEST_F(AmdControlTest, unsupported_settings)
{
    auto c = GpuControl::amd(h, indexOfBus(10));
    OcSettings s;
    s.memOffset = 500;
    s.powerW = 150;  // no power1_cap
    auto errors = c->apply(s);
    EXPECT_TRUE(hasMessage(errors, "use --oc-core-clock and --oc-mem-clock"));
    EXPECT_TRUE(hasMessage(errors, "power"));

    fs::remove(dev1 / "pp_od_clk_voltage");
    s = OcSettings();
    s.memClock = 2100;
    errors = c->apply(s);
    EXPECT_TRUE(hasMessage(errors, "amdgpu.ppfeaturemask"));
}
