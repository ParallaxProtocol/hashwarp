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

// Use XHash epoch context for Parallax (XHash) support
#include <xhash/global_context.hpp>
#include <xhash/xhash.hpp>

namespace dev
{
namespace eth
{
Farm* Farm::m_this = nullptr;

Farm::Farm(std::map<std::string, DeviceDescriptor>& _DevicesCollection, FarmSettings _settings,
    CUSettings _CUSettings, CLSettings _CLSettings, CPSettings _CPSettings)
  : m_Settings(std::move(_settings)),
    m_CUSettings(std::move(_CUSettings)),
    m_CLSettings(std::move(_CLSettings)),
    m_CPSettings(std::move(_CPSettings)),
    m_io_strand(g_io_service),
    m_collectTimer(g_io_service),
    m_DevicesCollection(_DevicesCollection)
{
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::Farm() begin");

    m_this = this;

    // Init HWMON if needed
    if (m_Settings.hwMon)
    {
        m_telemetry.hwmon = true;

#if defined(__linux)
        bool need_sysfsh = false;
#else
        bool need_adlh = false;
#endif
        bool need_nvmlh = false;

        // Scan devices collection to identify which hw monitors to initialize
        for (auto it = m_DevicesCollection.begin(); it != m_DevicesCollection.end(); it++)
        {
            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::Cuda)
            {
                need_nvmlh = true;
                continue;
            }
            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::OpenCL)
            {
                if (it->second.clPlatformType == ClPlatformTypeEnum::Nvidia)
                {
                    need_nvmlh = true;
                    continue;
                }
                if (it->second.clPlatformType == ClPlatformTypeEnum::Amd)
                {
#if defined(__linux)
                    need_sysfsh = true;
#else
                    need_adlh = true;
#endif
                    continue;
                }
            }
        }

#if defined(__linux)
        if (need_sysfsh)
            initSysfs();
#else
        if (need_adlh)
            adlh = wrap_adl_create();
        if (adlh)
        {
            // Build Pci identification as done in miners.
            for (int i = 0; i < adlh->adl_gpucount; i++)
            {
                std::ostringstream oss;
                std::string uniqueId;
                oss << std::setfill('0') << std::setw(2) << std::hex
                    << (unsigned int)adlh->devs[adlh->phys_logi_device_id[i]].iBusNumber << ":"
                    << std::setw(2)
                    << (unsigned int)(adlh->devs[adlh->phys_logi_device_id[i]].iDeviceNumber)
                    << ".0";
                uniqueId = oss.str();
                map_adl_handle[uniqueId] = i;
            }
        }

#endif
        if (need_nvmlh)
            initNvml();
    }

    // Initialize nonce_scrambler
    shuffle();

    m_lastCollect = std::chrono::steady_clock::now().time_since_epoch().count();
    if (m_Settings.watchdogTimeout)
        m_hangWatchdog = std::thread([this]() { hangWatchdogLoop(); });

    // Start data collector timer
    // It should work for the whole lifetime of Farm
    // regardless it's mining state
    m_collectTimer.expires_from_now(boost::posix_time::milliseconds(m_collectInterval));
    m_collectTimer.async_wait(
        m_io_strand.wrap(boost::bind(&Farm::collectData, this, boost::asio::placeholders::error)));

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::Farm() end");
}

Farm::~Farm()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::~Farm() begin");

    // Stop data collector (before monitors !!!)
    m_collectTimer.cancel();

    if (m_hangWatchdog.joinable())
    {
        {
            std::lock_guard<std::mutex> l(x_hangWatchdog);
            m_hangWatchdogStopping = true;
        }
        m_hangWatchdogStop.notify_all();
        m_hangWatchdog.join();
    }

    restoreOc();
    m_oc.clear();

    // Deinit HWMON
#if defined(__linux)
    if (sysfsh)
        wrap_amdsysfs_destroy(sysfsh);
#else
    if (adlh)
        wrap_adl_destroy(adlh);
#endif
    if (nvmlh)
        wrap_nvml_destroy(nvmlh);

    // Stop mining (if needed)
    if (m_isMining.load(std::memory_order_relaxed))
        stop();

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::~Farm() end");
}

/**
 * @brief Randomizes the nonce scrambler
 */
void Farm::shuffle()
{
    // Given that all nonces are equally likely to solve the problem
    // we could reasonably always start the nonce search ranges
    // at a fixed place, but that would be boring. Provide a once
    // per run randomized start place, without creating much overhead.
    random_device engine;
    m_nonce_scrambler = uniform_int_distribution<uint64_t>()(engine);
}

void Farm::setWork(WorkPackage const& _newWp)
{
    // Set work to each miner giving it's own starting nonce
    Guard l(x_minerWork);

    // Retrieve appropriate EpochContext
    if (m_currentWp.epoch != _newWp.epoch)
    {
        // Retrieve XHash epoch context (provides light cache pointer and sizes)
        const auto& _ec = xhash::get_global_epoch_context(_newWp.epoch);
        m_currentEc.epochNumber = _newWp.epoch;
        m_currentEc.lightNumItems = _ec.light_cache_num_items;
        m_currentEc.lightSize = xhash::get_light_cache_size(_ec.light_cache_num_items);
        m_currentEc.dagNumItems = _ec.full_dataset_num_items;
        m_currentEc.dagSize = xhash::get_full_dataset_size(_ec.full_dataset_num_items);
        m_currentEc.lightCache = _ec.light_cache;

        for (auto const& miner : m_miners)
            miner->setEpoch(m_currentEc);
    }

    m_currentWp = _newWp;

    // Check if we need to shuffle per work (ergodicity == 2)
    if (m_Settings.ergodicity == 2 && m_currentWp.exSizeBytes == 0)
        shuffle();

    // Equally divide the nonce space left by the pool's extranonce (if any)
    // among miners so their segments never overlap
    uint64_t _startNonce;
    if (m_currentWp.exSizeBytes > 0)
        _startNonce = m_currentWp.startNonce;
    else
        _startNonce = m_nonce_scrambler;  // Get the randomly selected nonce

    m_nonce_segment_with =
        nonceSegmentWidth(m_currentWp.exSizeBytes, m_miners.size(), m_userSegmentWidth);
    if (m_nonce_segment_with < 24)
        cwarn << "Pool leaves only " << m_nonce_segment_with
              << " bits of nonce space per GPU: GPUs will idle waiting for new jobs";

    for (unsigned int i = 0; i < m_miners.size(); i++)
    {
        if (m_nonce_segment_with < 64)
        {
            m_currentWp.startNonce = _startNonce + ((uint64_t)i << m_nonce_segment_with);
            m_currentWp.nonceRange = uint64_t(1) << m_nonce_segment_with;
        }
        else
        {
            m_currentWp.startNonce = _startNonce;
            m_currentWp.nonceRange = 0;
        }
        m_miners.at(i)->setWork(m_currentWp);
    }
}

unsigned Farm::nonceSegmentWidth(unsigned _exSizeHexChars, size_t _miners, unsigned _userWidth)
{
    unsigned bits = (_exSizeHexChars * 4u < 64u) ? 64u - _exSizeHexChars * 4u : 0u;

    unsigned minerBits = 0;  // ceil(log2(miners))
    while ((size_t(1) << minerBits) < _miners)
        minerBits++;

    unsigned width = (bits > minerBits) ? bits - minerBits : 0;

    // A narrower width can only be requested when there's no extranonce
    if (!_exSizeHexChars && _userWidth && _userWidth < width)
        width = _userWidth;
    return width;
}

namespace
{
// PCI id as in the device descriptors: bus:device.0
std::string pciId(unsigned _bus, unsigned _device)
{
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << std::hex << _bus << ":" << std::setw(2) << _device
        << ".0";
    return oss.str();
}
}  // namespace

void Farm::initNvml()
{
    if (nvmlh)
        return;
    nvmlh = wrap_nvml_create();
    if (nvmlh)
        for (int i = 0; i < nvmlh->nvml_gpucount; i++)
            map_nvml_handle[pciId(nvmlh->nvml_pci_bus_id[i], nvmlh->nvml_pci_device_id[i])] = i;
}

#if defined(__linux)
void Farm::initSysfs()
{
    if (sysfsh)
        return;
    sysfsh = wrap_amdsysfs_create();
    if (sysfsh)
        for (int i = 0; i < sysfsh->sysfs_gpucount; i++)
            map_amdsysfs_handle[pciId(sysfsh->sysfs_pci_bus_id[i], sysfsh->sysfs_pci_device_id[i])] =
                i;
}
#endif

dev::GpuControl* Farm::gpuControl(const DeviceDescriptor& _device, std::string& _error)
{
    std::lock_guard<std::mutex> l(x_oc);
    auto it = m_oc.find(_device.uniqueId);
    if (it != m_oc.end())
        return it->second.get();

    bool nvidia = _device.subscriptionType == DeviceSubscriptionTypeEnum::Cuda ||
                  (_device.subscriptionType == DeviceSubscriptionTypeEnum::OpenCL &&
                      _device.clPlatformType == ClPlatformTypeEnum::Nvidia);
    bool amd = _device.subscriptionType == DeviceSubscriptionTypeEnum::OpenCL &&
               _device.clPlatformType == ClPlatformTypeEnum::Amd;
    std::unique_ptr<dev::GpuControl> control;
    if (nvidia)
    {
        initNvml();
        auto idx = map_nvml_handle.find(_device.uniqueId);
        if (!nvmlh)
            _error = "NVML is not available";
        else if (idx == map_nvml_handle.end())
            _error = "NVML doesn't list this GPU";
        else
            control = dev::GpuControl::nvml(nvmlh, idx->second);
    }
    else if (amd)
    {
#if defined(__linux)
        initSysfs();
        auto idx = map_amdsysfs_handle.find(_device.uniqueId);
        if (!sysfsh || idx == map_amdsysfs_handle.end())
            _error = "the amdgpu driver doesn't list this GPU";
        else
            control = dev::GpuControl::amd(sysfsh, idx->second);
#else
        _error = "overclocking AMD GPUs is only supported on Linux";
#endif
    }
    else
        _error = "overclocking is only supported on NVIDIA and AMD GPUs";
    if (!control)
        return nullptr;
    return (m_oc[_device.uniqueId] = std::move(control)).get();
}

void Farm::restoreOc()
{
    std::lock_guard<std::mutex> l(x_oc);
    for (auto& oc : m_oc)
        if (oc.second->changed())
        {
            oc.second->restore();
            cnote << "Restored the clocks, power limit and fans of " << oc.first;
        }
}

HwSensorsType Farm::readSensors(Miner& _miner)
{
    HwSensorsType sensors;
    HwMonitorInfo hwInfo = _miner.hwmonInfo();
    unsigned int tempC = 0, fanpcnt = 0, powerMw = 0, limitMw = 0, coreMHz = 0, memMHz = 0,
                 memC = 0, hotspotC = 0;

    // Maps the miner's PCI id to the index of the monitoring library once
    auto mapIndex = [&](const std::map<string, int>& _map) {
        int devIdx = hwInfo.deviceIndex;
        if (devIdx == -1 && !hwInfo.devicePciId.empty())
        {
            auto it = _map.find(hwInfo.devicePciId);
            devIdx = (it != _map.end()) ? it->second : -2;  // -2: don't try again
            _miner.setHwmonDeviceIndex(devIdx);
        }
        return devIdx;
    };

    if (hwInfo.deviceType == HwMonitorInfoType::NVIDIA && nvmlh)
    {
        int devIdx = mapIndex(map_nvml_handle);
        if (devIdx >= 0)
        {
            wrap_nvml_get_tempC(nvmlh, devIdx, &tempC);
            wrap_nvml_get_fanpcnt(nvmlh, devIdx, &fanpcnt);
            wrap_nvml_get_mem_tempC(nvmlh, devIdx, &memC);
            wrap_nvml_get_clocks(nvmlh, devIdx, &coreMHz, &memMHz);
            if (m_Settings.hwMon == 2)
            {
                wrap_nvml_get_power_usage(nvmlh, devIdx, &powerMw);
                wrap_nvml_get_power_limit(nvmlh, devIdx, &limitMw);
            }
        }
    }
    else if (hwInfo.deviceType == HwMonitorInfoType::AMD)
    {
#if defined(__linux)
        if (sysfsh)
        {
            int devIdx = mapIndex(map_amdsysfs_handle);
            if (devIdx >= 0)
            {
                wrap_amdsysfs_get_tempC(sysfsh, devIdx, &tempC);
                wrap_amdsysfs_get_extra_tempsC(sysfsh, devIdx, &hotspotC, &memC);
                wrap_amdsysfs_get_fanpcnt(sysfsh, devIdx, &fanpcnt);
                wrap_amdsysfs_get_clocks(sysfsh, devIdx, &coreMHz, &memMHz);
                if (m_Settings.hwMon == 2)
                {
                    wrap_amdsysfs_get_power_usage(sysfsh, devIdx, &powerMw);
                    wrap_amdsysfs_get_power_limit(sysfsh, devIdx, &limitMw);
                }
            }
        }
#else
        if (adlh)  // Windows only for AMD
        {
            int devIdx = mapIndex(map_adl_handle);
            if (devIdx >= 0)
            {
                wrap_adl_get_tempC(adlh, devIdx, &tempC);
                wrap_adl_get_fanpcnt(adlh, devIdx, &fanpcnt);
                if (m_Settings.hwMon == 2)
                    wrap_adl_get_power_usage(adlh, devIdx, &powerMw);
            }
        }
#endif
    }

    sensors.tempC = tempC;
    sensors.hotspotC = hotspotC;
    sensors.memTempC = memC;
    sensors.fanP = fanpcnt;
    sensors.powerW = powerMw / 1000.0;
    sensors.powerLimitW = limitMw / 1000.0;
    sensors.coreMHz = coreMHz;
    sensors.memMHz = memMHz;
    return sensors;
}

void Farm::checkTemperatures(Miner& _miner, int _minerIdx, const HwSensorsType& _sensors)
{
    // Memory limits also apply to the hotspot on GPUs which don't report memory
    int memTemp = _sensors.memTempC ? _sensors.memTempC : _sensors.hotspotC;
    int hottest = std::max(std::max(_sensors.tempC, _sensors.hotspotC), _sensors.memTempC);

    if (m_Settings.shutdownTemp && hottest >= (int)m_Settings.shutdownTemp)
    {
        cerror << "GPU " << _minerIdx << " reached " << hottest << "C (--shutdown-temp "
               << m_Settings.shutdownTemp << "). Stopping hashwarp";
        g_exitCode = EXIT_CODE_SHUTDOWN_TEMP;
        raise(SIGTERM);
        return;
    }

    // Per GPU limits, falling back to the global (API settable) core limits
    const DeviceSettings& ds = _miner.getDescriptor().settings;
    unsigned tempStop = ds.tempStop ? ds.tempStop : m_Settings.tempStop;
    unsigned tempResume = ds.tempStop ? ds.tempResume : m_Settings.tempStart;
    unsigned memStop = ds.memTempStop;
    unsigned memResume = ds.memTempResume;

    bool paused = _miner.pauseTest(MinerPauseEnum::PauseDueToOverHeating);
    bool coreHot = tempStop && _sensors.tempC && _sensors.tempC >= (int)tempStop;
    bool memHot = memStop && memTemp && memTemp >= (int)memStop;
    if (!paused && (coreHot || memHot))
    {
        if (coreHot)
            cwarn << "GPU " << _minerIdx << " reached " << _sensors.tempC << "C, pausing until "
                  << tempResume << "C";
        else
            cwarn << "GPU " << _minerIdx << " memory reached " << memTemp << "C, pausing until "
                  << memResume << "C";
        _miner.pause(MinerPauseEnum::PauseDueToOverHeating);
        return;
    }
    if (memStop && !memTemp && !m_memTempWarned.count(_minerIdx))
    {
        m_memTempWarned.insert(_minerIdx);
        cwarn << "GPU " << _minerIdx
              << " reports no memory or hotspot temperature: --mem-temp-stop has no effect";
    }

    // A failed sensor read (0) never counts as cooled down
    if (paused)
    {
        bool coreCool = !tempStop || (_sensors.tempC && _sensors.tempC <= (int)tempResume);
        bool memCool = !memStop || !memTemp || memTemp <= (int)memResume;
        if (coreCool && memCool)
        {
            cnote << "GPU " << _minerIdx << " cooled down to " << _sensors.tempC << "C"
                  << (memTemp ? " (memory " + std::to_string(memTemp) + "C)" : "")
                  << ", resuming";
            _miner.resume(MinerPauseEnum::PauseDueToOverHeating);
        }
    }
}

void Farm::restartFailedMiner(
    Miner& _miner, int _minerIdx, std::chrono::steady_clock::time_point _now)
{
    MinerRestarts& r = m_minerRestarts[_minerIdx];
    if (r.givenUp || _now < r.next)
        return;
    while (!r.times.empty() && _now - r.times.front() > std::chrono::hours(1))
        r.times.pop_front();

    std::string reason = _miner.lastError();
    if (reason.empty())
        reason = "device initialization failed";

    if (r.times.size() >= m_Settings.watchdogRestarts)
    {
        // Leave this GPU stopped, the others keep mining
        r.givenUp = true;
        _miner.pause(MinerPauseEnum::PauseDueToFailure);
        cerror << "GPU " << _minerIdx << " failed " << r.times.size() + 1
               << " times in the last hour (" << reason << "). Leaving it stopped";

        bool anyMining = false;
        for (auto const& miner : m_miners)
            anyMining = anyMining || !m_minerRestarts[miner->Index()].givenUp;
        if (!anyMining)
            watchdogGiveUp(_minerIdx, "no GPU left mining", false);
        return;
    }

    // Wait 5, 10, 20 ... (up to 300) seconds before the following restart
    r.times.push_back(_now);
    r.next = _now + std::chrono::seconds(std::min(300, 5 << std::min<int>(r.times.size(), 6)));
    cwarn << "GPU " << _minerIdx << " stopped (" << reason << "). Restarting it ("
          << r.times.size() << "/" << m_Settings.watchdogRestarts << " in the last hour)";
    _miner.heartbeat();
    _miner.startWorking();
}

void Farm::hangWatchdogLoop()
{
    dev::setThreadName("watchdog");
    const auto timeout = std::chrono::seconds(m_Settings.watchdogTimeout);
    std::unique_lock<std::mutex> l(x_hangWatchdog);
    while (!m_hangWatchdogStop.wait_for(l, std::chrono::seconds(5), [this]() { return m_hangWatchdogStopping; }))
    {
        auto now = std::chrono::steady_clock::now();
        auto lastCollect = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(m_lastCollect.load()));
        if (now - lastCollect > timeout + std::chrono::seconds(10))
        {
            cerror << "Hashwarp's event loop is stuck since "
                   << std::chrono::duration_cast<std::chrono::seconds>(now - lastCollect).count()
                   << " seconds (GPU driver hang)";
            watchdogGiveUp(-1, "hung", true);
        }

        if (!m_isMining.load(std::memory_order_relaxed))
            continue;
        std::vector<std::shared_ptr<Miner>> miners;
        {
            Guard g(x_minerWork);
            miners = m_miners;
        }
        for (auto const& miner : miners)
        {
            if (!miner->isStopped() && miner->unresponsive(timeout))
            {
                cerror << "GPU " << miner->Index() << " gave no sign of life for "
                       << m_Settings.watchdogTimeout << " seconds (GPU or driver hang)";
                watchdogGiveUp(miner->Index(), "hung", true);
            }
        }
    }
}

void Farm::watchdogGiveUp(int _minerIdx, const std::string& _reason, bool _hung)
{
    if (m_watchdogGaveUp.exchange(true))
        return;
    g_exitCode = EXIT_CODE_WATCHDOG;
    if (!m_Settings.watchdogScript.empty())
    {
        cwarn << "Running watchdog script " << m_Settings.watchdogScript;
        try
        {
            boost::process::spawn(
                m_Settings.watchdogScript, std::to_string(_minerIdx), _reason);
        }
        catch (const std::exception& ex)
        {
            cerror << "Can't run " << m_Settings.watchdogScript << ": " << ex.what();
        }
    }

    // A hung GPU blocks its thread in the driver: a clean shutdown would hang too.
    // quick_exit runs the at_quick_exit handlers (terminal restore)
    if (_hung)
    {
        cerror << "Exiting (code " << EXIT_CODE_WATCHDOG << ")";
        std::quick_exit(EXIT_CODE_WATCHDOG);
    }
    cerror << "Stopping hashwarp (exit code " << EXIT_CODE_WATCHDOG << ")";
    raise(SIGTERM);
}

void Farm::pruneSamples(std::deque<Sample>& _samples, std::chrono::steady_clock::time_point _now)
{
    while (!_samples.empty() && _now - _samples.front().time > std::chrono::seconds(900))
        _samples.pop_front();
}

float Farm::averageSince(const std::deque<Sample>& _samples, std::chrono::steady_clock::time_point _since)
{
    double sum = 0;
    unsigned count = 0;
    for (auto it = _samples.rbegin(); it != _samples.rend() && it->time >= _since; ++it, ++count)
        sum += it->value;
    return count ? float(sum / count) : 0.0f;
}

/**
 * @brief Start a number of miners.
 */
bool Farm::start()
{
    // Prevent recursion
    if (m_isMining.load(std::memory_order_relaxed))
        return true;

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::start() begin");
    Guard l(x_minerWork);

    // Start all subscribed miners if none yet
    if (!m_miners.size())
    {
        for (auto it = m_DevicesCollection.begin(); it != m_DevicesCollection.end(); it++)
        {
            TelemetryAccountType minerTelemetry;
#if ETH_XHASHCUDA
            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::Cuda)
            {
                minerTelemetry.prefix = "cu";
                m_miners.push_back(std::shared_ptr<Miner>(
                    new CUDAMiner(m_miners.size(), m_CUSettings, it->second)));
            }
#endif
#if ETH_XHASHCL

            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::OpenCL)
            {
                minerTelemetry.prefix = "cl";
                m_miners.push_back(
                    std::shared_ptr<Miner>(new CLMiner(m_miners.size(), m_CLSettings, it->second)));
            }
#endif
#if ETH_XHASHCPU

            if (it->second.subscriptionType == DeviceSubscriptionTypeEnum::Cpu)
            {
                minerTelemetry.prefix = "cp";
                m_miners.push_back(std::shared_ptr<Miner>(
                    new CPUMiner(m_miners.size(), m_CPSettings, it->second)));
            }
#endif
            if (minerTelemetry.prefix.empty())
                continue;
            m_telemetry.miners.push_back(minerTelemetry);
            m_miners.back()->startWorking();
        }

        // Initialize DAG Load mode
        Miner::setDagLoadInfo(m_Settings.dagLoadMode, (unsigned int)m_miners.size());

        m_isMining.store(true, std::memory_order_relaxed);
    }
    else
    {
        for (auto const& miner : m_miners)
            miner->startWorking();
        m_isMining.store(true, std::memory_order_relaxed);
    }

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::start() end");
    return m_isMining.load(std::memory_order_relaxed);
}

/**
 * @brief Stop all mining activities.
 */
void Farm::stop()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::stop() begin");
    // Avoid re-entering if not actually mining.
    // This, in fact, is also called by destructor
    if (isMining())
    {
        {
            Guard l(x_minerWork);
            for (auto const& miner : m_miners)
            {
                miner->triggerStopWorking();
                miner->kick_miner();
            }

            m_miners.clear();
            m_isMining.store(false, std::memory_order_relaxed);
        }
    }
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "Farm::stop() end");
}

/**
 * @brief Pauses the whole collection of miners
 */
void Farm::pause()
{
    // Signal each miner to suspend mining
    Guard l(x_minerWork);
    m_paused.store(true, std::memory_order_relaxed);
    for (auto const& m : m_miners)
        m->pause(MinerPauseEnum::PauseDueToFarmPaused);
}

/**
 * @brief Returns whether or not this farm is paused for any reason
 */
bool Farm::paused()
{
    return m_paused.load(std::memory_order_relaxed);
}

/**
 * @brief Resumes from a pause condition
 */
void Farm::resume()
{
    // Signal each miner to resume mining
    // Note ! Miners may stay suspended if other reasons
    Guard l(x_minerWork);
    m_paused.store(false, std::memory_order_relaxed);
    for (auto const& m : m_miners)
        m->resume(MinerPauseEnum::PauseDueToFarmPaused);
}

/**
 * @brief Stop all mining activities and Starts them again
 */
void Farm::restart()
{
    if (m_onMinerRestart)
        m_onMinerRestart();
}

/**
 * @brief Stop all mining activities and Starts them again (async post)
 */
void Farm::restart_async()
{
    boost::asio::post(m_io_strand, boost::bind(&Farm::restart, this));
}

/**
 * @brief Spawn a reboot script (reboot.bat/reboot.sh)
 * @return false if no matching file was found
 */
bool Farm::reboot(const std::vector<std::string>& args)
{
#if defined(_WIN32)
    const char* filename = "reboot.bat";
#else
    const char* filename = "reboot.sh";
#endif

    return spawn_file_in_bin_dir(filename, args);
}

/**
 * @brief Account solutions for miner and for farm
 */
void Farm::accountSolution(unsigned _minerIdx, SolutionAccountingEnum _accounting, double _hashes)
{
    if (_accounting == SolutionAccountingEnum::Accepted)
    {
        if (_hashes > 0)
            m_acceptedHashes.push_back({std::chrono::steady_clock::now(), _hashes});
        m_telemetry.farm.solutions.accepted++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.accepted++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
    if (_accounting == SolutionAccountingEnum::Wasted)
    {
        m_telemetry.farm.solutions.wasted++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.wasted++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
    if (_accounting == SolutionAccountingEnum::Rejected)
    {
        m_telemetry.farm.solutions.rejected++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.rejected++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
    if (_accounting == SolutionAccountingEnum::Failed)
    {
        m_telemetry.farm.solutions.failed++;
        m_telemetry.farm.solutions.tstamp = std::chrono::steady_clock::now();
        m_telemetry.miners.at(_minerIdx).solutions.failed++;
        m_telemetry.miners.at(_minerIdx).solutions.tstamp = std::chrono::steady_clock::now();
        return;
    }
}

/**
 * @brief Gets the solutions account for the whole farm
 */

SolutionAccountType Farm::getSolutions()
{
    return m_telemetry.farm.solutions;
}

/**
 * @brief Gets the solutions account for single miner
 */
SolutionAccountType Farm::getSolutions(unsigned _minerIdx)
{
    try
    {
        return m_telemetry.miners.at(_minerIdx).solutions;
    }
    catch (const std::exception&)
    {
        return SolutionAccountType();
    }
}

/**
 * @brief Provides the description of segments each miner is working on
 * @return a JsonObject
 */
Json::Value Farm::get_nonce_scrambler_json()
{
    Json::Value jRes;
    jRes["start_nonce"] = toHex(m_nonce_scrambler, HexPrefix::Add);
    jRes["device_width"] = m_nonce_segment_with;
    jRes["device_count"] = (uint64_t)m_miners.size();

    return jRes;
}

void Farm::setTStartTStop(unsigned tstart, unsigned tstop)
{
    m_Settings.tempStart = tstart;
    m_Settings.tempStop = tstop;
}

void Farm::submitProof(Solution const& _s)
{
    g_io_service.post(m_io_strand.wrap(boost::bind(&Farm::submitProofAsync, this, _s)));
}

void Farm::submitProofAsync(Solution const& _s)
{
    // Test hook (see Miner::faultInjectionPoint): a GPU returning wrong solutions
    static const bool badSolutions = getenv("HASHWARP_FAULT_INJECTION") &&
                                     std::string(getenv("HASHWARP_FAULT_INJECTION")) == "bad-solution";
    uint64_t nonce = badSolutions ? _s.nonce + 1 : _s.nonce;

    if (!m_Settings.noEval)
    {
        Result r = XHashAux::eval(_s.work.epoch, _s.work.header, nonce);
        if (r.value > _s.work.boundary)
        {
            accountSolution(_s.midx, SolutionAccountingEnum::Failed);
            cwarn << "GPU " << _s.midx
                  << " gave incorrect result. Lower overclocking values if it happens frequently.";
            return;
        }
        m_onSolutionFound(Solution{_s.nonce, r.mixHash, _s.work, _s.tstamp, _s.midx});
    }
    else
        m_onSolutionFound(_s);

#ifdef DEV_BUILD
    if (g_logOptions & LOG_SUBMIT)
        cdebug << "Submit time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - _s.tstamp)
                     .count()
              << " us.";
#endif
}

// Collects data about hashing and hardware status
void Farm::collectData(const boost::system::error_code& ec)
{
    if (ec)
        return;

    // Reset hashrate (it will accumulate from miners)
    float farm_hr = 0.0f;
    auto now = std::chrono::steady_clock::now();
    if (m_minerHashrates.size() < m_miners.size())
        m_minerHashrates.resize(m_miners.size());

    // Process miners
    for (auto const& miner : m_miners)
    {
        int minerIdx = miner->Index();

        // Zeroes the hashrate of a miner which didn't answer the previous
        // trigger (stopped or idle) before reading it
        miner->TriggerHashRateUpdate();
        float hr = (miner->paused() ? 0.0f : miner->RetrieveHashRate());

        // Averages start with the first hashes (not while the DAG is built)
        auto& history = m_minerHashrates.at(minerIdx);
        if (hr > 0 || !history.empty())
            history.push_back({now, hr});
        pruneSamples(history, now);
        m_telemetry.miners.at(minerIdx).hashrate60 =
            averageSince(history, now - std::chrono::seconds(60));
        m_telemetry.miners.at(minerIdx).hashrate900 =
            averageSince(history, now - std::chrono::seconds(900));
        farm_hr += hr;
        m_telemetry.miners.at(minerIdx).hashrate = hr;
        m_telemetry.miners.at(minerIdx).paused = miner->paused();


        if (m_Settings.hwMon)
        {
            HwSensorsType sensors = readSensors(*miner);
            checkTemperatures(*miner, minerIdx, sensors);
            m_telemetry.miners.at(minerIdx).sensors = sensors;
        }

        if (m_Settings.watchdogTimeout && m_isMining.load(std::memory_order_relaxed) &&
            miner->isStopped())
            restartFailedMiner(*miner, minerIdx, now);
        m_telemetry.farm.hashrate = farm_hr;
    }

    if (farm_hr > 0 || !m_farmHashrates.empty())
        m_farmHashrates.push_back({now, farm_hr});
    pruneSamples(m_farmHashrates, now);
    m_telemetry.farm.hashrate60 = averageSince(m_farmHashrates, now - std::chrono::seconds(60));
    m_telemetry.farm.hashrate900 = averageSince(m_farmHashrates, now - std::chrono::seconds(900));

    // Effective hashrate: what the accepted shares are worth over the last
    // 15 minutes (or since mining started)
    pruneSamples(m_acceptedHashes, now);
    double window = std::min(900.0,
        std::chrono::duration<double>(now - m_telemetry.start).count());
    double hashes = 0;
    for (const auto& s : m_acceptedHashes)
        hashes += s.value;
    // Meaningless (a couple of lucky shares) until enough time passed
    m_telemetry.effectiveHashrate = window >= 120 ? hashes / window : 0;

    m_lastCollect = now.time_since_epoch().count();

    // Resubmit timer for another loop
    m_collectTimer.expires_from_now(boost::posix_time::milliseconds(m_collectInterval));
    m_collectTimer.async_wait(
        m_io_strand.wrap(boost::bind(&Farm::collectData, this, boost::asio::placeholders::error)));
}

bool Farm::spawn_file_in_bin_dir(const char* filename, const std::vector<std::string>& args)
{
    std::string fn = boost::dll::program_location().parent_path().string() +
                     "/" +  // boost::filesystem::path::preferred_separator
                     filename;
    try
    {
        if (!boost::filesystem::exists(fn))
            return false;

        /* anything in the file */
        if (!boost::filesystem::file_size(fn))
            return false;

#if defined(__linux)
        struct stat sb;
        if (stat(fn.c_str(), &sb) != 0)
            return false;
        /* just check if any exec flag is set.
           still execution can fail (not the uid, not in the group, selinux, ...)
         */
        if ((sb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
            return false;
#endif
        /* spawn it (no wait,...) - fire and forget! */
        boost::process::spawn(fn, args);
        return true;
    }
    catch (...)
    {}
    return false;
}


}  // namespace eth
}  // namespace dev
