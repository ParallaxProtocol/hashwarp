// Hashwarp command line and configuration file options.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <string>
#include <vector>

namespace CLI
{
class App;
}

namespace hashwarp
{
// Default getwork endpoint of a local Parallax node
static const char* const c_defaultSoloUrl = "http://127.0.0.1:8545";

/// Every user facing setting. Each one is a command line option and, under the
/// same name without leading dashes, a key of the JSON configuration file.
struct Options
{
    // Connection
    std::vector<std::string> pools;  // host:port or URLs, in failover order
    std::string wallet;
    std::string worker;
    std::string password = "x";
    bool tls = false;
    std::string esm = "auto";  // Stratum flavour: auto, 0, 1, 2, 3
    std::string solo;          // getwork URL of a Parallax node (empty: not solo)
    bool soloRequested = false;
    bool reportHashrate = false;
    unsigned getworkInterval = 1000;  // ms
    unsigned poolRetries = 3;
    unsigned retryDelay = 5;           // s
    unsigned failoverTimeout = 0;      // min
    unsigned workTimeout = 7200;       // s
    unsigned responseTimeout = 2;      // s

    // Devices
    bool listDevices = false;
    std::string backend = "auto";  // auto, cuda, opencl
    std::vector<unsigned> gpuIds;
    bool disableNvidia = false;
    bool disableAmd = false;
    bool disableIntel = false;
    std::vector<unsigned> cudaDevices;    // legacy --cu-devices
    std::vector<unsigned> openclDevices;  // legacy --cl-devices

    // Tuning. Lists hold one value per selected GPU; a single value applies to all
    std::vector<unsigned> cudaGridSize;
    std::vector<unsigned> cudaBlockSize;
    std::vector<unsigned> cudaStreams;
    std::string cudaSchedule = "sync";
    std::string cudaKernel = "auto";
    std::vector<unsigned> clGlobalWork;
    std::vector<unsigned> clLocalWork;
    bool clNoExit = false;
    unsigned dagLoadMode = 0;
    bool noEval = false;
    unsigned ergodicity = 0;

    // Hardware monitoring and safety
    bool noHwmon = false;
    int legacyHwmon = -1;              // legacy --HWMON
    std::vector<unsigned> tempStop;    // per GPU, 0 = off
    std::vector<unsigned> tempResume;  // per GPU
    std::vector<unsigned> memTempStop;    // per GPU, 0 = off
    std::vector<unsigned> memTempResume;  // per GPU
    unsigned shutdownTemp = 0;            // 0 = off
    unsigned watchdogTimeout = 120;       // s, 0 = off
    unsigned watchdogRestarts = 3;        // per GPU per hour
    std::string watchdogScript;

    // Logging and display
    std::string logFile;
    unsigned logMaxSize = 20;  // MB
    unsigned logKeep = 3;
    std::string logLevel = "info";
    std::vector<std::string> logDebug;  // debug categories
    unsigned legacyVerbosity = 0;       // legacy -v bitmask
    bool noColor = false;
    bool syslog = false;
    bool logStdout = false;
    unsigned displayInterval = 10;  // s
    unsigned statsInterval = 60;    // s, 0 = off
    bool noHotkeys = false;

    // API
    std::string apiBind;  // address:port
    int legacyApiPort = 0;
    std::string apiToken;
    bool apiAllowControl = false;

    // Misc
    int benchmarkBlock = -1;  // >= 0: simulation mode on this block
    unsigned selftestSeconds = 0;  // > 0: run the GPU self-test for at most this long
    bool exitOnError = false;
    std::string saveConfig;  // write the configuration to this file and exit
};

/// Registers every option of _opts on _app, including the JSON --config file
void registerOptions(CLI::App& _app, Options& _opts);

/// Checks option values and resolves defaults that depend on other options.
/// Throws std::invalid_argument on errors, returns warnings
std::vector<std::string> validateOptions(Options& _opts);

/// Builds the pool URIs (as accepted by dev::URI) from pools, wallet, worker,
/// password, tls, esm and solo
std::vector<std::string> buildPoolUris(const Options& _opts);

/// Debug categories (LOG_JSON ...) selected by --log-debug and the legacy -v
unsigned debugCategories(const Options& _opts);

/// Values of a per GPU list for the _index-th selected GPU: the matching
/// element, the only one if a single value was given, otherwise 0 (default)
unsigned perGpu(const std::vector<unsigned>& _values, size_t _index);

/// Whether _wallet looks like a Parallax address (0x followed by 40 hex digits)
bool isAddress(const std::string& _wallet);

/// JSON configuration of the options set on _app (written by --save-config)
std::string configToJson(const CLI::App& _app);

/// Default configuration file: hashwarp.json next to the executable, or in the
/// working directory if that one doesn't exist. Empty if none exists
std::string findDefaultConfig();

/// Where a new configuration file is created by default
std::string defaultConfigPath();

}  // namespace hashwarp
