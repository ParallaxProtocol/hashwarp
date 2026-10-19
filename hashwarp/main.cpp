/*
    This file is part of ethminer.

    ethminer is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ethminer is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <CLI/CLI.hpp>

#include <hashwarp/buildinfo.h>
#include <condition_variable>
#include <fstream>
#include <future>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#include <libparallaxcore/Farm.h>
#if ETH_XHASHCL
#include <libxhash-cl/CLMiner.h>
#endif
#if ETH_XHASHCUDA
#include <libxhash-cuda/CUDAMiner.h>
#endif
#if ETH_XHASHCPU
#include <libxhash-cpu/CPUMiner.h>
#endif
#include <libpoolprotocols/PoolCheck.h>
#include <libpoolprotocols/PoolManager.h>
#include <libpoolprotocols/Tls.h>

#if API_CORE
#include <libapicore/ApiServer.h>
#endif

#include "Console.h"
#include "Hotkeys.h"
#include "Oc.h"
#include "OcTune.h"
#include "Options.h"
#include "Wizard.h"

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

using namespace std;
using namespace dev;
using namespace dev::eth;


// Global vars
std::atomic<bool> g_running = {false};
std::atomic<bool> g_interrupted = {false};  // Set by the signal handler
bool g_exitOnError = false;  // Whether or not hashwarp should exit on mining threads errors
std::atomic<int> g_exitCode = {0};

condition_variable g_shouldstop;
boost::asio::io_service g_io_service;  // The IO service itself

#if ETH_DBUS
#include <hashwarp/DBusInt.h>
#endif

namespace
{
enum class GpuVendor
{
    Unknown,
    Nvidia,
    Amd,
    Intel
};

GpuVendor vendorOf(const DeviceDescriptor& _d)
{
    if (_d.cuDetected)
        return GpuVendor::Nvidia;
    switch (_d.clPlatformType)
    {
    case ClPlatformTypeEnum::Nvidia:
        return GpuVendor::Nvidia;
    case ClPlatformTypeEnum::Amd:
    case ClPlatformTypeEnum::Clover:
        return GpuVendor::Amd;
    case ClPlatformTypeEnum::Intel:
        return GpuVendor::Intel;
    default:
        return GpuVendor::Unknown;
    }
}
}  // namespace

class MinerCLI
{
public:
    enum class OperationMode
    {
        None,
        Simulation,
        Mining
    };

    MinerCLI() : m_cliDisplayTimer(g_io_service), m_io_strand(g_io_service)
    {
        // Initialize display timer as sleeper
        m_cliDisplayTimer.expires_from_now(boost::posix_time::pos_infin);
        m_cliDisplayTimer.async_wait(m_io_strand.wrap(boost::bind(
            &MinerCLI::cliDisplayInterval_elapsed, this, boost::asio::placeholders::error)));

        // Start io_service in it's own thread
        m_io_thread = std::thread{boost::bind(&boost::asio::io_service::run, &g_io_service)};

        // Io service is now live and running
        // All components using io_service should post to reference of g_io_service
        // and should not start/stop or even join threads (which heavily time consuming)
    }

    virtual ~MinerCLI()
    {
        m_cliDisplayTimer.cancel();
        g_io_service.stop();
        m_io_thread.join();
    }

    static hashwarp::PoolStatus poolStatus()
    {
        hashwarp::PoolStatus pool;
        pool.connected = PoolManager::p().isConnected();
        auto conn = PoolManager::p().getActiveConnection();
        if (conn)
            pool.host = conn->Host() + ":" + to_string(conn->Port());
        pool.difficulty = PoolManager::p().getCurrentDifficulty();
        pool.epoch = PoolManager::p().getCurrentEpoch();
        pool.block = PoolManager::p().getCurrentBlock();
        return pool;
    }

    static void printReport()
    {
        bool perGpu = g_logFormat == LogFormat::Json || Farm::f().getMiners().size() > 1;
        for (const auto& l : hashwarp::reportLines(Farm::f().Telemetry(), poolStatus(), perGpu))
        {
            dev::LogOutputStream<dev::NoteChannel> log(l.source.c_str());
            log.with("event", l.source == "miner" ? "hashrate" : "gpu");
            for (const auto& f : l.fields)
                log.with(f.first.c_str(), f.second);
            log << l.text;
        }
    }

    static void printStats()
    {
        std::vector<hashwarp::GpuStatus> gpus;
        for (auto const& miner : Farm::f().getMiners())
            gpus.push_back({miner->getDescriptor().name, miner->pausedString()});
        for (const auto& line : hashwarp::statsTable(Farm::f().Telemetry(), gpus, poolStatus()))
            cnote << line;
    }

    // Runs on the io_service thread (like the telemetry it reads)
    static void handleKey(char _key)
    {
        _key = (char)std::tolower((unsigned char)_key);
        if (_key >= '0' && _key <= '9')
        {
            unsigned idx = unsigned(_key - '0');
            auto miner = Farm::f().getMiner(idx);
            if (!miner)
            {
                cnote << "There is no gpu" << idx;
                return;
            }
            if (miner->pauseTest(MinerPauseEnum::PauseDueToAPIRequest))
            {
                miner->resume(MinerPauseEnum::PauseDueToAPIRequest);
                cgpunote(idx) << "Resumed";
            }
            else
            {
                miner->pause(MinerPauseEnum::PauseDueToAPIRequest);
                cgpunote(idx) << "Paused (press " << idx << " again to resume)";
            }
            return;
        }

        switch (_key)
        {
        case 'h':
            cnote << "Keys: " EthWhite "h" EthReset " help, " EthWhite "s" EthReset
                     " statistics, " EthWhite "p" EthReset " pause all, " EthWhite "r" EthReset
                     " resume all, " EthWhite "0-9" EthReset " pause/resume a GPU, " EthWhite
                     "c" EthReset " connection, " EthWhite "l" EthReset " debug log, " EthWhite
                     "q" EthReset " quit";
            break;
        case 's':
            printStats();
            break;
        case 'p':
            for (auto const& miner : Farm::f().getMiners())
                miner->pause(MinerPauseEnum::PauseDueToAPIRequest);
            cnote << "All GPUs paused (press r to resume)";
            break;
        case 'r':
            for (auto const& miner : Farm::f().getMiners())
                miner->resume(MinerPauseEnum::PauseDueToAPIRequest);
            cnote << "All GPUs resumed";
            break;
        case 'c':
        {
            hashwarp::PoolStatus pool = poolStatus();
            auto conn = PoolManager::p().getActiveConnection();
            cnote << (pool.connected ? "Connected to " : "Not connected to ") << pool.host
                  << (conn ? " (" + conn->Scheme() + ", user " + conn->UserDotWorker() + ")" : "")
                  << ", difficulty " << getFormattedHashes(pool.difficulty) << ", epoch "
                  << pool.epoch << (pool.block >= 0 ? ", block " + to_string(pool.block) : "")
                  << ", " << PoolManager::p().getConnectionSwitches() << " pool switches";
            break;
        }
        case 'l':
            g_logLevel = g_logLevel >= LogLevel::Debug ? LogLevel::Info : LogLevel::Debug;
            cnote << "Log level " << logLevelName(g_logLevel);
            break;
        case 'q':
            cnote << "Quitting";
            raise(SIGINT);
            break;
        default:
            break;
        }
    }

    void cliDisplayInterval_elapsed(const boost::system::error_code& ec)
    {
        if (!ec && g_running)
        {
            auto now = std::chrono::steady_clock::now();
            if (m_opts.statsInterval && now - m_lastStats >= std::chrono::seconds(m_opts.statsInterval))
            {
                m_lastStats = now;
                printStats();
            }
            else
                printReport();

#if ETH_DBUS
            dbusint.send(Farm::f().Telemetry().str().c_str());
#endif
            // Resubmit timer
            m_cliDisplayTimer.expires_from_now(boost::posix_time::seconds(m_cliDisplayInterval));
            m_cliDisplayTimer.async_wait(m_io_strand.wrap(boost::bind(
                &MinerCLI::cliDisplayInterval_elapsed, this, boost::asio::placeholders::error)));
        }
    }

    static void signalHandler(int sig)
    {
        switch (sig)
        {
#if defined(__linux__) || defined(__APPLE__)
#define BACKTRACE_MAX_FRAMES 100
        case SIGSEGV:
            static bool in_handler = false;
            if (!in_handler)
            {
                int j, nptrs;
                void* buffer[BACKTRACE_MAX_FRAMES];
                char** symbols;

                in_handler = true;

                cerr << "SIGSEGV encountered ...\n";
                cerr << "stack trace:\n";

                nptrs = backtrace(buffer, BACKTRACE_MAX_FRAMES);
                cerr << "backtrace() returned " << nptrs << " addresses\n";

                symbols = backtrace_symbols(buffer, nptrs);
                if (symbols == NULL)
                {
                    perror("backtrace_symbols()");
                    exit(EXIT_FAILURE);  // Also exit 128 ??
                }
                for (j = 0; j < nptrs; j++)
                    cerr << symbols[j] << "\n";
                free(symbols);

                in_handler = false;
            }
            exit(128);
#undef BACKTRACE_MAX_FRAMES
#endif
        case (999U):
            // Compiler complains about the lack of
            // a case statement in Windows
            // this makes it happy.
            break;
        default:
            // Only async signal safe work here: the main thread polls the flags
            g_interrupted = true;
            g_running = false;
            break;
        }
    }

    /// Parses the command line (and configuration file) and applies the options.
    /// Returns false if hashwarp should exit right away (help, version, --save-config)
    bool validateArgs(int argc, char** argv)
    {
        auto* bi = hashwarp_get_buildinfo();
        CLI::App app;
        app.set_version_flag("-V,--version", string("hashwarp ") + bi->project_version);
        hashwarp::registerOptions(app, m_opts);

        try
        {
            app.parse(argc, argv);
        }
        catch (const CLI::CallForHelp&)
        {
            cout << "hashwarp " << bi->project_version << " - Parallax (XHash) GPU miner" << endl
                 << endl
                 << app.help();
            return false;
        }
        catch (const CLI::CallForAllHelp&)
        {
            cout << "hashwarp " << bi->project_version << " - Parallax (XHash) GPU miner" << endl
                 << endl
                 << app.help("", CLI::AppFormatMode::All);
            return false;
        }
        catch (const CLI::CallForVersion&)
        {
            cout << "hashwarp " << bi->project_version << endl;
            return false;
        }
        catch (const CLI::ParseError& e)
        {
            throw std::invalid_argument(e.what());
        }
        m_opts.soloRequested = app.count("--solo") > 0;
        if (app.count("--selftest") && !m_opts.selftestSeconds)
            m_opts.selftestSeconds = 30;

        if (!m_opts.saveConfig.empty())
        {
            std::ofstream out(m_opts.saveConfig);
            if (!out)
                throw std::runtime_error("Can't write " + m_opts.saveConfig);
            out << hashwarp::configToJson(app);
            cout << "Configuration written to " << m_opts.saveConfig << endl;
            return false;
        }

        std::vector<string> warnings = hashwarp::validateOptions(m_opts);
        applyLogging();
        cnote << EthWhiteBold "hashwarp " << bi->project_version << EthReset " · "
              << bi->system_name << "/" << bi->build_type << "/" << bi->compiler_id;
        if (app.count("--config"))
            cnote << "Configuration " << app.get_config_ptr()->as<std::string>();

        // Farm
        m_FarmSettings.dagLoadMode = m_opts.dagLoadMode;
        m_FarmSettings.noEval = m_opts.noEval;
        m_FarmSettings.ergodicity = m_opts.ergodicity;
        m_FarmSettings.hwMon = m_opts.noHwmon ? 0 : 2;
        if (m_opts.legacyHwmon >= 0)
            m_FarmSettings.hwMon = (unsigned)m_opts.legacyHwmon;
        m_FarmSettings.shutdownTemp = m_opts.shutdownTemp;
        m_FarmSettings.watchdogTimeout = m_opts.watchdogTimeout;
        m_FarmSettings.watchdogRestarts = m_opts.watchdogRestarts;
        m_FarmSettings.watchdogScript = m_opts.watchdogScript;
        auto anySet = [](const std::vector<unsigned>& _v) {
            return std::any_of(_v.begin(), _v.end(), [](unsigned t) { return t != 0; });
        };
        bool tempControl =
            anySet(m_opts.tempStop) || anySet(m_opts.memTempStop) || m_opts.shutdownTemp;
        if (tempControl && !m_FarmSettings.hwMon)
        {
            warnings.push_back("Temperature limits need hardware monitoring: ignoring --no-hwmon");
            m_FarmSettings.hwMon = 2;
        }
        g_exitOnError = m_opts.exitOnError;

        // Miners
        m_CLSettings.noExit = m_opts.clNoExit;
#if ETH_XHASHCUDA
        static const std::map<string, unsigned> schedules = {
            {"auto", 0}, {"spin", 1}, {"yield", 2}, {"sync", 4}};
        m_CUSettings.schedule = schedules.at(m_opts.cudaSchedule);
        m_CUSettings.kernel = m_opts.cudaKernel == "auto" ? -1 : std::stoi(m_opts.cudaKernel);
#endif
        if (m_opts.backend == "cuda")
            m_minerType = MinerType::CUDA;
        else if (m_opts.backend == "opencl")
            m_minerType = MinerType::CL;
        else
            m_minerType = MinerType::Mixed;

        // Pools
        m_PoolSettings.getWorkPollInterval = m_opts.getworkInterval;
        m_PoolSettings.connectionMaxRetries = m_opts.poolRetries;
        m_PoolSettings.delayBeforeRetry = m_opts.retryDelay;
        m_PoolSettings.poolFailoverTimeout = m_opts.failoverTimeout;
        m_PoolSettings.noWorkTimeout = m_opts.workTimeout;
        m_PoolSettings.noResponseTimeout = m_opts.responseTimeout;
        m_PoolSettings.reportHashrate = m_opts.reportHashrate;
        m_cliDisplayInterval = m_opts.displayInterval;

        if (!m_opts.ocTune.empty())
        {
            // Offline, at an easy difficulty: wrong results show up within seconds (a
            // 4090 finds about 5 solutions a second; more of them make the hashrate noisy)
            if (m_opts.benchmarkBlock < 0)
                m_opts.benchmarkBlock = c_selftestBlock;
            m_PoolSettings.simulationDifficulty = 0.005;
            m_FarmSettings.noEval = false;
            m_FarmSettings.hwMon = 2;
        }
        if (m_opts.selftestSeconds)
        {
            // Offline, at an easy difficulty so every GPU finds solutions quickly
            if (m_opts.benchmarkBlock < 0)
                m_opts.benchmarkBlock = c_selftestBlock;
            m_PoolSettings.simulationDifficulty = 0.01;
            m_FarmSettings.noEval = false;
        }
        if (m_opts.benchmarkBlock >= 0)
        {
            m_mode = OperationMode::Simulation;
            m_PoolSettings.benchmarkBlock = (unsigned)m_opts.benchmarkBlock;
            m_PoolSettings.connections.push_back(
                std::shared_ptr<URI>(new URI("simulation://localhost:0", true)));
        }
        else
        {
            m_mode = OperationMode::Mining;
        }

        if (!m_opts.listDevices && m_mode != OperationMode::Simulation)
        {
            std::vector<string> uris = hashwarp::buildPoolUris(m_opts);
            for (size_t i = 0; i < uris.size(); i++)
            {
                std::string url = uris.at(i);
                if (url == "exit")
                {
                    if (i == 0)
                        throw std::invalid_argument(
                            "'exit' failover directive can't be the first pool");
                    url = "stratum+tcp://-:x@exit:0";
                }

                try
                {
                    std::shared_ptr<URI> uri = std::shared_ptr<URI>(new URI(url));
                    if (uri->SecLevel() != dev::SecureLevel::NONE &&
                        uri->HostNameType() != dev::UriHostNameType::Dns && !getenv("SSL_NOVERIFY"))
                    {
                        warnings.push_back(
                            "You have specified host " + uri->Host() + " with encryption enabled.");
                        warnings.push_back("Certificate validation will likely fail");
                    }
                    m_PoolSettings.connections.push_back(uri);
                }
                catch (const std::exception& _ex)
                {
                    string what = _ex.what();
                    throw std::invalid_argument("Bad pool " + uris.at(i) + " : " + what);
                }
            }
        }

#if API_CORE
        applyApi(warnings);
#endif

        for (const auto& w : warnings)
            cwarn << w;
        return true;
    }

    void execute()
    {
#if API_CORE
        if (m_opts.monitor)
        {
            doMonitor();
            return;
        }
#endif
#if ETH_XHASHCL
        if (m_minerType == MinerType::CL || m_minerType == MinerType::Mixed)
            CLMiner::enumDevices(m_DevicesCollection);
#endif
#if ETH_XHASHCUDA
        if (m_minerType == MinerType::CUDA || m_minerType == MinerType::Mixed)
            CUDAMiner::enumDevices(m_DevicesCollection);
#endif
#if ETH_XHASHCPU
        if (m_minerType == MinerType::CPU)
            CPUMiner::enumDevices(m_DevicesCollection);
#endif

        // Can't proceed without any GPU
        if (!m_DevicesCollection.size())
            throw std::runtime_error("No usable mining devices found");

        // If requested list detected devices and exit
        if (m_opts.listDevices)
        {
            listDevices();
            return;
        }

        subscribeDevices();

        // Enable
        g_running = true;

        // Signal traps
#if defined(__linux__) || defined(__APPLE__)
        signal(SIGSEGV, MinerCLI::signalHandler);
#endif
        signal(SIGINT, MinerCLI::signalHandler);
        signal(SIGTERM, MinerCLI::signalHandler);

        // Initialize Farm
        new Farm(m_DevicesCollection, m_FarmSettings, m_CUSettings, m_CLSettings, m_CPSettings);
        if (m_opts.ocReset)
        {
            g_exitCode = resetOc() ? 0 : 1;
            return;
        }
        applyOc();

        // Run Miner
        doMiner();
        Farm::f().restoreOc();
    }

private:
    // --oc-reset: stock settings on every selected GPU, kept after exit
    bool resetOc()
    {
        bool ok = true;
        hashwarp::TuneResults none;
        for (const auto& d : m_DevicesCollection)
        {
            if (d.second.subscriptionType == DeviceSubscriptionTypeEnum::None)
                continue;
            std::string gpu = d.second.name + " (" + d.first + ")", error, note;
            dev::GpuControl* control = Farm::f().gpuControl(d.second, error);
            if (!control)
            {
                clogs(dev::WarnChannel, "oc") << gpu << ": " << error;
                ok = false;
                continue;
            }
            auto errors =
                control->apply(*hashwarp::gpuProfile("stock", none, "", control->limits(), note));
            control->keep();
            for (const auto& e : errors)
                clogs(dev::WarnChannel, "oc") << gpu << ": " << e;
            if (errors.empty())
                clogs(dev::NoteChannel, "oc") << gpu << ": stock settings";
            ok = ok && errors.empty();
        }
        return ok;
    }

    // Sets the overclocking of the GPUs before mining starts
    void applyOc()
    {
        if (m_ocRequests.empty())
            return;
        if (!m_opts.ocTune.empty())
        {
            clogs(dev::WarnChannel, "oc")
                << "--oc-tune starts from stock settings, the other --oc options are ignored";
            m_ocRequests.clear();
            return;
        }
        // Also when hashwarp exits otherwise (and the watchdog gives up on a hung GPU,
        // which may well be the overclocking's fault)
        std::atexit([]() { Farm::f().restoreOc(); });
        std::at_quick_exit([]() { Farm::f().restoreOc(); });

        hashwarp::TuneResults tuned;
        std::string tuneFile = hashwarp::tuneFilePath(m_opts);
        try
        {
            tuned = hashwarp::loadTuneResults(tuneFile);
        }
        catch (const std::exception& ex)
        {
            clogs(dev::WarnChannel, "oc") << ex.what();
        }
        for (const auto& r : m_ocRequests)
        {
            const DeviceDescriptor& d = m_DevicesCollection.at(r.uniqueId);
            std::string error;
            dev::GpuControl* control = Farm::f().gpuControl(d, error);
            if (!control)
            {
                cgpuwarn(r.gpu) << "Can't overclock: " << error;
                continue;
            }
            dev::OcSettings s;
            std::string note;
            if (!r.request.gpuProfile.empty())
            {
                auto profile = hashwarp::gpuProfile(
                    r.request.gpuProfile, tuned, control->id(), control->limits(), note);
                if (!profile)
                    cgpuwarn(r.gpu) << "Profile " << r.request.gpuProfile << " " << note;
                else
                    s = *profile;
            }
            s.merge(r.request.settings);
            if (s.empty())
                continue;
            auto errors = control->apply(s);
            cgpunote(r.gpu) << "Overclock " << s.str() << (note.empty() ? "" : " (" + note + ")");
            for (const auto& e : errors)
                cgpuwarn(r.gpu) << e;
        }
    }

    void applyLogging()
    {
        g_logLevel = dev::parseLogLevel(m_opts.logLevel);
        g_logFormat = dev::parseLogFormat(m_opts.logFormat);
        g_logOptions = hashwarp::debugCategories(m_opts);
        g_logSyslog = g_logSyslog || m_opts.syslog || getenv("SYSLOG");
        g_logNoColor = g_logNoColor || m_opts.noColor || g_logSyslog || getenv("NO_COLOR");
        g_logStdout = m_opts.logStdout;
        if (!m_opts.logFile.empty())
            dev::setLogFile(m_opts.logFile, size_t(m_opts.logMaxSize) * 1024 * 1024, m_opts.logKeep);

#ifndef DEV_BUILD
        if (g_logOptions & (LOG_CONNECT | LOG_SWITCH | LOG_SUBMIT | LOG_PROGRAMFLOW))
            cwarn << "The connect, switch, submit and flow debug output need a build with "
                     "-DDEVBUILD=ON";
#endif
    }

#if API_CORE
    void applyApi(std::vector<string>& _warnings)
    {
        bool legacyReadOnly = false;
        if (!m_opts.apiBind.empty())
        {
            auto colon = m_opts.apiBind.rfind(':');
            m_api_address = m_opts.apiBind.substr(0, colon);
            m_api_port = std::stoi(m_opts.apiBind.substr(colon + 1));
        }
        else if (m_opts.legacyApiPort)
        {
            m_api_address = "127.0.0.1";
            m_api_port = m_opts.legacyApiPort;
        }
        if (!m_api_port)
            return;

        // Legacy: a negative port meant read-only, a positive one allowed control
        if (m_api_port < 0)
        {
            legacyReadOnly = true;
            m_api_port = -m_api_port;
        }
        else if (m_opts.legacyApiPort > 0 && !m_opts.apiAllowControl)
            _warnings.push_back(
                "The API no longer allows control unless --api-allow-control is given");
        m_api_readonly = legacyReadOnly || !m_opts.apiAllowControl;
        m_api_password = m_opts.apiToken;
        for (const auto& spec : m_opts.rigs)
            m_rigs.push_back(hashwarp::api::parseRig(spec));

        // Control lets clients add pools and restart the miner. Never expose
        // that beyond the loopback interface without a secret.
        boost::system::error_code ec;
        auto addr = boost::asio::ip::address::from_string(m_api_address, ec);
        if (!m_api_readonly && m_api_password.empty() && (ec || !addr.is_loopback()))
            throw std::invalid_argument("--api-allow-control on " + m_api_address +
                                        " requires --api-token. Bind to 127.0.0.1 or set a token");
    }
#endif

    void listDevices()
    {
        cout << setiosflags(ios::left) << setw(4) << "Id" << setw(10) << "Pci Id" << setw(8)
             << "Backend" << setw(32) << "Name" << resetiosflags(ios::left) << setw(10)
             << "Memory" << "  Details" << endl;
        cout << setiosflags(ios::left) << setw(4) << "--" << setw(10) << "------" << setw(8)
             << "-------" << setw(32) << "----" << resetiosflags(ios::left) << setw(10)
             << "------" << "  -------" << endl;
        unsigned i = 0;
        for (auto it = m_DevicesCollection.begin(); it != m_DevicesCollection.end(); it++, i++)
        {
            const DeviceDescriptor& d = it->second;
            string backends = d.cuDetected ? (d.clDetected ? "cu,cl" : "cu") : "cl";
            string details;
            if (d.cuDetected)
                details = "SM " + d.cuCompute;
            if (d.clDetected)
                details += (details.empty() ? "" : ", ") + d.clPlatformName + " " +
                           d.clDeviceVersion;
            cout << setiosflags(ios::left) << setw(4) << i << setw(10) << it->first << setw(8)
                 << backends << setw(32) << d.name.substr(0, 31) << resetiosflags(ios::left)
                 << setw(10) << getFormattedMemory((double)d.totalMemory) << "  " << details
                 << endl;
        }
    }

    // Decides which backend mines on which device and applies per GPU settings
    void subscribeDevices()
    {
        auto nth = [this](unsigned _id) -> DeviceDescriptor& {
            if (_id >= m_DevicesCollection.size())
                throw std::invalid_argument("GPU id " + to_string(_id) +
                                            " doesn't exist. See --list-devices");
            auto it = m_DevicesCollection.begin();
            std::advance(it, _id);
            return it->second;
        };

        // Legacy explicit per backend lists
#if ETH_XHASHCUDA
        for (auto id : m_opts.cudaDevices)
        {
            auto& d = nth(id);
            if (!d.cuDetected)
                throw std::invalid_argument("GPU " + to_string(id) + " is not a CUDA device");
            d.subscriptionType = DeviceSubscriptionTypeEnum::Cuda;
        }
#endif
#if ETH_XHASHCL
        for (auto id : m_opts.openclDevices)
        {
            auto& d = nth(id);
            if (!d.clDetected)
                throw std::invalid_argument("GPU " + to_string(id) + " is not an OpenCL device");
            if (d.subscriptionType != DeviceSubscriptionTypeEnum::None)
                throw std::invalid_argument("GPU " + to_string(id) + " is already used by CUDA");
            d.subscriptionType = DeviceSubscriptionTypeEnum::OpenCL;
        }
#endif

        // GPUs to mine on, in the order per GPU option lists refer to them
        std::vector<unsigned> selected = m_opts.gpuIds;
        if (selected.empty())
        {
            for (auto id : m_opts.cudaDevices)
                selected.push_back(id);
            for (auto id : m_opts.openclDevices)
                if (std::find(selected.begin(), selected.end(), id) == selected.end())
                    selected.push_back(id);
        }
        bool explicitSelection = !selected.empty();
        if (selected.empty())
            for (unsigned i = 0; i < m_DevicesCollection.size(); i++)
                selected.push_back(i);

        for (size_t k = 0; k < selected.size(); k++)
        {
            auto& d = nth(selected[k]);
            GpuVendor vendor = vendorOf(d);
            if ((vendor == GpuVendor::Nvidia && m_opts.disableNvidia) ||
                (vendor == GpuVendor::Amd && m_opts.disableAmd) ||
                (vendor == GpuVendor::Intel && m_opts.disableIntel))
            {
                if (explicitSelection)
                    cwarn << "gpu" << selected[k] << " is selected but its vendor is disabled";
                continue;
            }
            if (d.subscriptionType == DeviceSubscriptionTypeEnum::None)
            {
#if ETH_XHASHCUDA
                if (d.cuDetected && m_minerType != MinerType::CL)
                    d.subscriptionType = DeviceSubscriptionTypeEnum::Cuda;
#endif
#if ETH_XHASHCL
                if (d.subscriptionType == DeviceSubscriptionTypeEnum::None && d.clDetected &&
                    m_minerType != MinerType::CUDA)
                    d.subscriptionType = DeviceSubscriptionTypeEnum::OpenCL;
#endif
#if ETH_XHASHCPU
                if (m_minerType == MinerType::CPU)
                    d.subscriptionType = DeviceSubscriptionTypeEnum::Cpu;
#endif
            }
            if (d.subscriptionType == DeviceSubscriptionTypeEnum::None)
            {
                if (explicitSelection)
                    cwarn << "GPU " << selected[k] << " can't be used with --backend "
                          << m_opts.backend;
                continue;
            }

            DeviceSettings& s = d.settings;
            s.cuGridSize = hashwarp::perGpu(m_opts.cudaGridSize, k);
            s.cuBlockSize = hashwarp::perGpu(m_opts.cudaBlockSize, k);
            s.cuStreams = hashwarp::perGpu(m_opts.cudaStreams, k);
            s.clGlobalWorkMultiplier = hashwarp::perGpu(m_opts.clGlobalWork, k);
            s.clLocalWorkSize = hashwarp::perGpu(m_opts.clLocalWork, k);
            s.tempStop = hashwarp::perGpu(m_opts.tempStop, k);
            s.tempResume = hashwarp::perGpu(m_opts.tempResume, k);
            if (s.tempStop && !s.tempResume)
                s.tempResume = s.tempStop - 15;
            s.memTempStop = hashwarp::perGpu(m_opts.memTempStop, k);
            s.memTempResume = hashwarp::perGpu(m_opts.memTempResume, k);
            if (s.memTempStop && !s.memTempResume)
                s.memTempResume = s.memTempStop - 10;

            auto oc = hashwarp::ocRequest(m_opts, k);
            if (!oc.empty())
                m_ocRequests.push_back({selected[k], d.uniqueId, oc});
        }

        // Count of subscribed devices
        int subscribedDevices = 0;
        for (auto it = m_DevicesCollection.begin(); it != m_DevicesCollection.end(); it++)
        {
            if (it->second.subscriptionType != DeviceSubscriptionTypeEnum::None)
                subscribedDevices++;
        }

        // If no OpenCL and/or CUDA devices subscribed then throw error
        if (!subscribedDevices)
            throw std::runtime_error("No mining device selected. Aborting ...");
    }

    void doMiner()
    {
        new PoolManager(m_PoolSettings);
        if (m_mode != OperationMode::Simulation)
            for (auto conn : m_PoolSettings.connections)
                cdebug << "Configured pool " << conn->Host() + ":" + to_string(conn->Port());

#if API_CORE

        ApiServer api(m_api_address, m_api_readonly ? -m_api_port : m_api_port, m_api_password);
        std::unique_ptr<hashwarp::api::RigPoller> rigs;
        if (m_api_port && !m_rigs.empty())
        {
            rigs.reset(new hashwarp::api::RigPoller(g_io_service, m_rigs));
            api.setRigs(rigs.get(), true);
            rigs->start();
        }
        if (m_api_port)
            api.start();

#endif

        // Start PoolManager
        PoolManager::p().start();

        // Warns when the pool has the majority of the blocks (not offline nor solo)
        std::unique_ptr<PoolCheck> poolCheck;
        if (m_mode == OperationMode::Mining && !m_opts.noPoolCheck && m_opts.solo.empty() &&
            m_opts.ocTune.empty() && !m_opts.selftestSeconds)
        {
            poolCheck.reset(new PoolCheck());
            poolCheck->start();
        }

        // Initialize display timer as sleeper with proper interval
        m_cliDisplayTimer.expires_from_now(boost::posix_time::seconds(m_cliDisplayInterval));
        m_cliDisplayTimer.async_wait(m_io_strand.wrap(boost::bind(
            &MinerCLI::cliDisplayInterval_elapsed, this, boost::asio::placeholders::error)));

        // Interactive keys
        std::unique_ptr<hashwarp::Hotkeys> hotkeys;
        if (!m_opts.noHotkeys)
        {
            hotkeys.reset(new hashwarp::Hotkeys(
                [](char _key) { g_io_service.post([_key]() { handleKey(_key); }); }));
            if (hotkeys->active())
                cnote << "Press " EthWhite "h" EthReset " for the keys";
        }

        if (!m_opts.ocTune.empty())
        {
            g_exitCode = runOcTune() ? 0 : 1;
            g_running = false;
        }

        // Stay in non-busy wait till signals arrive (or the self-test ends)
        unique_lock<mutex> clilock(m_climtx);
        auto selftestEnd =
            std::chrono::steady_clock::now() + std::chrono::seconds(m_opts.selftestSeconds);
        while (g_running)
        {
            g_shouldstop.wait_for(clilock, std::chrono::seconds(1));
            if (!m_opts.selftestSeconds)
                continue;
            auto now = std::chrono::steady_clock::now();
            if (now >= selftestEnd ||
                (onIoThread(selftestDone) &&
                    now >= selftestEnd - std::chrono::seconds(m_opts.selftestSeconds) +
                               std::chrono::seconds(15)))
                break;
        }
        if (g_interrupted)
            cnote << "Stopping (Ctrl+C)";
        hotkeys.reset();
        if (m_opts.selftestSeconds)
        {
            g_running = false;
            g_exitCode = onIoThread(selftestReport) ? 0 : 1;
        }

#if API_CORE

        // Stop Api server
        if (api.isRunning())
            api.stop();
        if (rigs)
            rigs->stop();

#endif
        poolCheck.reset();
        if (PoolManager::p().isRunning())
            PoolManager::p().stop();

        cnote << "Stopped";
        return;
    }

#if API_CORE
    // --monitor: serves the dashboard of the other rigs until stopped, without mining
    void doMonitor()
    {
        g_running = true;
        signal(SIGINT, MinerCLI::signalHandler);
        signal(SIGTERM, MinerCLI::signalHandler);

        hashwarp::api::RigPoller rigs(g_io_service, m_rigs);
        // Read only: there is no miner to control here
        ApiServer api(m_api_address, -m_api_port, m_api_password);
        api.setRigs(&rigs, false);
        rigs.start();
        api.start();
        if (!api.isRunning())
            return;
        cnote << "Monitoring " << m_rigs.size() << (m_rigs.size() == 1 ? " rig" : " rigs")
              << " · dashboard on http://"
              << (m_api_address == "0.0.0.0" ? std::string("localhost") : m_api_address) << ":"
              << m_api_port;

        unique_lock<mutex> clilock(m_climtx);
        while (g_running)
            g_shouldstop.wait_for(clilock, std::chrono::seconds(1));
        if (g_interrupted)
            cnote << "Stopping (Ctrl+C)";
        api.stop();
        rigs.stop();
        cnote << "Stopped";
    }
#endif

    // --oc-tune: searches each GPU's settings while mining offline, saves them and
    // returns whether every GPU could be tuned. The GPUs go back to their settings on exit
    bool runOcTune()
    {
        using clock = std::chrono::steady_clock;
        const auto settle = std::chrono::seconds(10);
        const auto sampleEvery = std::chrono::seconds(5);
        const size_t samples = 4, maxSamples = 8;  // the median of them
        const double unsteady = 0.03;  // spread that takes more samples
        bool memory = m_opts.ocTune == "all";

clogs(dev::NoteChannel, "tune")
            << EthWhiteBold "Tuning the " << (memory ? "memory and core clocks" : "core clock")
            << EthReset " · takes " << (memory ? "10 to 20" : "about 5")
            << " minutes, the GPUs mine offline · Ctrl+C stops without saving";
        std::atexit([]() { Farm::f().restoreOc(); });
        std::at_quick_exit([]() { Farm::f().restoreOc(); });

        // Every GPU hashing: DAG built, kernels tuned
        auto deadline = clock::now() + std::chrono::minutes(5);
        while (g_running && clock::now() < deadline)
        {
            bool all = onIoThread([]() {
                auto miners = Farm::f().getMiners();
                const TelemetryType& t = Farm::f().Telemetry();
                if (miners.empty() || t.miners.size() < miners.size())
                    return false;
                for (const auto& m : t.miners)
                    if (m.hashrate <= 0)
                        return false;
                return true;
            });
            if (all)
                break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::this_thread::sleep_for(settle);
        if (!g_running)
            return false;

        struct Tuning
        {
            unsigned index;
            std::string name;
            dev::GpuControl* control;
            std::unique_ptr<hashwarp::OcSearch> search;
            clock::time_point stepStart, lastSample;
            unsigned errorsAtStart = 0;
            std::vector<std::pair<double, double>> samples;  // H/s, W
            bool done = false;
            bool warned = false;
        };
        std::vector<Tuning> gpus;
        hashwarp::TuneResults none, previous;
        std::string path = hashwarp::tuneFilePath(m_opts);
        try
        {
            previous = hashwarp::loadTuneResults(path);
        }
        catch (const std::exception& ex)
        {
            clogs(dev::WarnChannel, "tune") << ex.what() << ", starting a new file";
        }
        bool allTuned = true;
        for (const auto& miner : Farm::f().getMiners())
        {
            Tuning t;
            t.index = miner->Index();
            t.name = miner->getDescriptor().name;
            std::string error;
            t.control = Farm::f().gpuControl(miner->getDescriptor(), error);
            if (!t.control)
            {
                cgpuwarn(t.index) << "Can't be tuned: " << error;
                allTuned = false;
                continue;
            }
            unsigned coreMHz = onIoThread([&]() {
                const TelemetryType& tm = Farm::f().Telemetry();
                return t.index < tm.miners.size() ? tm.miners[t.index].sensors.coreMHz : 0u;
            });
            std::string note;
            auto errors = t.control->apply(
                *hashwarp::gpuProfile("stock", none, "", t.control->limits(), note));
            if (!errors.empty())
            {
                for (const auto& e : errors)
                    cgpuwarn(t.index) << e;
                cgpuwarn(t.index) << "Can't be tuned";
                allTuned = false;
                continue;
            }
            // --oc-tune=core keeps the memory clock tuned before
            dev::OcSettings tunedMemory;
            auto old = previous.find(t.control->id());
            if (!memory && old != previous.end())
                tunedMemory = old->second.settings;
            t.search.reset(
                new hashwarp::OcSearch(t.control->limits(), memory, coreMHz, tunedMemory));
            gpus.push_back(std::move(t));
        }

        auto startStep = [&](Tuning& _t) {
            auto settings = _t.search->next();
            if (!settings)
            {
                _t.done = true;
                return;
            }
            for (const auto& e : _t.control->apply(*settings))
                cgpuwarn(_t.index) << e;
            cgpunote(_t.index) << "Trying " << _t.search->phase();
            _t.stepStart = _t.lastSample = clock::now();
            _t.samples.clear();
            auto s = Farm::f().getSolutions(_t.index);
            _t.errorsAtStart = s.failed + s.rejected;
        };
        for (auto& t : gpus)
            startStep(t);

        auto pending = [&]() {
            return std::any_of(gpus.begin(), gpus.end(), [](const Tuning& _t) { return !_t.done; });
        };
        while (g_running && pending())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = clock::now();
            for (auto& t : gpus)
            {
                if (t.done || now - t.stepStart < settle || now - t.lastSample < sampleEvery)
                    continue;
                t.lastSample = now;
                t.samples.push_back(onIoThread([&]() {
                    const TelemetryType& tm = Farm::f().Telemetry();
                    if (t.index >= tm.miners.size())
                        return std::make_pair(0.0, 0.0);
                    return std::make_pair(
                        (double)tm.miners[t.index].hashrate, tm.miners[t.index].sensors.powerW);
                }));
                if (t.samples.size() < samples)
                    continue;
                // Medians: a sample can catch the GPU busy with something else
                std::vector<double> hs, ws;
                bool stopped = false;
                for (const auto& sample : t.samples)
                {
                    hs.push_back(sample.first);
                    ws.push_back(sample.second);
                    stopped = stopped || sample.first <= 0;
                }
                auto median = [](std::vector<double>& _v) {
                    std::sort(_v.begin(), _v.end());
                    size_t n = _v.size();
                    return n % 2 ? _v[n / 2] : (_v[n / 2 - 1] + _v[n / 2]) / 2;
                };
                double h = median(hs), w = median(ws);
                if (h > 0 && (hs.back() - hs.front()) / h > unsteady &&
                    t.samples.size() < maxSamples)
                {
                    if (!t.warned)
                        cgpuwarn(t.index) << "The hashrate is unsteady, measuring longer. Is "
                                             "something else using the GPU (a desktop, video)?";
                    t.warned = true;
                    continue;
                }
                auto s = Farm::f().getSolutions(t.index);
                unsigned wrong = s.failed + s.rejected - t.errorsAtStart;
                cgpunote(t.index)
                        .with("event", "tune_step")
                        .with("step", t.search->phase())
                        .with("hashrate", h)
                        .with("power", w)
                        .with("wrong", wrong)
                    << t.search->phase() << ": " << getFormattedHashes(h) << "/s · "
                    << (int)(w + 0.5) << " W"
                    << (wrong ? " · " + std::to_string(wrong) + " wrong results" : "")
                    << (stopped ? " · stopped hashing" : "");
                t.search->report(h, w, wrong > 0 || stopped);
                startStep(t);
            }
        }
        if (!g_running)
        {
            clogs(dev::WarnChannel, "tune") << "Stopped, nothing saved";
            return false;
        }

        hashwarp::TuneResults results = previous;
clogs(dev::NoteChannel, "tune") << EthWhiteBold "Results" EthReset;
        auto efficiency = [](double _h, double _w) {
            return _w > 0 ? " · " + getFormattedHashes(_h / _w) + "/J" : std::string();
        };
        for (const auto& t : gpus)
        {
            const auto& s = *t.search;
            std::string stock = getFormattedHashes(s.baseHashrate()) + "/s · " +
                                std::to_string((int)(s.basePowerW() + 0.5)) + " W" +
                                efficiency(s.baseHashrate(), s.basePowerW());
            if (!s.found())
            {
                cgpuwarn(t.index) << "Stock settings are best: " << stock;
                allTuned = false;
                continue;
            }
            cgpunote(t.index)
                    .with("event", "tuned")
                    .with("settings", s.best().str())
                    .with("hashrate", s.bestHashrate())
                    .with("power", s.bestPowerW())
                << EthLime "✔ " EthReset << s.best().str() << ": "
                << getFormattedHashes(s.bestHashrate()) << "/s · " << (int)(s.bestPowerW() + 0.5)
                << " W" << efficiency(s.bestHashrate(), s.bestPowerW()) << " (stock " << stock
                << ")";
            hashwarp::TunedGpu r;
            r.name = t.name;
            r.mode = m_opts.ocTune;
            r.settings = s.best();
            // A stock memory clock isn't a setting: applying it would undo another tool's
            if (r.settings.memOffset == 0)
                r.settings.memOffset.reset();
            if (r.settings.memClock == t.control->limits().memStateClock)
                r.settings.memClock.reset();
            r.hashrate = s.bestHashrate();
            r.powerW = s.bestPowerW();
            auto old = previous.find(t.control->id());
            if (!memory && old != previous.end() && old->second.mode == "all")
                r.mode = "all";  // the memory clock tuned before, which the search kept
            results[t.control->id()] = r;
        }
        try
        {
            hashwarp::saveTuneResults(path, results);
clogs(dev::NoteChannel, "tune")
                << "Saved to " << path << ". Mine with --oc-profile tuned"
                << (memory ? " (or efficient for the core clock only)" : " or efficient");
        }
        catch (const std::exception& ex)
        {
            cerror << ex.what();
            return false;
        }
        return allTuned;
    }

    // Runs _f on the io_service thread (which owns the telemetry) and returns its result
    template <class F>
    static auto onIoThread(F _f) -> decltype(_f())
    {
        std::promise<decltype(_f())> result;
        g_io_service.post([&]() { result.set_value(_f()); });
        return result.get_future().get();
    }

    // Whether every GPU found enough solutions and has a measured hashrate
    static bool selftestDone()
    {
        auto miners = Farm::f().getMiners();
        if (miners.empty())
            return false;
        const TelemetryType& t = Farm::f().Telemetry();
        for (auto const& miner : miners)
        {
            unsigned i = miner->Index();
            auto s = Farm::f().getSolutions(i);
            if (s.failed || s.rejected)
                return true;  // Already failed
            if (s.accepted < c_selftestSolutions || i >= t.miners.size() ||
                t.miners[i].hashrate60 <= 0)
                return false;
        }
        return true;
    }

    // Prints the self-test verdict of each GPU, returns whether all passed
    static bool selftestReport()
    {
        bool allPassed = true;
        const TelemetryType& t = Farm::f().Telemetry();
        cnote << EthWhiteBold "Self-test results" EthReset;
        for (auto const& miner : Farm::f().getMiners())
        {
            unsigned i = miner->Index();
            auto s = Farm::f().getSolutions(i);
            bool passed = s.accepted > 0 && !s.failed && !s.rejected;
            allPassed = allPassed && passed;
            float hr = i < t.miners.size() ? t.miners[i].hashrate60 : 0;
            if (passed)
                cgpunote(i) << EthLime "✔ PASS" EthReset " " << miner->getDescriptor().name << " · "
                            << s.accepted << " solutions verified · " << getFormattedHashes(hr)
                            << "/s";
            else
                cgpuerror(i) << "✘ FAIL " << miner->getDescriptor().name << " · " << s.accepted
                             << " correct, " << s.failed + s.rejected << " wrong"
                             << (s.accepted ? "" : " · no solution found");
        }
        return allPassed;
    }

    static const unsigned c_selftestSolutions = 5;
    static const unsigned c_selftestBlock = 60480;  // Epoch 84

    // Global boost's io_service
    std::thread m_io_thread;                        // The IO service thread
    boost::asio::deadline_timer m_cliDisplayTimer;  // The timer which ticks display lines
    boost::asio::io_service::strand m_io_strand;    // A strand to serialize posts in
                                                    // multithreaded environment

    hashwarp::Options m_opts;  // Options from the command line and configuration file

    // Overclocking of the selected GPUs
    struct OcGpu
    {
        unsigned gpu;  // index of --list-devices
        std::string uniqueId;
        hashwarp::OcRequest request;
    };
    std::vector<OcGpu> m_ocRequests;
    std::chrono::steady_clock::time_point m_lastStats = std::chrono::steady_clock::now();

    // Physical Mining Devices descriptor
    std::map<std::string, DeviceDescriptor> m_DevicesCollection = {};

    // Mining options
    MinerType m_minerType = MinerType::Mixed;
    OperationMode m_mode = OperationMode::None;

    FarmSettings m_FarmSettings;  // Operating settings for Farm
    PoolSettings m_PoolSettings;  // Operating settings for PoolManager
    CLSettings m_CLSettings;      // Operating settings for CL Miners
    CUSettings m_CUSettings;      // Operating settings for CUDA Miners
    CPSettings m_CPSettings;      // Operating settings for CPU Miners

    // -- CLI Interface related params
    unsigned m_cliDisplayInterval = 10;  // Display stats/info on cli interface every this number of seconds

    // -- CLI Flow control
    mutex m_climtx;

#if API_CORE
    // -- API and Http interfaces related params
    string m_api_address = "127.0.0.1";  // API interface binding address
    int m_api_port = 0;                  // API interface binding port (0 = off)
    bool m_api_readonly = true;          // Whether clients may control the miner
    string m_api_password;               // API interface write protection password
    std::vector<hashwarp::api::RigAddress> m_rigs;  // other rigs shown on the dashboard
#endif

#if ETH_DBUS
    DBusInt dbusint;
#endif
};

int main(int argc, char** argv)
{
    // Return values
    // 0 - Normal exit
    // 1 - Invalid/Insufficient command line arguments
    // 2 - Runtime error
    // 3 - Other exceptions
    // 4 - Possible corruption

    // On Windows it switches the console to UTF-8
    g_logUtf8 = dev::consoleUtf8();

    // Without arguments use the default configuration file, or create one
    std::vector<std::string> args(argv, argv + argc);
    if (argc < 2)
    {
#if defined(_WIN32)
        // Most likely started from the explorer: keep the window open on exit
        if (hashwarp::isInteractive())
            std::atexit([]() {
                cout << endl << "Press Enter to exit" << endl;
                std::string line;
                std::getline(std::cin, line);
            });
#endif
        std::string config = hashwarp::findDefaultConfig();
        if (config.empty())
        {
            if (!hashwarp::isInteractive())
            {
                cerr << "No arguments and no hashwarp.json configuration found." << endl
                     << "Try 'hashwarp --help' to get a list of arguments." << endl
                     << endl;
                return 1;
            }
            config = hashwarp::defaultConfigPath();
            try
            {
                // The pools' shares of the blocks, to list a majority pool last
                std::unique_ptr<NetworkShares> shares;
                try
                {
                    shares.reset(new NetworkShares(parseNetworkShares(
                        httpsGet(c_statusHost, c_statusPath, std::chrono::seconds(5)))));
                }
                catch (const std::exception&)
                {
                    // Listed as they are
                }
                if (!hashwarp::runWizard(config, std::cin, cout, shares.get()))
                    return 0;
            }
            catch (const std::exception& ex)
            {
                cerr << "Error: " << ex.what() << endl;
                return 2;
            }
        }
        args.push_back("--config");
        args.push_back(config);
    }
    std::vector<char*> cargs;
    for (auto& a : args)
        cargs.push_back(&a[0]);

    try
    {
        MinerCLI cli;

        try
        {
            // Set env vars controlling GPU driver behavior.
            setenv("GPU_MAX_HEAP_SIZE", "100");
            setenv("GPU_MAX_ALLOC_PERCENT", "100");
            setenv("GPU_SINGLE_ALLOC_PERCENT", "100");

            // Argument validation either throws exception
            // or returns false which means do not continue
            if (!cli.validateArgs((int)cargs.size(), cargs.data()))
                return 0;

#if defined(_WIN32)
            if (!g_logNoColor)
            {
                g_logNoColor = true;
                // Set output mode to handle virtual terminal sequences
                // Only works on Windows 10, but most users should use it anyway
                HANDLE hOut = GetStdHandle(g_logStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
                if (hOut != INVALID_HANDLE_VALUE)
                {
                    DWORD dwMode = 0;
                    if (GetConsoleMode(hOut, &dwMode))
                    {
                        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                        if (SetConsoleMode(hOut, dwMode))
                            g_logNoColor = false;
                    }
                }
            }
#endif

            cli.execute();
            return g_exitCode;
        }
        catch (std::invalid_argument& ex1)
        {
            cerr << "Error: " << ex1.what() << endl
                 << "Try hashwarp --help to get an explained list of arguments." << endl
                 << endl;
            return 1;
        }
        catch (std::runtime_error& ex2)
        {
            cerr << "Error: " << ex2.what() << endl << endl;
            return 2;
        }
        catch (std::exception& ex3)
        {
            cerr << "Error: " << ex3.what() << endl << endl;
            return 3;
        }
        catch (...)
        {
            cerr << "Error: Unknown failure occurred. Possible memory corruption." << endl << endl;
            return 4;
        }
    }
    catch (const std::exception& ex)
    {
        cerr << "Could not initialize CLI interface " << endl
             << "Error: " << ex.what() << endl
             << endl;
        return 4;
    }
}
