// Hashwarp overclocking settings: --oc-* options, profiles and tuning results.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include "Options.h"

#include <libhwmon/GpuControl.h>

#include <map>
#include <optional>
#include <string>

namespace hashwarp
{
/// Overclocking asked for one selected GPU
struct OcRequest
{
    dev::OcSettings settings;  // a profile of oc-profiles, overridden by the --oc-* options
    std::string gpuProfile;    // stock, efficient or tuned: depends on the GPU (gpuProfile())

    bool empty() const { return settings.empty() && gpuProfile.empty(); }
};

/// Overclocking of the _gpu-th selected GPU. Throws std::invalid_argument on
/// invalid values and unknown profiles
OcRequest ocRequest(const Options& _o, size_t _gpu);

/// Checks every --oc-* value and profile (part of validateOptions)
void validateOc(const Options& _o);

/// Results of --oc-tune for one GPU
struct TunedGpu
{
    std::string name;
    std::string mode;          // all or core
    dev::OcSettings settings;  // core clock, and the memory offset or clock for "all"
    double hashrate = 0;       // H/s
    double powerW = 0;
    std::string date;
};

/// By GpuControl::id()
using TuneResults = std::map<std::string, TunedGpu>;

/// Empty if the file doesn't exist. Throws std::runtime_error if it can't be read
TuneResults loadTuneResults(const std::string& _path);
void saveTuneResults(const std::string& _path, const TuneResults& _results);

/// --oc-tune-file, or hashwarp-oc.json next to the configuration file
std::string tuneFilePath(const Options& _o);

/// Settings of a profile that depends on the GPU. _note says what was chosen, or
/// why nothing can be (nullopt)
std::optional<dev::OcSettings> gpuProfile(const std::string& _profile, const TuneResults& _tuned,
    const std::string& _gpuId, const dev::OcLimits& _limits, std::string& _note);

/// The power limit of the efficient profile for GPUs that weren't tuned
static const unsigned c_efficientPowerPct = 80;

}  // namespace hashwarp
