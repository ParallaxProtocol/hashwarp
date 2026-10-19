// Hashwarp console output: periodic summary line and statistics table.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <libparallaxcore/Miner.h>

#include <string>
#include <vector>

namespace hashwarp
{
/// What the console shows about the pool connection
struct PoolStatus
{
    bool connected = false;
    std::string host;           // host:port
    double difficulty = 0;      // Expected hashes per share
    int epoch = -1;
    int block = -1;             // -1 if unknown
};

/// What the console shows about each GPU
struct GpuStatus
{
    std::string name;
    std::string pauseReason;  // Empty if mining
};

/// A line of the periodic report, with the fields of the JSON log format
struct ReportLine
{
    std::string source;  // miner, or gpuN
    std::string text;
    std::vector<std::pair<std::string, double>> fields;
};

/// The periodic report: the farm's speeds and shares, then one line per GPU with
/// _perGpu (otherwise the only GPU's sensors are on the farm's line)
std::vector<ReportLine> reportLines(
    const dev::eth::TelemetryType& _t, const PoolStatus& _pool, bool _perGpu);

/// Per GPU statistics table, one string per line
std::vector<std::string> statsTable(const dev::eth::TelemetryType& _t,
    const std::vector<GpuStatus>& _gpus, const PoolStatus& _pool);

}  // namespace hashwarp
