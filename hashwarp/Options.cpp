// Hashwarp command line and configuration file options.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Options.h"
#include "Oc.h"

#include <libdevcore/Log.h>
#include <libpoolprotocols/PoolURI.h>

#include <CLI/CLI.hpp>
#include <json/json.h>

#include <boost/asio/ip/address.hpp>
#include <boost/dll.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace hashwarp
{
namespace
{
// Groups of options in --help. Options in no group are hidden (legacy aliases)
const char* const c_groupPool = "Pool";
const char* const c_groupDevices = "Devices";
const char* const c_groupTuning = "Tuning (per GPU lists: one value per GPU, or one for all)";
const char* const c_groupHardware = "Hardware";
const char* const c_groupOc = "Overclocking (per GPU lists, x leaves a GPU alone; needs root or administrator rights)";
const char* const c_groupLogging = "Logging and display";
const char* const c_groupApi = "API";
const char* const c_groupMisc = "Other";

const std::map<std::string, unsigned> c_debugCategories = {{"json", LOG_JSON},
    {"per-gpu", LOG_PER_GPU}, {"connect", LOG_CONNECT}, {"switch", LOG_SWITCH},
    {"submit", LOG_SUBMIT}, {"flow", LOG_PROGRAMFLOW}};

// Options which are never written to a configuration file
const std::set<std::string> c_notSaved = {"help", "help-all", "version", "config", "save-config",
    "list-devices", "benchmark", "selftest", "oc-tune", "oc-reset"};

bool isInteger(const std::string& _s)
{
    return !_s.empty() &&
           std::all_of(_s.begin() + (_s[0] == '-' ? 1 : 0), _s.end(), [](unsigned char c) {
               return std::isdigit(c);
           }) &&
           _s != "-";
}

Json::Value jsonValue(const std::string& _s)
{
    if (isInteger(_s) && _s.size() < 18)
        return Json::Value(Json::Int64(std::stoll(_s)));
    return Json::Value(_s);
}

/// JSON configuration files for CLI11: top level keys are long option names
class JsonConfig : public CLI::Config
{
public:
    std::string to_config(const CLI::App* _app, bool _defaultAlso, bool, std::string) const override
    {
        Json::Value root(Json::objectValue);
        for (const CLI::Option* opt : _app->get_options({}))
        {
            if (opt->get_lnames().empty() || !opt->get_configurable() || opt->get_group().empty())
                continue;
            const std::string& name = opt->get_lnames()[0];
            if (c_notSaved.count(name))
                continue;

            if (opt->get_type_size() == 0)
            {
                // Flag
                if (opt->count())
                    root[name] = true;
                else if (_defaultAlso)
                    root[name] = false;
                continue;
            }

            std::vector<std::string> values = opt->results();
            if (values.empty())
            {
                if (!_defaultAlso || opt->get_default_str().empty())
                    continue;
                values = {opt->get_default_str()};
            }
            if (name == "oc-profiles")
            {
                // Written as the object it is
                Json::CharReaderBuilder reader;
                std::string errors;
                std::istringstream in(values.back());
                Json::Value profiles;
                if (Json::parseFromStream(reader, in, &profiles, &errors))
                    root[name] = profiles;
                continue;
            }
            if (opt->get_items_expected_max() > 1)
            {
                Json::Value arr(Json::arrayValue);
                for (const auto& v : values)
                    arr.append(jsonValue(v));
                root[name] = arr;
            }
            else
                root[name] = jsonValue(values.back());
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "    ";
        // Keep lists of plain values on one line: "temp-stop" : [85, 80]
        static const std::regex list("\\s*\\[\\s*([^\\[\\]{}]*?)\\s*\\]");
        static const std::regex separator(",\\s+");
        std::string out = Json::writeString(builder, root);
        std::string compact;
        auto last = out.cbegin();
        for (std::sregex_iterator it(out.begin(), out.end(), list), end; it != end; ++it)
        {
            compact.append(last, (*it)[0].first);
            compact += " [" + std::regex_replace((*it)[1].str(), separator, ", ") + "]";
            last = (*it)[0].second;
        }
        compact.append(last, out.cend());
        return compact + "\n";
    }

    std::vector<CLI::ConfigItem> from_config(std::istream& _input) const override
    {
        Json::CharReaderBuilder builder;
        builder["allowComments"] = true;
        Json::Value root;
        std::string errors;
        if (!Json::parseFromStream(builder, _input, &root, &errors))
            throw CLI::ConversionError("Invalid JSON configuration: " + errors);
        if (!root.isObject())
            throw CLI::ConversionError("The JSON configuration must be an object");

        std::vector<CLI::ConfigItem> items;
        for (const auto& name : root.getMemberNames())
        {
            const Json::Value& v = root[name];
            CLI::ConfigItem item;
            item.name = name;
            if (v.isObject())
            {
                // oc-profiles: the option takes the object as JSON text
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                item.inputs.push_back(Json::writeString(writer, v));
            }
            else if (v.isArray())
            {
                for (const auto& e : v)
                    item.inputs.push_back(scalar(name, e));
            }
            else
                item.inputs.push_back(scalar(name, v));
            items.push_back(item);
        }
        return items;
    }

private:
    static std::string scalar(const std::string& _name, const Json::Value& _v)
    {
        if (_v.isBool())
            return _v.asBool() ? "true" : "false";
        if (_v.isIntegral())
            return _v.isInt64() ? std::to_string(_v.asInt64()) : std::to_string(_v.asUInt64());
        if (_v.isDouble())
        {
            std::ostringstream ss;
            ss << _v.asDouble();
            return ss.str();
        }
        if (_v.isString())
            return _v.asString();
        throw CLI::ConversionError("Invalid value for '" + _name + "' in configuration file");
    }
};

// Userinfo parts with characters the URI parser treats as separators
// must be enclosed in backticks
std::string uriPart(const std::string& _s)
{
    if (_s.find('`') != std::string::npos)
        throw std::invalid_argument("Backticks are not allowed in wallet, worker or password");
    if (_s.find_first_of(".:@/") != std::string::npos)
        return "`" + _s + "`";
    return _s;
}

std::string userInfo(const Options& _opts)
{
    std::string ret = uriPart(_opts.wallet);
    if (!_opts.worker.empty())
        ret += "." + uriPart(_opts.worker);
    if (!_opts.password.empty())
        ret += ":" + uriPart(_opts.password);
    return ret + "@";
}

// URL scheme of a host:port pool. "stratum+tcp" is plain Stratum, only the bare
// schemes detect the flavour ("stratumss" with TLS 1.2 or newer, like "+ssl")
std::string stratumScheme(const std::string& _esm, bool _tls)
{
    const char* tail = _tls ? "+ssl" : "+tcp";
    if (_esm == "0")
        return std::string("stratum1") + tail;  // EthProxy
    if (_esm == "1" || _esm == "2")
        return std::string("stratum2") + tail;  // EthereumStratum/1.0.0 (NiceHash)
    if (_esm == "3")
        return std::string("stratum3") + tail;  // EthereumStratum/2.0.0
    return _tls ? "stratumss" : "stratum";      // Autodetect
}

void checkPerGpu(const std::vector<unsigned>& _values, const std::set<unsigned>& _allowed,
    unsigned _min, unsigned _max, const char* _name)
{
    for (auto v : _values)
    {
        if (!_allowed.empty() && !_allowed.count(v))
        {
            std::string allowed;
            for (auto a : _allowed)
                allowed += (allowed.empty() ? "" : ", ") + std::to_string(a);
            throw std::invalid_argument(
                std::string("--") + _name + " values must be one of " + allowed);
        }
        if (_allowed.empty() && (v < _min || v > _max))
            throw std::invalid_argument(std::string("--") + _name + " values must be in [" +
                                        std::to_string(_min) + " .. " + std::to_string(_max) +
                                        "]");
    }
}
}  // namespace

void registerOptions(CLI::App& _app, Options& _o)
{
    _app.option_defaults()->always_capture_default();
    _app.get_formatter()->column_width(32);
    _app.set_help_flag("-h,--help", "Show this help and exit");
    _app.set_help_all_flag("--help-all", "Show help including legacy options and exit");
    _app.name("hashwarp");
    _app.config_formatter(std::make_shared<JsonConfig>());
    _app.allow_config_extras(CLI::config_extras_mode::error);

    // Pool
    _app.add_option("-o,--pool,-P", _o.pools,
            "Pool as host:port or URL (stratum+tcp://, stratum+ssl://, http:// for "
            "getwork). Repeat or comma separate for failover pools")
        ->delimiter(',')
        ->group(c_groupPool);
    _app.add_option("-u,--wallet", _o.wallet, "Your Parallax address (0x...)")
        ->envname("HASHWARP_WALLET")
        ->group(c_groupPool);
    _app.add_option("-w,--worker", _o.worker, "Worker (rig) name shown by the pool")
        ->group(c_groupPool);
    _app.add_option("-p,--password", _o.password, "Pool password")->group(c_groupPool);
    _app.add_flag("--tls", _o.tls, "Use TLS for pools given as host:port")->group(c_groupPool);
    _app.add_option("--esm", _o.esm,
            "Stratum flavour: auto, 0 = EthProxy, 1/2 = EthereumStratum/1.0.0 (NiceHash), "
            "3 = EthereumStratum/2.0.0")
        ->check(CLI::IsMember({"auto", "0", "1", "2", "3"}))
        ->group(c_groupPool);
    _app.add_option("--solo", _o.solo,
            "Solo mine with getwork on a Parallax node (default " +
                std::string(c_defaultSoloUrl) +
                "). Rewards go to the node's --miner.coinbase")
        ->expected(0, 1)
        ->default_str("")
        ->group(c_groupPool);
    _app.add_flag("-R,--report-hashrate,--report-hr", _o.reportHashrate,
            "Report the hashrate to the pool or node")
        ->group(c_groupPool);
    _app.add_option("--pool-retries,--farm-retries", _o.poolRetries,
            "Connection attempts before switching to the next pool (a single pool is "
            "retried forever)")
        ->check(CLI::Range(0, 99999))
        ->group(c_groupPool);
    _app.add_option("--retry-delay", _o.retryDelay,
            "Seconds before reconnecting, doubles on each failed attempt (up to 120)")
        ->check(CLI::Range(1, 999))
        ->group(c_groupPool);
    _app.add_flag("--no-pool-check", _o.noPoolCheck,
            "Don't warn when the pool finds most of the network's blocks (hashwarp reads the "
            "pools' shares from status.parallaxprotocol.org every 10 minutes)")
        ->group(c_groupPool);
    _app.add_option("--failover-timeout", _o.failoverTimeout,
            "Minutes on a failover pool before trying the first one again (0 = never)")
        ->check(CLI::Range(0, 999))
        ->group(c_groupPool);
    _app.add_option("--work-timeout", _o.workTimeout,
            "Reconnect if no new work arrives for this many seconds")
        ->check(CLI::Range(600, 99999))
        ->group(c_groupPool);
    _app.add_option("--response-timeout", _o.responseTimeout,
            "Reconnect if the pool doesn't answer within this many seconds")
        ->check(CLI::Range(2, 999))
        ->group(c_groupPool);
    _app.add_option("--getwork-interval,--farm-recheck", _o.getworkInterval,
            "Milliseconds between getwork polls (solo / http pools)")
        ->check(CLI::Range(100, 99999))
        ->group(c_groupPool);

    // Devices
    _app.add_flag("--list-devices", _o.listDevices, "List the GPUs and exit")
        ->group(c_groupDevices);
    _app.add_option("--backend", _o.backend,
            "auto (CUDA on NVIDIA, OpenCL on the others), cuda or opencl")
        ->check(CLI::IsMember({"auto", "cuda", "opencl"}))
        ->group(c_groupDevices);
    _app.add_flag_callback("-U,--cuda", [&_o]() { _o.backend = "cuda"; }, "Same as --backend cuda")
        ->configurable(false)
        ->group(c_groupDevices);
    _app.add_flag_callback(
            "-G,--opencl", [&_o]() { _o.backend = "opencl"; }, "Same as --backend opencl")
        ->configurable(false)
        ->group(c_groupDevices);
    _app.add_option("--gpu-id,--devices", _o.gpuIds,
            "GPUs to mine on, as listed by --list-devices (default: all)")
        ->delimiter(',')
        ->group(c_groupDevices);
    _app.add_flag("--disable-gpu-nvidia", _o.disableNvidia, "Don't mine on NVIDIA GPUs")
        ->group(c_groupDevices);
    _app.add_flag("--disable-gpu-amd", _o.disableAmd, "Don't mine on AMD GPUs")
        ->group(c_groupDevices);
    _app.add_flag("--disable-gpu-intel", _o.disableIntel, "Don't mine on Intel GPUs")
        ->group(c_groupDevices);
    _app.add_option("--cuda-devices,--cu-devices", _o.cudaDevices, "")->delimiter(',')->group("");
    _app.add_option("--opencl-devices,--opencl-device,--cl-devices", _o.openclDevices, "")
        ->delimiter(',')
        ->group("");

    // Tuning
    _app.add_option("--cuda-grid-size,--cu-grid-size", _o.cudaGridSize,
            "CUDA grid size [1 .. 131072] (default 8192)")
        ->delimiter(',')
        ->group(c_groupTuning);
    _app.add_option("--cuda-block-size,--cu-block-size", _o.cudaBlockSize,
            "CUDA block size: 32, 64, 128 or 256 (default 128)")
        ->delimiter(',')
        ->group(c_groupTuning);
    _app.add_option("--cuda-streams,--cu-streams", _o.cudaStreams,
            "CUDA streams [1 .. 99] (default 2)")
        ->delimiter(',')
        ->group(c_groupTuning);
    _app.add_option("--cuda-schedule,--cu-schedule", _o.cudaSchedule,
            "How the CPU waits for CUDA kernels: auto, spin, yield or sync (all GPUs)")
        ->check(CLI::IsMember({"auto", "spin", "yield", "sync"}))
        ->group(c_groupTuning);
    _app.add_option("--cuda-kernel", _o.cudaKernel,
            "CUDA search kernel: auto (time both on each GPU at start), 0 (best when memory "
            "bandwidth limits the hashrate) or 1 (best when compute does)")
        ->check(CLI::IsMember({"auto", "0", "1"}))
        ->group(c_groupTuning);
    _app.add_option("--cl-global-work", _o.clGlobalWork,
            "OpenCL global work size multiplier (default 65536)")
        ->delimiter(',')
        ->group(c_groupTuning);
    _app.add_option("--cl-local-work", _o.clLocalWork,
            "OpenCL work group size: 64, 128 or 256 (default 128)")
        ->delimiter(',')
        ->group(c_groupTuning);
    _app.add_flag("--cl-noexit", _o.clNoExit, "OpenCL: don't use the fast exit kernel (all GPUs)")
        ->group(c_groupTuning);
    _app.add_option("-L,--dag-load-mode", _o.dagLoadMode,
            "0 = build DAGs on all GPUs at once, 1 = one GPU after the other")
        ->check(CLI::Range(0, 1))
        ->group(c_groupTuning);
    _app.add_flag("--noeval", _o.noEval,
            "Submit solutions without checking them on the CPU first (not recommended)")
        ->group(c_groupTuning);
    _app.add_option("--ergodicity", _o.ergodicity,
            "Pick a new random nonce range 0 = once, 1 = per connection, 2 = per job")
        ->check(CLI::Range(0, 2))
        ->group(c_groupTuning);

    // Hardware
    _app.add_flag("--no-hwmon", _o.noHwmon, "Don't read temperatures, fans and power")
        ->group(c_groupHardware);
    _app.add_option("--HWMON", _o.legacyHwmon, "")->check(CLI::Range(0, 2))->group("");
    _app.add_option("--temp-stop,--tstop", _o.tempStop,
            "Pause a GPU at this core temperature in C [30 .. 100] (default off)")
        ->delimiter(',')
        ->group(c_groupHardware);
    _app.add_option("--temp-resume,--tstart", _o.tempResume,
            "Resume a GPU paused by --temp-stop at this temperature (default stop - 15)")
        ->delimiter(',')
        ->group(c_groupHardware);
    _app.add_option("--mem-temp-stop", _o.memTempStop,
            "Pause a GPU at this memory temperature in C (hotspot on GPUs without memory "
            "sensor) [40 .. 120] (default off)")
        ->delimiter(',')
        ->group(c_groupHardware);
    _app.add_option("--mem-temp-resume", _o.memTempResume,
            "Resume a GPU paused by --mem-temp-stop at this temperature (default stop - 10)")
        ->delimiter(',')
        ->group(c_groupHardware);
    _app.add_option("--shutdown-temp", _o.shutdownTemp,
            "Stop hashwarp (exit code 6) if any GPU temperature reaches this in C (default off)")
        ->check(CLI::Range(0, 125))
        ->group(c_groupHardware);
    _app.add_option("--watchdog-timeout", _o.watchdogTimeout,
            "Seconds a GPU may give no sign of life before hashwarp exits (code 5) so it can "
            "be restarted (0 = no watchdog)")
        ->check(CLI::Range(0, 3600))
        ->group(c_groupHardware);
    _app.add_option("--watchdog-restarts", _o.watchdogRestarts,
            "Times per hour a failed GPU is restarted before hashwarp exits (code 5)")
        ->check(CLI::Range(0, 100))
        ->group(c_groupHardware);
    _app.add_option("--watchdog-script", _o.watchdogScript,
            "Run this program (with the GPU index and the reason) when the watchdog gives up, "
            "e.g. to reboot the rig")
        ->group(c_groupHardware);

    // Overclocking
    _app.add_option("--oc-profile", _o.ocProfile,
            "Overclocking profile: stock, efficient (the tuned core clock, or 80% power), tuned "
            "(the --oc-tune results) or one of oc-profiles in the configuration file")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-core-clock", _o.ocCoreClock,
            "Core clock in MHz: locked on NVIDIA, highest state on AMD (0 = default)")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-core-offset", _o.ocCoreOffset, "Core clock offset in MHz (NVIDIA)")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-mem-offset", _o.ocMemOffset,
            "Memory clock offset in MHz (NVIDIA), in the units of nvidia-settings and HiveOS: "
            "twice MSI Afterburner's")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-mem-clock", _o.ocMemClock,
            "Memory clock in MHz of the highest state (AMD, 0 = default)")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-power", _o.ocPower, "Power limit in W, or in percent of the default (75%)")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-fan", _o.ocFan, "Fan speed in percent, or auto")
        ->delimiter(',')
        ->group(c_groupOc);
    _app.add_option("--oc-profiles", _o.ocProfiles,
            "Named overclocking profiles, a JSON object (the oc-profiles object of the "
            "configuration file)")
        ->group(c_groupOc);
    _app.add_flag("--oc-tune{all}", _o.ocTune,
            "Find the best memory clock and the most efficient core clock of each GPU, save them "
            "for --oc-profile tuned and exit. --oc-tune=core only tunes the core clock, which is "
            "safe")
        ->check(CLI::IsMember({"all", "core"}))
        ->group(c_groupOc);
    _app.add_flag("--oc-reset", _o.ocReset,
            "Set the stock clocks, power limits and fan control of the GPUs and exit, e.g. "
            "after hashwarp was killed while overclocking")
        ->group(c_groupOc);
    _app.add_option("--oc-tune-file", _o.ocTuneFile,
            "Tuning results (default hashwarp-oc.json next to the configuration file)")
        ->group(c_groupOc);

    // Logging and display
    _app.add_option("--log-file", _o.logFile, "Also write the log to this file")
        ->group(c_groupLogging);
    _app.add_option("--log-max-size", _o.logMaxSize,
            "Rotate the log file when it reaches this many MB (0 = never)")
        ->group(c_groupLogging);
    _app.add_option("--log-keep", _o.logKeep, "Rotated log files to keep")
        ->check(CLI::Range(0, 99))
        ->group(c_groupLogging);
    _app.add_option("--log-level", _o.logLevel, "error, warn, info, debug or trace")
        ->check(CLI::IsMember({"error", "warn", "info", "debug", "trace"}))
        ->group(c_groupLogging);
    _app.add_option("--log-format", _o.logFormat,
            "text, or json: one JSON object per line, on the console and in the log file")
        ->check(CLI::IsMember({"text", "json"}))
        ->group(c_groupLogging);
    _app.add_option("--log-debug", _o.logDebug,
            "Extra debug output (implies --log-level debug): json, per-gpu, connect, "
            "switch, submit, flow")
        ->delimiter(',')
        ->check(CLI::IsMember({"json", "per-gpu", "connect", "switch", "submit", "flow"}))
        ->group(c_groupLogging);
    _app.add_option("-v,--verbosity", _o.legacyVerbosity, "")
        ->check(CLI::Range(0, LOG_NEXT - 1))
        ->group("");
    _app.add_flag("--no-color,--nocolor", _o.noColor, "Don't use colors")->group(c_groupLogging);
    _app.add_flag("--syslog", _o.syslog, "Log without timestamps and colors (for syslog)")
        ->group(c_groupLogging);
    _app.add_flag("--stdout", _o.logStdout, "Log to stdout instead of stderr")
        ->group(c_groupLogging);
    _app.add_option("--display-interval", _o.displayInterval,
            "Seconds between hashrate lines")
        ->check(CLI::Range(1, 1800))
        ->group(c_groupLogging);
    _app.add_flag("--no-hotkeys", _o.noHotkeys, "Don't read keys from the terminal (h for help)")
        ->group(c_groupLogging);
    _app.add_option("--stats-interval", _o.statsInterval,
            "Seconds between per GPU statistics tables (0 = never)")
        ->check(CLI::Range(0, 86400))
        ->group(c_groupLogging);

    // API
    _app.add_option("--api-bind", _o.apiBind,
            "Serve the API on address:port, e.g. 127.0.0.1:21550 (default off)")
        ->group(c_groupApi);
    _app.add_option("--api-port", _o.legacyApiPort, "")
        ->check(CLI::Range(-65535, 65535))
        ->group("");
    _app.add_option("--api-token,--api-password", _o.apiToken,
            "Secret required by the API (required to allow control on non loopback addresses)")
        ->group(c_groupApi);
    _app.add_flag("--api-allow-control", _o.apiAllowControl,
            "Allow API clients to pause, restart and change pools")
        ->group(c_groupApi);
    _app.add_option("--rig", _o.rigs,
            "Show another rig on the dashboard: [name=][token@]host[:port] (port 21550 by "
            "default). Repeat it, or separate rigs with commas")
        ->delimiter(',')
        ->group(c_groupApi);
    _app.add_flag("--monitor", _o.monitor,
            "Don't mine: only serve the dashboard of the --rig rigs (needs --api-bind)")
        ->group(c_groupApi);

    // Other
    _app.set_config("-c,--config", "", "Read options from a JSON file (keys are option names)")
        ->group(c_groupMisc);
    _app.add_option("--save-config", _o.saveConfig,
            "Write the options given to this JSON file and exit")
        ->group(c_groupMisc);
    _app.add_option("-M,-Z,--benchmark,--simulation", _o.benchmarkBlock,
            "Benchmark offline on the DAG of this block number")
        ->default_str("")
        ->group(c_groupMisc);
    _app.add_option("--selftest", _o.selftestSeconds,
            "Check every GPU's results on the CPU offline for up to this many seconds "
            "(default 30) and exit with 0 if all are correct")
        ->expected(0, 1)
        ->default_str("30")
        ->group(c_groupMisc);
    _app.add_flag("--exit", _o.exitOnError, "Exit when a GPU fails instead of carrying on")
        ->group(c_groupMisc);

    // Empty lists have no meaningful default to show
    for (CLI::Option* opt : _app.get_options())
        if (opt->get_default_str() == "{}" || opt->get_default_str() == "[]")
            opt->default_str("");

    _app.footer(
        "\nExamples:\n"
        "  hashwarp --pool pool.example.com:3333 --wallet 0xYourAddress --worker rig1\n"
        "  hashwarp --pool pool.example.com:4443 --tls --wallet 0xYourAddress\n"
        "  hashwarp --pool a.example.com:3333,b.example.com:3333 --wallet 0xYourAddress\n"
        "  hashwarp --solo                  getwork on a local node "
        "(parallaxd --http --http.api eth,net,web3,miner --mine)\n"
        "  hashwarp --config hashwarp.json  options from a file "
        "(see --save-config)\n"
        "  hashwarp --list-devices\n"
        "\nRun without arguments to use hashwarp.json next to the executable, or to create it.");
}

std::vector<std::string> validateOptions(Options& _o)
{
    std::vector<std::string> warnings;
    validateOc(_o);

    if (!_o.wallet.empty() && !isAddress(_o.wallet))
        warnings.push_back("Wallet " + _o.wallet +
                           " doesn't look like a Parallax address (0x followed by 40 hex digits)");

    checkPerGpu(_o.cudaGridSize, {}, 1, 131072, "cuda-grid-size");
    checkPerGpu(_o.cudaBlockSize, {32, 64, 128, 256}, 0, 0, "cuda-block-size");
    checkPerGpu(_o.cudaStreams, {}, 1, 99, "cuda-streams");
    checkPerGpu(_o.clLocalWork, {64, 128, 256}, 0, 0, "cl-local-work");
    checkPerGpu(_o.clGlobalWork, {}, 1, 1u << 22, "cl-global-work");  // x 256 fits 32 bits
    checkPerGpu(_o.tempStop, {}, 30, 100, "temp-stop");
    checkPerGpu(_o.tempResume, {}, 20, 100, "temp-resume");
    checkPerGpu(_o.memTempStop, {}, 40, 120, "mem-temp-stop");
    checkPerGpu(_o.memTempResume, {}, 30, 120, "mem-temp-resume");
    for (size_t i = 0; i < std::max(_o.tempStop.size(), _o.tempResume.size()); i++)
    {
        unsigned stop = perGpu(_o.tempStop, i);
        unsigned resume = perGpu(_o.tempResume, i);
        if (stop && resume && resume >= stop)
            throw std::invalid_argument("--temp-resume must be lower than --temp-stop");
    }
    for (size_t i = 0; i < std::max(_o.memTempStop.size(), _o.memTempResume.size()); i++)
    {
        unsigned stop = perGpu(_o.memTempStop, i);
        unsigned resume = perGpu(_o.memTempResume, i);
        if (stop && resume && resume >= stop)
            throw std::invalid_argument("--mem-temp-resume must be lower than --mem-temp-stop");
    }
    if (_o.watchdogTimeout && _o.watchdogTimeout < 30)
        throw std::invalid_argument("--watchdog-timeout must be at least 30 seconds (or 0)");
    if (_o.shutdownTemp && _o.shutdownTemp < 50)
        throw std::invalid_argument("--shutdown-temp must be at least 50");

    if (!_o.apiBind.empty())
    {
        auto colon = _o.apiBind.rfind(':');
        boost::system::error_code ec;
        if (colon == std::string::npos ||
            (boost::asio::ip::address::from_string(_o.apiBind.substr(0, colon), ec), ec) ||
            !isInteger(_o.apiBind.substr(colon + 1)) ||
            std::abs(std::stoi(_o.apiBind.substr(colon + 1))) > 65535 ||
            std::stoi(_o.apiBind.substr(colon + 1)) == 0)
            throw std::invalid_argument(
                "--api-bind must be address:port, e.g. 127.0.0.1:21550 or 0.0.0.0:21550");
    }

    if (_o.monitor && (_o.apiBind.empty() || _o.rigs.empty()))
        throw std::invalid_argument(
            "--monitor needs --api-bind for the dashboard and the rigs to show with --rig");
    if (!_o.rigs.empty() && _o.apiBind.empty() && !_o.legacyApiPort)
        warnings.push_back("--rig needs --api-bind: the rigs are shown on the dashboard");

    if (_o.soloRequested && _o.solo.empty())
        _o.solo = c_defaultSoloUrl;

    if (!_o.listDevices && _o.benchmarkBlock < 0 && !_o.selftestSeconds && _o.ocTune.empty() &&
        !_o.ocReset && !_o.monitor && _o.saveConfig.empty() &&
        _o.pools.empty() && !_o.soloRequested)
        throw std::invalid_argument(
            "No pool configured: use --pool and --wallet, or --solo to mine on your own node");

    if (!_o.logDebug.empty() || _o.legacyVerbosity)
        if (dev::parseLogLevel(_o.logLevel) < dev::LogLevel::Debug)
            _o.logLevel = "debug";

    return warnings;
}

std::vector<std::string> buildPoolUris(const Options& _o)
{
    std::vector<std::string> uris;

    if (_o.soloRequested)
        uris.push_back(_o.solo.empty() ? c_defaultSoloUrl : _o.solo);

    for (auto pool : _o.pools)
    {
        pool.erase(0, pool.find_first_not_of(" \t"));
        pool.erase(pool.find_last_not_of(" \t") + 1);
        if (pool.empty())
            continue;

        if (pool == "exit")
        {
            uris.push_back(pool);
            continue;
        }

        auto sep = pool.find("://");
        if (sep == std::string::npos)
        {
            // host:port
            if (pool.find(':') == std::string::npos)
                throw std::invalid_argument("Pool " + pool + " has no port, use host:port");
            if (_o.wallet.empty())
                throw std::invalid_argument("--wallet is required to mine on pool " + pool);
            uris.push_back(stratumScheme(_o.esm, _o.tls) + "://" + userInfo(_o) + pool);
            continue;
        }

        // Full URL: add the wallet if it carries no credentials
        std::string scheme = pool.substr(0, sep);
        std::string rest = pool.substr(sep + 3);
        bool isStratum = scheme.compare(0, 7, "stratum") == 0;
        if (isStratum && rest.find('@') == std::string::npos)
        {
            if (_o.wallet.empty())
                throw std::invalid_argument("--wallet is required to mine on pool " + pool);
            pool = scheme + "://" + userInfo(_o) + rest;
        }
        uris.push_back(pool);
    }
    return uris;
}

unsigned debugCategories(const Options& _o)
{
    unsigned ret = _o.legacyVerbosity;
    for (const auto& c : _o.logDebug)
        ret |= c_debugCategories.at(c);
    return ret;
}

unsigned perGpu(const std::vector<unsigned>& _values, size_t _index)
{
    if (_values.empty())
        return 0;
    if (_values.size() == 1)
        return _values[0];
    return _index < _values.size() ? _values[_index] : 0;
}

bool isAddress(const std::string& _wallet)
{
    static const std::regex address("^0x[0-9a-fA-F]{40}$");
    return std::regex_match(_wallet, address);
}

std::string configToJson(const CLI::App& _app)
{
    return _app.config_to_str(false, false);
}

std::string findDefaultConfig()
{
    namespace fs = boost::filesystem;
    for (const fs::path& dir : {fs::path(boost::dll::program_location().parent_path().string()),
             fs::current_path()})
    {
        boost::system::error_code ec;
        fs::path p = dir / "hashwarp.json";
        if (fs::is_regular_file(p, ec))
            return p.string();
    }
    return "";
}

std::string defaultConfigPath()
{
    namespace fs = boost::filesystem;
    fs::path dir = boost::dll::program_location().parent_path().string();
    // Prefer the executable's directory, fall back to the working directory
    boost::system::error_code ec;
    fs::path probe = dir / ".hashwarp-write-test";
    {
        std::ofstream test(probe.string());
        if (!test.is_open())
            dir = fs::current_path();
    }
    fs::remove(probe, ec);
    return (dir / "hashwarp.json").string();
}

}  // namespace hashwarp
