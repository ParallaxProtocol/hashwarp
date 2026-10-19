// Search of the best memory clock and the most efficient core clock of a GPU (--oc-tune).
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "OcTune.h"

#include <algorithm>

namespace hashwarp
{
namespace
{
// Highest memory offset tried: XHash gains nothing measurable beyond it
const int c_memOffsetMax = 5000;
}  // namespace

OcSearch::OcSearch(const dev::OcLimits& _limits, bool _memory, unsigned _coreMHz,
    const dev::OcSettings& _fixedMemory)
  : m_limits(_limits),
    m_memory(_memory),
    m_nvidia(_limits.minMemOffset || _limits.maxMemOffset),
    m_coreStart(_coreMHz)
{
    m_fixedMemory.memOffset = _fixedMemory.memOffset;
    m_fixedMemory.memClock = _fixedMemory.memClock;
}

dev::OcSettings OcSearch::memSettings(int _step) const
{
    if (!m_memory && !m_fixedMemory.empty())
        return m_fixedMemory;
    dev::OcSettings s;
    if (m_nvidia)
        s.memOffset = _step * c_memOffsetStep;
    else if (m_limits.memStateClock)
        s.memClock = m_limits.memStateClock + _step * c_memClockStep;
    return s;
}

dev::OcSettings OcSearch::withCore(unsigned _mhz) const
{
    dev::OcSettings s = m_mem;
    s.coreClock = _mhz;
    return s;
}

std::optional<dev::OcSettings> OcSearch::next()
{
    switch (m_phase)
    {
    case Phase::Base:
        m_trying = memSettings(0);
        m_trying.coreClock = 0;
        break;
    case Phase::Memory:
        m_trying = memSettings(m_memStep);
        m_trying.coreClock = 0;
        break;
    case Phase::CoreCoarse:
    case Phase::CoreFine:
        m_trying = withCore(m_coreTry);
        break;
    case Phase::Confirm:
        m_trying = m_coreOk ? withCore(m_coreOk) : m_mem;
        if (!m_trying.coreClock)
            m_trying.coreClock = 0;
        break;
    case Phase::Done:
        return std::nullopt;
    }
    return m_trying;
}

std::string OcSearch::phase() const
{
    switch (m_phase)
    {
    case Phase::Base:
        return "stock settings";
    case Phase::Memory:
        return "memory " + m_trying.str();
    case Phase::CoreCoarse:
    case Phase::CoreFine:
        return "core clock " + std::to_string(m_coreTry) + " MHz";
    case Phase::Confirm:
        return "confirming " + m_trying.str();
    case Phase::Done:
        break;
    }
    return "done";
}

void OcSearch::report(double _hashrate, double _powerW, bool _errors)
{
    switch (m_phase)
    {
    case Phase::Base:
    {
        m_baseHashrate = _hashrate;
        m_basePowerW = _powerW;
        if (_errors || _hashrate <= 0)
        {
            // Wrong results at stock settings: nothing to tune
            m_phase = Phase::Done;
            return;
        }
        m_memHashrates = {_hashrate};
        bool tunable = m_nvidia ? m_limits.maxMemOffset >= c_memOffsetStep :
                                  m_limits.memStateClock &&
                                      m_limits.maxMemClock >= m_limits.memStateClock + c_memClockStep;
        if (m_memory && tunable)
        {
            m_memStep = 1;
            m_phase = Phase::Memory;
        }
        else
        {
            m_mem = memSettings(0);
            m_memHashrate = _hashrate;
            finishMemory(false);
        }
        return;
    }
    case Phase::Memory:
    {
        if (_errors)
        {
            // This step is too far: the margin comes from the step before the best
            finishMemory(true);
            return;
        }
        double best = *std::max_element(m_memHashrates.begin(), m_memHashrates.end());
        m_memHashrates.push_back(_hashrate);
        m_drops = _hashrate < best * c_memDrop ? m_drops + 1 : 0;
        m_memStep++;
        bool atLimit = m_nvidia ? (m_memStep * c_memOffsetStep > std::min(m_limits.maxMemOffset,
                                                                    c_memOffsetMax)) :
                                  (m_limits.memStateClock + m_memStep * c_memClockStep >
                                      m_limits.maxMemClock);
        if (m_drops >= 2 || atLimit)
            finishMemory(false);
        return;
    }
    case Phase::CoreCoarse:
    case Phase::CoreFine:
    {
        bool ok = !_errors && _hashrate >= m_memHashrate * c_coreKeep;
        if (m_phase == Phase::CoreCoarse)
        {
            if (ok)
            {
                m_coreOk = m_coreTry;
                if (m_coreTry < c_coreMin + c_coreCoarseStep)
                    finishCore();
                else
                    m_coreTry -= c_coreCoarseStep;
                return;
            }
            // Too low: go up from here in fine steps
            m_phase = Phase::CoreFine;
        }
        else if (ok)
        {
            m_coreOk = m_coreTry;
            finishCore();
            return;
        }
        m_coreTry += c_coreFineStep;
        unsigned ceiling = m_coreOk ? m_coreOk : m_coreStart;
        if (m_coreTry >= ceiling)
            finishCore();
        return;
    }
    case Phase::Confirm:
    {
        double reference = m_memHashrate;
        if (!_errors && _hashrate >= reference * c_coreKeep)
        {
            m_best = m_trying;
            m_bestHashrate = _hashrate;
            m_bestPowerW = _powerW;
            m_found = true;
            m_phase = Phase::Done;
            return;
        }
        // Not stable after all: back off once, then give up
        if (++m_confirmTries >= 2)
        {
            m_phase = Phase::Done;
            return;
        }
        if (m_coreOk)
            m_coreOk += c_coreFineStep;
        if (m_mem.memOffset && *m_mem.memOffset >= c_memOffsetStep)
            *m_mem.memOffset -= c_memOffsetStep;
        if (m_mem.memClock && *m_mem.memClock >= m_limits.memStateClock + c_memClockStep)
            *m_mem.memClock -= c_memClockStep;
        return;
    }
    case Phase::Done:
        return;
    }
}

void OcSearch::finishMemory(bool _errors)
{
    if (m_phase == Phase::Memory)
    {
        size_t best = std::max_element(m_memHashrates.begin(), m_memHashrates.end()) -
                      m_memHashrates.begin();
        size_t choice = best;
        // One step below when it costs (nearly) nothing, or the next one failed
        if (choice > 0 && (m_memHashrates[choice - 1] >= m_memHashrates[best] * c_memMargin ||
                              (_errors && choice + 1 == m_memHashrates.size())))
            choice--;
        m_mem = memSettings((int)choice);
        m_memHashrate = m_memHashrates[choice];
    }
    if (m_coreStart >= c_coreMin + c_coreCoarseStep)
    {
        m_coreTry = m_coreStart - c_coreCoarseStep;
        m_phase = Phase::CoreCoarse;
    }
    else
        finishCore();
}

void OcSearch::finishCore()
{
    m_phase = Phase::Confirm;
}

}  // namespace hashwarp
