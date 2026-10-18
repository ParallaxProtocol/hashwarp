// AMD sysfs hardware monitoring against a fake /sys tree

#include <libhwmon/wrapamdsysfs.h>

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

namespace fs = boost::filesystem;

namespace
{
void write(const fs::path& _p, const std::string& _content)
{
    fs::create_directories(_p.parent_path());
    std::ofstream(_p.string()) << _content << "\n";
}

struct AmdSysfsTest : public ::testing::Test
{
    fs::path root;
    wrap_amdsysfs_handle* h = nullptr;

    void SetUp() override
    {
        root = fs::temp_directory_path() / fs::unique_path("hashwarp-sysfs-%%%%-%%%%");
        fs::path drm = root / "sys/class/drm";

        // card0: RDNA2 like, labelled sensors, pwm fan, power1_average
        fs::path c0 = drm / "card0/device";
        write(c0 / "vendor", "0x1002");
        write(c0 / "uevent", "DRIVER=amdgpu\nPCI_SLOT_NAME=0000:03:00.0");
        fs::path h0 = c0 / "hwmon/hwmon2";
        write(h0 / "temp1_label", "edge");
        write(h0 / "temp1_input", "55000");
        write(h0 / "temp2_label", "junction");
        write(h0 / "temp2_input", "71000");
        write(h0 / "temp3_label", "mem");
        write(h0 / "temp3_input", "82000");
        write(h0 / "pwm1", "128");
        write(h0 / "pwm1_min", "0");
        write(h0 / "pwm1_max", "255");
        write(h0 / "power1_average", "150000000");
        write(h0 / "power1_cap", "200000000");
        write(h0 / "freq1_input", "2100000000");
        write(h0 / "freq2_input", "1000000000");

        // card1: firmware controlled fan (no pwm1), power1_input only, no labels
        fs::path c1 = drm / "card1/device";
        write(c1 / "vendor", "0x1002");
        write(c1 / "uevent", "PCI_SLOT_NAME=0000:0a:00.0");
        fs::path h1 = c1 / "hwmon/hwmon5";
        write(h1 / "temp1_input", "60000");
        write(h1 / "temp2_input", "75000");
        write(h1 / "fan1_input", "1500");
        write(h1 / "fan1_max", "3000");
        write(h1 / "power1_input", "90000000");

        // card2: not AMD
        write(drm / "card2/device/vendor", "0x10de");

        setenv("HASHWARP_SYSFS_ROOT", root.string().c_str(), 1);
        h = wrap_amdsysfs_create();
    }

    void TearDown() override
    {
        if (h)
            wrap_amdsysfs_destroy(h);
        unsetenv("HASHWARP_SYSFS_ROOT");
        fs::remove_all(root);
    }

    // Index of the GPU on _bus (directory order is not guaranteed)
    int indexOfBus(unsigned _bus)
    {
        for (int i = 0; i < h->sysfs_gpucount; i++)
            if (h->sysfs_pci_bus_id[i] == _bus)
                return i;
        return -1;
    }
};
}  // namespace

TEST_F(AmdSysfsTest, finds_amd_gpus)
{
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->sysfs_gpucount, 2);
    EXPECT_GE(indexOfBus(0x03), 0);
    EXPECT_GE(indexOfBus(0x0a), 0);
}

TEST_F(AmdSysfsTest, labelled_sensors)
{
    ASSERT_NE(h, nullptr);
    int i = indexOfBus(0x03);
    unsigned int t = 0, hot = 0, mem = 0, fan = 0, mw = 0, core = 0, memClk = 0, cap = 0;
    EXPECT_EQ(wrap_amdsysfs_get_tempC(h, i, &t), 0);
    EXPECT_EQ(t, 55u);
    EXPECT_EQ(wrap_amdsysfs_get_extra_tempsC(h, i, &hot, &mem), 0);
    EXPECT_EQ(hot, 71u);
    EXPECT_EQ(mem, 82u);
    EXPECT_EQ(wrap_amdsysfs_get_fanpcnt(h, i, &fan), 0);
    EXPECT_EQ(fan, 50u);
    EXPECT_EQ(wrap_amdsysfs_get_power_usage(h, i, &mw), 0);
    EXPECT_EQ(mw, 150000u);
    EXPECT_EQ(wrap_amdsysfs_get_power_limit(h, i, &cap), 0);
    EXPECT_EQ(cap, 200000u);
    EXPECT_EQ(wrap_amdsysfs_get_clocks(h, i, &core, &memClk), 0);
    EXPECT_EQ(core, 2100u);
    EXPECT_EQ(memClk, 1000u);
}

TEST_F(AmdSysfsTest, unlabelled_sensors_and_fallbacks)
{
    ASSERT_NE(h, nullptr);
    int i = indexOfBus(0x0a);
    unsigned int t = 0, hot = 0, mem = 0, fan = 0, mw = 0, cap = 0;
    EXPECT_EQ(wrap_amdsysfs_get_tempC(h, i, &t), 0);
    EXPECT_EQ(t, 60u);
    EXPECT_EQ(wrap_amdsysfs_get_extra_tempsC(h, i, &hot, &mem), 0);
    EXPECT_EQ(hot, 75u);
    EXPECT_EQ(mem, 0u);  // no sensor
    EXPECT_EQ(wrap_amdsysfs_get_fanpcnt(h, i, &fan), 0);
    EXPECT_EQ(fan, 50u);  // from rpm
    EXPECT_EQ(wrap_amdsysfs_get_power_usage(h, i, &mw), 0);
    EXPECT_EQ(mw, 90000u);  // from power1_input
    EXPECT_EQ(wrap_amdsysfs_get_power_limit(h, i, &cap), -1);
    EXPECT_EQ(wrap_amdsysfs_get_tempC(h, 7, &t), -1);  // bad index
}
