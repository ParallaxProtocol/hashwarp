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

/* Root of /sys (HASHWARP_SYSFS_ROOT environment variable, for tests) */
std::string wrap_amdsysfs_root();
