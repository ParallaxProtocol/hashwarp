// Overclocking options, profiles and tuning results

#include <hashwarp/Oc.h>
#include <hashwarp/Options.h>

#include <CLI/CLI.hpp>
#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

using namespace hashwarp;
namespace fs = boost::filesystem;

namespace
{
Options parse(std::vector<std::string> _args)
{
    Options opts;
    CLI::App app;
    registerOptions(app, opts);
    _args.insert(_args.begin(), "hashwarp");
    std::vector<char*> argv;
    for (auto& a : _args)
        argv.push_back(&a[0]);
    app.parse((int)argv.size(), argv.data());
    return opts;
}

dev::OcLimits nvidiaLimits()
{
    dev::OcLimits l;
    l.minMemOffset = -2000;
    l.maxMemOffset = 6000;
    l.defaultPowerW = 450;
    return l;
}
}  // namespace

TEST(oc, per_gpu_values)
{
    auto o = parse({"--oc-mem-offset", "2000,x,1500", "--oc-power", "75%", "--oc-fan", "auto",
        "--oc-core-clock", "1600"});
    validateOc(o);
    auto r0 = ocRequest(o, 0);
    EXPECT_EQ(*r0.settings.memOffset, 2000);
    EXPECT_EQ(*r0.settings.powerPct, 75u);
    EXPECT_EQ(*r0.settings.fanPct, dev::OcSettings::c_fanAuto);
    EXPECT_EQ(*r0.settings.coreClock, 1600u);
    auto r1 = ocRequest(o, 1);
    EXPECT_FALSE(r1.settings.memOffset);  // x
    EXPECT_EQ(*r1.settings.powerPct, 75u);  // single value: every GPU
    EXPECT_EQ(*ocRequest(o, 2).settings.memOffset, 1500);
    EXPECT_FALSE(ocRequest(o, 3).settings.memOffset);
    EXPECT_TRUE(ocRequest(parse({}), 0).empty());
}

TEST(oc, profiles)
{
    auto o = parse({"--oc-profiles",
        R"({"quiet": {"core-clock": 1400, "power": "70%"}, "fast": {"mem-offset": "3000"}})",
        "--oc-profile", "quiet,fast,efficient", "--oc-power", "250"});
    validateOc(o);
    auto quiet = ocRequest(o, 0);
    EXPECT_EQ(*quiet.settings.coreClock, 1400u);
    EXPECT_EQ(*quiet.settings.powerW, 250u);  // the option overrides the profile
    EXPECT_FALSE(quiet.settings.powerPct);
    EXPECT_EQ(*ocRequest(o, 1).settings.memOffset, 3000);
    auto efficient = ocRequest(o, 2);
    EXPECT_EQ(efficient.gpuProfile, "efficient");
    EXPECT_EQ(*efficient.settings.powerW, 250u);
}

TEST(oc, invalid_values)
{
    EXPECT_THROW(validateOc(parse({"--oc-power", "300x"})), std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-power", "200%"})), std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-core-clock", "50"})), std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-fan", "101"})), std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-profile", "nope"})), std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-profiles", R"({"tuned": {}})"})), std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-profiles", R"({"a": {"memoffset": 1}})"})),
        std::invalid_argument);
    EXPECT_THROW(validateOc(parse({"--oc-profiles", "[1]"})), std::invalid_argument);
    EXPECT_NO_THROW(validateOc(parse({"--oc-core-clock", "0", "--oc-mem-offset", "-500"})));
}

TEST(oc, gpu_profiles)
{
    TuneResults tuned;
    TunedGpu t;
    t.settings.coreClock = 1650;
    t.settings.memOffset = 2500;
    t.date = "2026-10-18";
    tuned["GPU-a"] = t;
    std::string note;

    auto stock = gpuProfile("stock", tuned, "GPU-a", nvidiaLimits(), note);
    ASSERT_TRUE(stock);
    EXPECT_EQ(stock->str(), "core default, core +0 MHz, mem +0 MHz, power 100%, fan auto");
    dev::OcLimits amd;
    amd.maxMemClock = 1200;
    EXPECT_EQ(gpuProfile("stock", tuned, "x", amd, note)->str(), "core default, mem default, fan auto");

    auto efficient = gpuProfile("efficient", tuned, "GPU-a", nvidiaLimits(), note);
    EXPECT_EQ(efficient->str(), "core 1650 MHz");  // never the memory
    efficient = gpuProfile("efficient", tuned, "GPU-b", nvidiaLimits(), note);
    EXPECT_EQ(*efficient->powerPct, c_efficientPowerPct);
    EXPECT_NE(note.find("--oc-tune=core"), std::string::npos);

    EXPECT_EQ(gpuProfile("tuned", tuned, "GPU-a", nvidiaLimits(), note)->str(),
        "core 1650 MHz, mem +2500 MHz");
    EXPECT_FALSE(gpuProfile("tuned", tuned, "GPU-b", nvidiaLimits(), note));
    EXPECT_NE(note.find("--oc-tune"), std::string::npos);
}

TEST(oc, tune_results_file)
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("hashwarp-oc-%%%%-%%%%");
    fs::create_directories(dir);
    std::string path = (dir / "hashwarp-oc.json").string();
    EXPECT_TRUE(loadTuneResults(path).empty());

    TuneResults results;
    TunedGpu a;
    a.name = "NVIDIA GeForce RTX 4090";
    a.mode = "all";
    a.settings.coreClock = 1650;
    a.settings.memOffset = 2500;
    a.hashrate = 121.5e6;
    a.powerW = 301.4;
    results["GPU-a"] = a;
    TunedGpu b;
    b.name = "AMD Radeon RX 6800";
    b.mode = "core";
    b.settings.coreClock = 1500;
    results["amd-7a1c"] = b;
    saveTuneResults(path, results);

    auto loaded = loadTuneResults(path);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded["GPU-a"].name, a.name);
    EXPECT_EQ(loaded["GPU-a"].settings.str(), "core 1650 MHz, mem +2500 MHz");
    EXPECT_EQ(loaded["GPU-a"].hashrate, 121500000.0);
    EXPECT_EQ(loaded["GPU-a"].powerW, 301.0);
    EXPECT_FALSE(loaded["GPU-a"].date.empty());
    EXPECT_EQ(loaded["amd-7a1c"].mode, "core");
    EXPECT_FALSE(loaded["amd-7a1c"].settings.memOffset);

    std::ofstream(path) << "not json";
    EXPECT_THROW(loadTuneResults(path), std::runtime_error);
    fs::remove_all(dir);

    EXPECT_EQ(tuneFilePath(parse({"--oc-tune-file", "/x/y.json"})), "/x/y.json");
    EXPECT_EQ(fs::path(tuneFilePath(parse({}))).filename().string(), "hashwarp-oc.json");
}
