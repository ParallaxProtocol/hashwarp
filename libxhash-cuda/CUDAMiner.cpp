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
#include <libparallaxcore/XHashAux.h>
#include <xhash/xhash.hpp>

#include "CUDAMiner.h"
#include "xhash_cuda_miner_kernel.h"

using namespace std;
using namespace dev;
using namespace eth;

struct CUDAChannel : public LogChannel
{
    static const char* name() { return EthOrange "cu"; }
    static const int verbosity = 2;
};
#define cudalog clog(CUDAChannel)

namespace
{
CUSettings withDeviceSettings(CUSettings _settings, const DeviceDescriptor& _device)
{
    if (_device.settings.cuGridSize)
        _settings.gridSize = _device.settings.cuGridSize;
    if (_device.settings.cuBlockSize)
        _settings.blockSize = _device.settings.cuBlockSize;
    if (_device.settings.cuStreams)
        _settings.streams = _device.settings.cuStreams;
    return _settings;
}
}  // namespace

CUDAMiner::CUDAMiner(unsigned _index, CUSettings _settings, DeviceDescriptor& _device)
  : Miner("cuda-", _index),
    m_settings(withDeviceSettings(_settings, _device)),
    m_batch_size(m_settings.gridSize * m_settings.blockSize)
{
    m_deviceDescriptor = _device;
}

CUDAMiner::~CUDAMiner()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cudalog, "cuda-" << m_index << " CUDAMiner::~CUDAMiner() begin");
    stopWorking();
    kick_miner();
    DEV_BUILD_LOG_PROGRAMFLOW(cudalog, "cuda-" << m_index << " CUDAMiner::~CUDAMiner() end");
}

bool CUDAMiner::initDevice()
{
    cudalog << "Using Pci Id : " << m_deviceDescriptor.uniqueId << " " << m_deviceDescriptor.cuName
            << " (Compute " + m_deviceDescriptor.cuCompute + ") Memory : "
            << dev::getFormattedMemory((double)m_deviceDescriptor.totalMemory);

    // Set Hardware Monitor Info
    m_hwmoninfo.deviceType = HwMonitorInfoType::NVIDIA;
    m_hwmoninfo.devicePciId = m_deviceDescriptor.uniqueId;
    m_hwmoninfo.deviceIndex = -1;  // Will be later on mapped by nvml (see Farm() constructor)

    try
    {
        CUDA_SAFE_CALL(cudaSetDevice(m_deviceDescriptor.cuDeviceIndex));
        CUDA_SAFE_CALL(cudaDeviceReset());

        // The reset freed everything: nothing to reuse (e.g. when restarted by the watchdog)
        m_allocated_memory_dag = 0;
        m_allocated_memory_light_cache = 0;
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Could not set CUDA device on Pci Id " << m_deviceDescriptor.uniqueId
                << " Error : " << ec.what();
        cudalog << "Mining aborted on this device.";
        return false;
    }
    return true;
}

bool CUDAMiner::initEpoch_internal()
{
    // If we get here it means epoch has changed so it's not necessary
    // to check again dag sizes. They're changed for sure
    bool retVar = false;
    m_current_target = 0;
    auto startInit = std::chrono::steady_clock::now();
    size_t RequiredTotalMemory = (m_epochContext.dagSize + m_epochContext.lightSize);
    size_t RequiredDagMemory = m_epochContext.dagSize;

    // Release the pause flag if any
    resume(MinerPauseEnum::PauseDueToInsufficientMemory);
    resume(MinerPauseEnum::PauseDueToInitEpochError);

    bool lightOnHost = false;
    size_t freeMemory = m_deviceDescriptor.totalMemory;
    try
    {
        hash128_t* dag;
        hash64_t* light;

        // If we have already enough memory allocated, we just have to
        // copy light_cache and regenerate the DAG
        if (m_allocated_memory_dag < m_epochContext.dagSize ||
            m_allocated_memory_light_cache < m_epochContext.lightSize)
        {
            // We need to reset the device and (re)create the dag
            // cudaDeviceReset() frees all previous allocated memory
            CUDA_SAFE_CALL(cudaDeviceReset());
            m_allocated_memory_dag = 0;
            m_allocated_memory_light_cache = 0;
            CUDA_SAFE_CALL(cudaSetDeviceFlags(m_settings.schedule));
            CUDA_SAFE_CALL(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));

            // Check whether the current device has sufficient memory every time we recreate the dag
            size_t totalMem;
            CUDA_SAFE_CALL(cudaMemGetInfo(&freeMemory, &totalMem));
            if (freeMemory < RequiredTotalMemory)
            {
                if (freeMemory < RequiredDagMemory)
                {
                    cudalog << "Epoch " << m_epochContext.epochNumber << " requires "
                            << dev::getFormattedMemory((double)RequiredDagMemory) << " memory. Only "
                            << dev::getFormattedMemory((double)freeMemory)
                            << " free on device.";
                    cudalog << "This device hasn't enough memory available. Mining suspended ...";
                    pause(MinerPauseEnum::PauseDueToInsufficientMemory);
                    return true;  // This will prevent to exit the thread and
                                  // Eventually resume mining when changing coin or epoch (NiceHash)
                }
                else
                    lightOnHost = true;
            }

            cudalog << "Generating DAG + Light(on " << (lightOnHost ? "host" : "GPU")
                    << ") : " << dev::getFormattedMemory((double)RequiredTotalMemory);

            // create buffer for cache
            if (lightOnHost)
            {
                CUDA_SAFE_CALL(cudaHostAlloc(reinterpret_cast<void**>(&light),
                    m_epochContext.lightSize, cudaHostAllocDefault));
                cudalog << "WARNING: Generating DAG will take minutes, not seconds";
            }
            else
                CUDA_SAFE_CALL(
                    cudaMalloc(reinterpret_cast<void**>(&light), m_epochContext.lightSize));
            CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&dag), m_epochContext.dagSize));

            // create mining buffers
            for (unsigned i = 0; i != m_settings.streams; ++i)
            {
                CUDA_SAFE_CALL(cudaMallocHost(&m_search_buf[i], sizeof(Search_results)));
                CUDA_SAFE_CALL(cudaStreamCreateWithFlags(&m_streams[i], cudaStreamNonBlocking));
            }

            // Only now everything is allocated: buffers may be reused on next epoch
            m_allocated_memory_light_cache = m_epochContext.lightSize;
            m_allocated_memory_dag = m_epochContext.dagSize;
        }
        else
        {
            cudalog << "Generating DAG + Light (reusing buffers): "
                    << dev::getFormattedMemory((double)RequiredTotalMemory);
            get_constants(&dag, NULL, &light, NULL);
        }

        CUDA_SAFE_CALL(cudaMemcpy(reinterpret_cast<void*>(light), m_epochContext.lightCache,
            m_epochContext.lightSize, cudaMemcpyHostToDevice));

        set_constants(dag, m_epochContext.dagNumItems, light,
            m_epochContext.lightNumItems);  // in xhash_cuda_miner_kernel.cu

        xhash_generate_dag(
            m_epochContext.dagSize, m_settings.gridSize, m_settings.blockSize, m_streams[0]);

        cudalog << "Generated DAG + Light in "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startInit)
                       .count()
                << " ms. "
                << dev::getFormattedMemory(
                       lightOnHost ? (double)(freeMemory - RequiredDagMemory) :
                                     (double)(freeMemory - RequiredTotalMemory))
                << " left.";

        retVar = true;
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Unexpected error " << ec.what() << " on CUDA device "
                << m_deviceDescriptor.uniqueId;
        cudalog << "Mining suspended ...";
        // Start from a clean device on the next attempt
        m_allocated_memory_dag = 0;
        m_allocated_memory_light_cache = 0;
        pause(MinerPauseEnum::PauseDueToInitEpochError);
        retVar = true;
    }

    return retVar;
}

void CUDAMiner::workLoop()
{
    WorkPackage current;
    current.header = h256();
    uint64_t nextNonce = 0;   // Next nonce to search on current job
    bool exhausted = false;   // Whether current job's nonce range was fully searched

    m_search_buf.resize(m_settings.streams);
    m_streams.resize(m_settings.streams);

    if (!initDevice())
        return;

    try
    {
        while (!shouldStop())
        {
            // Wait for work or 3 seconds (whichever the first)
            const WorkPackage w = work();
            if (!w)
            {
                waitForWork();
                continue;
            }

            // Epoch change (or a previous initialization failed) ?
            if (current.epoch != w.epoch || epochInitFailed())
            {
                if (!canInitEpoch(w.epoch))
                {
                    waitForWork();
                    continue;
                }

                if (!initEpoch())
                    break;  // This will simply exit the thread

                // Keep current.epoch stale on failure so we retry later
                if (epochInitFailed())
                    continue;

                if (!m_kernelTuned)
                {
                    m_kernelTuned = true;
                    m_kernel = m_settings.kernel >= 0 ? unsigned(m_settings.kernel) : tuneKernel();
                }

                // As DAG generation takes a while we need to
                // ensure we're on latest job, not on the one
                // which triggered the epoch change
                current = w;
                nextNonce = w.startNonce;
                exhausted = false;
                continue;
            }

            // Same job again (resent by the pool, new target, resume after pause) ?
            // Then carry on where we left off instead of hashing the same nonces twice
            if (w.header != current.header || w.startNonce != current.startNonce)
            {
                nextNonce = w.startNonce;
                exhausted = false;
            }

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;

            if (exhausted)
            {
                // Nothing left to search on this job: wait for a new one
                waitForWork();
                continue;
            }

            uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)current.boundary >> 192);

            // Eventually start searching
            nextNonce = search(current.header.data(), upper64OfBoundary, nextNonce, w, exhausted);
            if (exhausted)
                cudalog << "Nonce range of job " << current.header.abridged()
                        << " exhausted. Waiting for new work ...";
        }

        // Reset miner and stop working
        CUDA_SAFE_CALL(cudaDeviceReset());
    }
    catch (cuda_runtime_error const& _e)
    {
        string _what = "GPU error: ";
        _what.append(_e.what());
        throw std::runtime_error(_what);
    }
}

unsigned CUDAMiner::tuneKernel()
{
    // Search a dummy job nobody can solve (target 0) with each variant in turn
    hash32_t header;
    for (auto& w : header.uint4s)
        w = make_uint4(0x12345678, 0x9abcdef0, 0x0fedcba9, 0x87654321);
    set_header(header);
    set_target(0);
    m_current_target = 0;

    const unsigned c_variants = 2;
    const char* const names[c_variants] = {"standard", "dedup"};
    double seconds[c_variants] = {0, 0};
    uint64_t hashes[c_variants] = {0, 0};
    cudaStream_t stream = m_streams[0];
    volatile Search_results& buffer(*m_search_buf[0]);
    cudaEvent_t start, stop;
    CUDA_SAFE_CALL(cudaEventCreate(&start));
    CUDA_SAFE_CALL(cudaEventCreate(&stop));

    // Batches for about 0.3 s at 100 MH/s per round, at least 8
    unsigned batches = std::max(8u, unsigned(30000000ull / m_batch_size));
    uint64_t nonce = 0;
    for (unsigned round = 0; round < 4; round++)
        for (unsigned v = 0; v < c_variants; v++)
        {
            CUDA_SAFE_CALL(cudaEventRecord(start, stream));
            for (unsigned b = 0; b < batches; b++, nonce += m_batch_size)
                run_xhash_search(m_settings.gridSize, m_settings.blockSize, stream, &buffer, nonce, v);
            CUDA_SAFE_CALL(cudaEventRecord(stop, stream));
            CUDA_SAFE_CALL(cudaEventSynchronize(stop));
            float ms = 0;
            CUDA_SAFE_CALL(cudaEventElapsedTime(&ms, start, stop));
            if (round == 0)
                continue;  // warm up (clocks, caches)
            seconds[v] += ms / 1000.0;
            hashes[v] += uint64_t(batches) * m_batch_size;
        }
    CUDA_SAFE_CALL(cudaEventDestroy(start));
    CUDA_SAFE_CALL(cudaEventDestroy(stop));
    buffer.count = 0;
    heartbeat();

    unsigned best = 0;
    std::ostringstream ss;
    for (unsigned v = 0; v < c_variants; v++)
    {
        double rate = seconds[v] > 0 ? hashes[v] / seconds[v] : 0;
        if (v && rate > (seconds[best] > 0 ? hashes[best] / seconds[best] : 0))
            best = v;
        ss << (v ? ", " : "") << names[v] << " " << dev::getFormattedHashes(rate) << "/s";
    }
    cudalog << "Kernel tuning: " << ss.str() << ". Using " << names[best];
    return best;
}

void CUDAMiner::kick_miner()
{
    m_new_work.store(true, std::memory_order_relaxed);
    m_new_work_signal.notify_one();
}

int CUDAMiner::getNumDevices()
{
    int deviceCount;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err == cudaSuccess)
        return deviceCount;

    if (err == cudaErrorInsufficientDriver)
    {
        int driverVersion = 0;
        cudaDriverGetVersion(&driverVersion);
        if (driverVersion == 0)
            std::cerr << "CUDA Error : No CUDA driver found" << std::endl;
        else
            std::cerr << "CUDA Error : Insufficient CUDA driver " << std::to_string(driverVersion)
                      << std::endl;
    }
    else
    {
        std::cerr << "CUDA Error : " << cudaGetErrorString(err) << std::endl;
    }

    return 0;
}

void CUDAMiner::enumDevices(std::map<string, DeviceDescriptor>& _DevicesCollection)
{
    int numDevices = getNumDevices();

    for (int i = 0; i < numDevices; i++)
    {
        string uniqueId;
        ostringstream s;
        DeviceDescriptor deviceDescriptor;
        cudaDeviceProp props;

        try
        {
            CUDA_SAFE_CALL(cudaGetDeviceProperties(&props, i));
            s << setw(2) << setfill('0') << hex << props.pciBusID << ":" << setw(2)
              << props.pciDeviceID << ".0";
            uniqueId = s.str();

            if (_DevicesCollection.find(uniqueId) != _DevicesCollection.end())
                deviceDescriptor = _DevicesCollection[uniqueId];
            else
                deviceDescriptor = DeviceDescriptor();

            deviceDescriptor.name = string(props.name);
            deviceDescriptor.cuDetected = true;
            deviceDescriptor.uniqueId = uniqueId;
            deviceDescriptor.type = DeviceTypeEnum::Gpu;
            deviceDescriptor.cuDeviceIndex = i;
            deviceDescriptor.cuDeviceOrdinal = i;
            deviceDescriptor.cuName = string(props.name);
            deviceDescriptor.totalMemory = props.totalGlobalMem;
            deviceDescriptor.cuCompute = (to_string(props.major) + "." + to_string(props.minor));
            deviceDescriptor.cuComputeMajor = props.major;
            deviceDescriptor.cuComputeMinor = props.minor;

            _DevicesCollection[uniqueId] = deviceDescriptor;
        }
        catch (const cuda_runtime_error& _e)
        {
            std::cerr << _e.what() << std::endl;
        }
    }
}

uint64_t CUDAMiner::search(uint8_t const* header, uint64_t target, uint64_t start_nonce,
    const dev::eth::WorkPackage& w, bool& exhausted)
{
    set_header(*reinterpret_cast<hash32_t const*>(header));
    if (m_current_target != target)
    {
        set_target(target);
        m_current_target = target;
    }

    // Whether a whole batch starting at _nonce fits in this miner's segment
    auto inRange = [&](uint64_t _nonce) {
        return !w.nonceRange ||
               (w.nonceRange >= m_batch_size && _nonce - w.startNonce <= w.nonceRange - m_batch_size);
    };

    // prime each stream, clear search result buffers and start the search
    std::vector<uint64_t> stream_nonce(m_settings.streams);
    std::vector<bool> stream_active(m_settings.streams, false);
    unsigned active = 0;
    for (uint32_t i = 0; i < m_settings.streams && inRange(start_nonce); i++)
    {
        volatile Search_results& buffer(*m_search_buf[i]);
        buffer.count = 0;

        // Run the batch for this stream
        run_xhash_search(
            m_settings.gridSize, m_settings.blockSize, m_streams[i], &buffer, start_nonce, m_kernel);
        stream_nonce[i] = start_nonce;
        stream_active[i] = true;
        active++;
        start_nonce += m_batch_size;
    }

    // process stream batches until we get new work
    // or this miner's nonce segment is exhausted
    bool done = false;
    while (active)
    {
        // Exit next time around if there's new work awaiting
        bool t = true;
        if (!done)
            done = m_new_work.compare_exchange_strong(t, false);

        // Check on every batch if we need to suspend mining
        if (!done)
            done = paused();

        // This inner loop will process each cuda stream individually
        unsigned completed = 0;
        for (uint32_t i = 0; i < m_settings.streams; i++)
        {
            if (!stream_active[i])
                continue;

            // Each pass of this loop will wait for a stream to exit,
            // save any found solutions, then restart the stream
            // on the next group of nonces.
            cudaStream_t stream = m_streams[i];

            // Wait for the stream complete
            CUDA_SAFE_CALL(cudaStreamSynchronize(stream));
            completed++;

            if (shouldStop())
            {
                m_new_work.store(false, std::memory_order_relaxed);
                done = true;
            }

            // Detect solutions in current stream's solution buffer
            volatile Search_results& buffer(*m_search_buf[i]);
            uint32_t found_count = std::min((unsigned)buffer.count, MAX_SEARCH_RESULTS);

            uint32_t gids[MAX_SEARCH_RESULTS];
            h256 mixes[MAX_SEARCH_RESULTS];

            if (found_count)
            {
                buffer.count = 0;

                // Extract solution and pass to higer level
                // using io_service as dispatcher

                for (uint32_t r = 0; r < found_count; r++)
                {
                    gids[r] = buffer.result[r].gid;
                    memcpy(mixes[r].data(), (void*)&buffer.result[r].mix,
                        sizeof(buffer.result[r].mix));
                }
            }

            // restart the stream on the next batch of nonces
            // unless we are done for this round.
            uint64_t nonce_base = stream_nonce[i];
            if (!done && inRange(start_nonce))
            {
                run_xhash_search(m_settings.gridSize, m_settings.blockSize, stream, &buffer,
                    start_nonce, m_kernel);
                stream_nonce[i] = start_nonce;
                start_nonce += m_batch_size;
            }
            else
            {
                stream_active[i] = false;
                active--;
            }

            for (uint32_t r = 0; r < found_count; r++)
            {
                uint64_t nonce = nonce_base + gids[r];

                Farm::f().submitProof(
                    Solution{nonce, mixes[r], w, std::chrono::steady_clock::now(), m_index});
                cdebug << "cuda-" << m_index << " Job: " << w.header.abridged() << " Sol: 0x"
                        << toHex(nonce) << EthReset;
            }
        }

        // Update the hash rate
        updateHashRate(m_batch_size, completed);
        faultInjectionPoint();
    }

    // Ran out of nonces without being asked to switch
    exhausted = !done && !shouldStop();

#ifdef DEV_BUILD
    // Optionally log job switch time
    if (!shouldStop() && (g_logOptions & LOG_SWITCH))
        cudalog << "Switch time: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - m_workSwitchStart)
                       .count()
                << " ms.";
#endif

    return start_nonce;
}
