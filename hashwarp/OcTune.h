// Search of the best memory clock and the most efficient core clock of a GPU (--oc-tune).
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <libhwmon/GpuControl.h>

#include <optional>
#include <string>
#include <vector>

namespace hashwarp
{
/// One GPU's search. next() gives the settings to measure, report() takes the
/// measurement, until next() returns nullopt. The settings of next() are
/// complete: they replace the previous ones
class OcSearch
{
public:
    /// _memory: also tune the memory clock, otherwise keep _fixedMemory (memory
    /// offset or clock, e.g. tuned before). _coreMHz: the core clock while mining at
    /// stock settings, where the core search starts
    OcSearch(const dev::OcLimits& _limits, bool _memory, unsigned _coreMHz,
        const dev::OcSettings& _fixedMemory = dev::OcSettings());

    std::optional<dev::OcSettings> next();

    /// _errors: wrong results, or the GPU failed
    void report(double _hashrate, double _powerW, bool _errors);

    /// What the search is doing, for the log
    std::string phase() const;

    /// The result, valid once next() returned nullopt
    const dev::OcSettings& best() const { return m_best; }
    double bestHashrate() const { return m_bestHashrate; }
    double bestPowerW() const { return m_bestPowerW; }
    double baseHashrate() const { return m_baseHashrate; }
    double basePowerW() const { return m_basePowerW; }
    /// Whether the result kept its hashrate without errors (false: stock is best)
    bool found() const { return m_found; }

    // The search's steps (public for the tests)
    static const int c_memOffsetStep = 250;     // MHz, NVIDIA (nvidia-settings units)
    static const unsigned c_memClockStep = 25;  // MHz, AMD
    static const unsigned c_coreCoarseStep = 150;
    static const unsigned c_coreFineStep = 50;
    static const unsigned c_coreMin = 600;
    static constexpr double c_coreKeep = 0.99;  // of the hashrate the core may cost
    static constexpr double c_memDrop = 0.99;   // below the best: past the memory's peak
    static constexpr double c_memMargin = 0.995;

private:
    enum class Phase
    {
        Base,
        Memory,
        CoreCoarse,
        CoreFine,
        Confirm,
        Done
    };

    dev::OcSettings memSettings(int _step) const;
    dev::OcSettings withCore(unsigned _mhz) const;
    void finishMemory(bool _errors);  // _errors: the last step gave wrong results
    void finishCore();

    dev::OcLimits m_limits;
    bool m_memory;
    bool m_nvidia;  // memory offsets (NVIDIA) or clocks (AMD)
    unsigned m_coreStart;
    dev::OcSettings m_fixedMemory;

    Phase m_phase = Phase::Base;
    dev::OcSettings m_trying;

    double m_baseHashrate = 0, m_basePowerW = 0;

    // Memory: hashrate of each step tried (step 0 = stock)
    std::vector<double> m_memHashrates;
    int m_memStep = 0;
    unsigned m_drops = 0;
    dev::OcSettings m_mem;  // the chosen memory settings
    double m_memHashrate = 0;

    // Core
    unsigned m_coreOk = 0;    // lowest clock that kept the hashrate
    unsigned m_coreTry = 0;
    double m_coreOkPower = 0, m_coreOkHashrate = 0;
    unsigned m_confirmTries = 0;

    dev::OcSettings m_best;
    double m_bestHashrate = 0, m_bestPowerW = 0;
    bool m_found = false;
};

}  // namespace hashwarp
