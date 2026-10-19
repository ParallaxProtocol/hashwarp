// The --oc-tune search against simulated GPUs

#include <hashwarp/OcTune.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>

using namespace hashwarp;

namespace
{
struct Result
{
    double hashrate;
    double powerW;
    bool errors;
};
using Gpu = std::function<Result(const dev::OcSettings&)>;

// Runs the search to the end, returns the number of steps
int run(OcSearch& _search, const Gpu& _gpu)
{
    int steps = 0;
    while (auto s = _search.next())
    {
        Result r = _gpu(*s);
        _search.report(r.hashrate, r.powerW, r.errors);
        if (++steps > 200)
            break;
    }
    return steps;
}

dev::OcLimits nvidia()
{
    dev::OcLimits l;
    l.minMemOffset = -2000;
    l.maxMemOffset = 6000;
    l.maxCoreClock = 3120;
    l.defaultPowerW = 450;
    return l;
}

// GDDR6X like: the hashrate grows with the memory clock up to +2500, then error
// correction costs more than it gains; wrong results from +3500. The core clock
// (unlocked: 2700 MHz) only matters below 1800 MHz
Result gddr6x(const dev::OcSettings& _s)
{
    int mem = _s.memOffset.value_or(0);
    unsigned core = _s.coreClock && *_s.coreClock ? *_s.coreClock : 2700;
    double h = 115e6 * (1 + 0.03 * std::min(mem, 2500) / 2500.0);
    if (mem > 2500)
        h *= 1 - 0.02 * (mem - 2500) / 250.0;
    h *= std::min(1.0, core / 1800.0);
    return {h, 100 + 0.1 * core + 0.01 * mem, mem >= 3500};
}
}  // namespace

TEST(octune, finds_memory_peak_and_efficient_core)
{
    OcSearch search(nvidia(), true, 2700);
    run(search, gddr6x);
    ASSERT_TRUE(search.found());
    // The peak is +2500; +2250 costs less than the margin
    EXPECT_EQ(*search.best().memOffset, 2250);
    // Lowest core clock keeping 99% of the hashrate, in 50 MHz steps
    unsigned core = *search.best().coreClock;
    EXPECT_GE(core, 1750u);
    EXPECT_LE(core, 1800u);
    EXPECT_GT(search.bestHashrate(), search.baseHashrate() * 1.01);
    EXPECT_LT(search.bestPowerW(), search.basePowerW() * 0.85);
}

TEST(octune, backs_off_from_wrong_results)
{
    // GDDR6: no error correction, wrong results from +1000 while it still gains
    auto gddr6 = [](const dev::OcSettings& _s) {
        int mem = _s.memOffset.value_or(0);
        return Result{100e6 * (1 + 0.00002 * mem), 200, mem >= 1000};
    };
    OcSearch search(nvidia(), true, 0);  // core clock unknown: memory only
    run(search, gddr6);
    ASSERT_TRUE(search.found());
    EXPECT_EQ(*search.best().memOffset, 500);  // the last good one was +750
    EXPECT_EQ(*search.best().coreClock, 0u);    // unlocked
}

TEST(octune, core_only_never_touches_the_memory)
{
    OcSearch search(nvidia(), false, 2700);
    bool memoryChanged = false;
    run(search, [&](const dev::OcSettings& _s) {
        memoryChanged = memoryChanged || _s.memOffset.value_or(0) != 0;
        return gddr6x(_s);
    });
    ASSERT_TRUE(search.found());
    EXPECT_FALSE(memoryChanged);
    EXPECT_EQ(*search.best().memOffset, 0);
    EXPECT_LE(*search.best().coreClock, 1800u);
}

TEST(octune, amd_memory_clock)
{
    dev::OcLimits l;
    l.memStateClock = 1000;
    l.minMemClock = 674;
    l.maxMemClock = 1200;
    l.maxCoreClock = 3150;
    // Gains up to the end of the range
    auto rdna = [](const dev::OcSettings& _s) {
        double mem = _s.memClock.value_or(1000);
        return Result{60e6 * mem / 1000, 180, false};
    };
    OcSearch search(l, true, 0);
    int steps = run(search, rdna);
    ASSERT_TRUE(search.found());
    EXPECT_EQ(*search.best().memClock, 1200u);
    EXPECT_FALSE(search.best().memOffset);
    EXPECT_EQ(steps, 1 + 8 + 1);  // stock, 1025 .. 1200, confirmation
}

TEST(octune, wrong_results_at_stock)
{
    OcSearch search(nvidia(), true, 2700);
    int steps = run(search, [](const dev::OcSettings&) { return Result{100e6, 200, true}; });
    EXPECT_EQ(steps, 1);
    EXPECT_FALSE(search.found());
}

TEST(octune, confirmation_failure_backs_off)
{
    // The first confirmation gives wrong results (it ran longer, or warmer)
    OcSearch reference(nvidia(), true, 2700);
    run(reference, gddr6x);
    int confirmations = 0;
    OcSearch search(nvidia(), true, 2700);
    run(search, [&](const dev::OcSettings& _s) {
        Result r = gddr6x(_s);
        if (search.phase().compare(0, 10, "confirming") == 0 && ++confirmations == 1)
            r.errors = true;
        return r;
    });
    EXPECT_EQ(confirmations, 2);
    ASSERT_TRUE(search.found());
    EXPECT_EQ(*search.best().memOffset, *reference.best().memOffset - OcSearch::c_memOffsetStep);
    EXPECT_EQ(*search.best().coreClock, *reference.best().coreClock + OcSearch::c_coreFineStep);

    // Twice: stock is best
    confirmations = 0;
    OcSearch failing(nvidia(), true, 2700);
    run(failing, [&](const dev::OcSettings& _s) {
        Result r = gddr6x(_s);
        r.errors = r.errors || failing.phase().compare(0, 10, "confirming") == 0;
        return r;
    });
    EXPECT_FALSE(failing.found());
}

TEST(octune, core_only_keeps_a_tuned_memory_clock)
{
    dev::OcSettings tuned;
    tuned.memOffset = 2000;
    tuned.coreClock = 1700;  // ignored: the core clock is what is tuned
    OcSearch search(nvidia(), false, 2700, tuned);
    bool other = false;
    run(search, [&](const dev::OcSettings& _s) {
        other = other || _s.memOffset.value_or(-1) != 2000;
        return gddr6x(_s);
    });
    ASSERT_TRUE(search.found());
    EXPECT_FALSE(other);
    EXPECT_EQ(*search.best().memOffset, 2000);
}
