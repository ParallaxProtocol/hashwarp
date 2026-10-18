/*
 * Wrapper for AMD SysFS on linux, using adapted code from amdcovc by matszpk
 *
 * By Philipp Andreas - github@smurfy.de
   Reworked and simplified by Andrea Lanfranchi (github @AndreaLanfranchi)
 */

#pragma once

#include <string>

typedef struct
{
    int sysfs_gpucount;
    unsigned int* sysfs_device_id;
    unsigned int* sysfs_hwmon_id;
    unsigned int* sysfs_pci_domain_id;
    unsigned int* sysfs_pci_bus_id;
    unsigned int* sysfs_pci_device_id;
} wrap_amdsysfs_handle;

typedef struct
{
    int DeviceId = -1;
    int HwMonId = -1;
    int PciDomain = -1;
    int PciBus = -1;
    int PciDevice = -1;

} pciInfo;

wrap_amdsysfs_handle* wrap_amdsysfs_create();
int wrap_amdsysfs_destroy(wrap_amdsysfs_handle* sysfsh);

int wrap_amdsysfs_get_gpucount(wrap_amdsysfs_handle* sysfsh, int* gpucount);

int wrap_amdsysfs_get_tempC(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* tempC);

int wrap_amdsysfs_get_fanpcnt(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* fanpcnt);

int wrap_amdsysfs_get_power_usage(
    wrap_amdsysfs_handle* sysfsh, int index, unsigned int* milliwatts);

/* Junction (hotspot) and memory temperatures, 0 when the GPU doesn't report them */
int wrap_amdsysfs_get_extra_tempsC(
    wrap_amdsysfs_handle* sysfsh, int index, unsigned int* junctionC, unsigned int* memC);

/* Current core (sclk) and memory (mclk) clocks in MHz */
int wrap_amdsysfs_get_clocks(
    wrap_amdsysfs_handle* sysfsh, int index, unsigned int* coreMHz, unsigned int* memMHz);

/* Power cap */
int wrap_amdsysfs_get_power_limit(
    wrap_amdsysfs_handle* sysfsh, int index, unsigned int* milliwatts);

/*
 * Overclocking (needs root). These return 0 on success, otherwise an errno
 * value (or -1 for an invalid index or a file that can't be parsed)
 */

/* Overdrive state of pp_od_clk_voltage: highest core and memory states and their ranges */
struct wrap_amdsysfs_od
{
    unsigned int coreMHz = 0, memMHz = 0;  /* current top state clocks */
    unsigned int coreMinMHz = 0, coreMaxMHz = 0, memMinMHz = 0, memMaxMHz = 0;  /* OD_RANGE */
};

/* ENOENT when overdrive is off (amdgpu.ppfeaturemask) or the GPU has no such table */
int wrap_amdsysfs_get_od(wrap_amdsysfs_handle* sysfsh, int index, wrap_amdsysfs_od* od);

/* Sets the clock of the highest core (mem = 0) or memory (mem = 1) state and commits it */
int wrap_amdsysfs_set_od_clock(wrap_amdsysfs_handle* sysfsh, int index, int mem, unsigned int mhz);

/* Back to the default clocks */
int wrap_amdsysfs_reset_od(wrap_amdsysfs_handle* sysfsh, int index);

/* Power cap in milliwatts: current, allowed range and default (0 when not reported) */
int wrap_amdsysfs_get_power_caps(wrap_amdsysfs_handle* sysfsh, int index, unsigned int* currentMw,
    unsigned int* minMw, unsigned int* maxMw, unsigned int* defaultMw);
int wrap_amdsysfs_set_power_cap(wrap_amdsysfs_handle* sysfsh, int index, unsigned int milliwatts);

/* Fan speed in percent, or back to automatic control when pct > 100 */
int wrap_amdsysfs_set_fan(wrap_amdsysfs_handle* sysfsh, int index, unsigned int pct);

/* Root of /sys (HASHWARP_SYSFS_ROOT environment variable, for tests) */
std::string wrap_amdsysfs_root();
