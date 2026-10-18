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

#pragma once

// Avoid pulling CUDA headers into non-CUDA targets including this header.
// Provide minimal shared types and forward declarations here.

#include <cstdint>

#ifndef XHASH_CUDA_TYPES_DEFINED
#define XHASH_CUDA_TYPES_DEFINED
// Mirror Search_Result/Search_results layout used by CUDA kernels
struct Search_Result
{
    uint32_t gid;
    uint32_t mix[8];
    uint32_t pad[7];
};

struct Search_results
{
    Search_Result result[4];
    uint32_t count;
};
#endif // XHASH_CUDA_TYPES_DEFINED

// Forward declare CUDA stream type to avoid depending on cuda_runtime.h here
struct CUstream_st;
typedef CUstream_st* cudaStream_t;

#include <libdevcore/Worker.h>
#include <libparallaxcore/XHashAux.h>
#include <libparallaxcore/Miner.h>

#include <functional>

namespace dev
{
namespace eth
{
class CUDAMiner : public Miner
{
public:
    CUDAMiner(unsigned _index, CUSettings _settings, DeviceDescriptor& _device);
    ~CUDAMiner() override;

    static int getNumDevices();
    static void enumDevices(std::map<string, DeviceDescriptor>& _DevicesCollection);

    // Searches from _startN until new work arrives or the miner's nonce range is
    // exhausted (sets exhausted). Returns the next nonce to search.
    uint64_t search(uint8_t const* header, uint64_t target, uint64_t _startN,
        const dev::eth::WorkPackage& w, bool& exhausted);

protected:
    bool initDevice() override;

    bool initEpoch_internal() override;

    void kick_miner() override;

private:
    atomic<bool> m_new_work = {false};

    void workLoop() override;

    std::vector<volatile Search_results*> m_search_buf;
    std::vector<cudaStream_t> m_streams;
    uint64_t m_current_target = 0;

    // Times the search kernel variants and returns the fastest
    unsigned tuneKernel();
    unsigned m_kernel = 0;       // Search kernel variant in use
    bool m_kernelTuned = false;  // Auto tuning is done once

    CUSettings m_settings;

    const uint32_t m_batch_size;

    uint64_t m_allocated_memory_dag = 0; // dag_size is a uint64_t in EpochContext struct
    size_t m_allocated_memory_light_cache = 0;
};


}  // namespace eth
}  // namespace dev
