// Command line and JSON configuration options

#include <hashwarp/Options.h>
#include <libpoolprotocols/PoolURI.h>

#include <CLI/CLI.hpp>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <fstream>

using namespace hashwarp;
namespace fs = boost::filesystem;

namespace
{
const std::string c_wallet = "0x1234567890abcdef1234567890abcdef12345678";

// Parses _args like the command line of hashwarp
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
    opts.soloRequested = app.count("--solo") > 0;
    return opts;
}
}  // namespace

TEST(options, host_port_pool_gets_wallet_and_worker)
{
    auto o = parse({"--pool", "pool.example.com:3333", "--wallet", c_wallet, "--worker", "rig1"});
    auto uris = buildPoolUris(o);
    ASSERT_EQ(uris.size(), 1u);
    EXPECT_EQ(uris[0], "stratum+tcp://" + c_wallet + ".rig1:x@pool.example.com:3333");

    dev::URI uri(uris[0]);
    EXPECT_EQ(uri.User(), c_wallet);
    EXPECT_EQ(uri.Workername(), "rig1");
    EXPECT_EQ(uri.Host(), "pool.example.com");
    EXPECT_EQ(uri.Port(), 3333);
}

TEST(options, tls_and_esm_pick_the_scheme)
{
    auto o = parse({"-o", "pool.example.com:4443", "-u", c_wallet, "--tls", "--esm", "2"});
    auto uris = buildPoolUris(o);
    ASSERT_EQ(uris.size(), 1u);
    EXPECT_EQ(uris[0].substr(0, 17), "stratum2+ssl://0x");
    EXPECT_EQ(dev::URI(uris[0]).SecLevel(), dev::SecureLevel::TLS12);

    o = parse({"-o", "pool.example.com:3333", "-u", c_wallet, "--esm", "0"});
    EXPECT_EQ(buildPoolUris(o)[0].substr(0, 15), "stratum1+tcp://");
}

TEST(options, failover_lists)
{
    auto o = parse(
        {"--pool", "a.example.com:1,b.example.com:2", "--pool", "c.example.com:3", "-u", c_wallet});
    auto uris = buildPoolUris(o);
    ASSERT_EQ(uris.size(), 3u);
    EXPECT_EQ(dev::URI(uris[1]).Host(), "b.example.com");
    EXPECT_EQ(dev::URI(uris[2]).Port(), 3);
}

TEST(options, urls_keep_their_credentials)
{
    auto o = parse({"-P", "stratum2+tcp://0xother.w:pass@pool.example.com:3333", "-u", c_wallet});
    EXPECT_EQ(buildPoolUris(o)[0], "stratum2+tcp://0xother.w:pass@pool.example.com:3333");

    // Without credentials the wallet is added
    o = parse({"--pool", "stratum+ssl://pool.example.com:4443", "-u", c_wallet, "-w", "r"});
    EXPECT_EQ(dev::URI(buildPoolUris(o)[0]).User(), c_wallet);

    // getwork URLs are left alone
    o = parse({"--pool", "http://127.0.0.1:8545"});
    EXPECT_EQ(buildPoolUris(o)[0], "http://127.0.0.1:8545");
}

TEST(options, worker_names_with_separators)
{
    auto o = parse({"--pool", "pool.example.com:3333", "-u", c_wallet, "-w", "rig.1"});
    dev::URI uri(buildPoolUris(o)[0]);
    EXPECT_EQ(uri.User(), c_wallet);
    EXPECT_EQ(uri.Workername(), "rig.1");
}

TEST(options, solo)
{
    auto o = parse({"--solo"});
    validateOptions(o);
    auto uris = buildPoolUris(o);
    ASSERT_EQ(uris.size(), 1u);
    EXPECT_EQ(uris[0], c_defaultSoloUrl);

    // Pools after a solo node are failovers
    o = parse({"--solo", "http://10.0.0.2:8545", "--pool", "pool.example.com:3333", "-u", c_wallet});
    uris = buildPoolUris(o);
    ASSERT_EQ(uris.size(), 2u);
    EXPECT_EQ(uris[0], "http://10.0.0.2:8545");
}

TEST(options, validation_errors)
{
    auto o = parse({"--pool", "pool.example.com:3333"});
    EXPECT_THROW(buildPoolUris(o), std::invalid_argument);  // no wallet

    o = parse({"--pool", "pool.example.com", "-u", c_wallet});
    EXPECT_THROW(buildPoolUris(o), std::invalid_argument);  // no port

    o = parse({});
    EXPECT_THROW(validateOptions(o), std::invalid_argument);  // nothing to mine on

    o = parse({"--solo", "--temp-stop", "80", "--temp-resume", "85"});
    EXPECT_THROW(validateOptions(o), std::invalid_argument);

    o = parse({"--solo", "--cuda-block-size", "100"});
    EXPECT_THROW(validateOptions(o), std::invalid_argument);

    EXPECT_THROW(parse({"--esm", "5"}), CLI::ValidationError);
    EXPECT_THROW(parse({"--unknown"}), CLI::ExtrasError);
}

TEST(options, wallet_warning)
{
    auto o = parse({"--pool", "pool.example.com:3333", "-u", "not-an-address"});
    auto warnings = validateOptions(o);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("not-an-address"), std::string::npos);
    EXPECT_TRUE(isAddress(c_wallet));
    EXPECT_FALSE(isAddress("0x1234"));
}

TEST(options, per_gpu_lists)
{
    auto o = parse({"--solo", "--temp-stop", "85,80", "--cuda-grid-size", "4096"});
    EXPECT_EQ(perGpu(o.tempStop, 0), 85u);
    EXPECT_EQ(perGpu(o.tempStop, 1), 80u);
    EXPECT_EQ(perGpu(o.tempStop, 2), 0u);  // not given: default
    EXPECT_EQ(perGpu(o.cudaGridSize, 5), 4096u);  // a single value applies to all
    EXPECT_EQ(perGpu(o.cudaStreams, 0), 0u);
}

TEST(options, legacy_names)
{
    auto o = parse({"-U", "--tstop", "85", "--farm-recheck", "500", "--cu-devices", "0,1", "-v", "1"});
    EXPECT_EQ(o.backend, "cuda");
    EXPECT_EQ(perGpu(o.tempStop, 0), 85u);
    EXPECT_EQ(o.getworkInterval, 500u);
    EXPECT_EQ(o.cudaDevices.size(), 2u);
    EXPECT_EQ(debugCategories(o), 1u);
}

TEST(options, json_config_round_trip)
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("hashwarp-cfg-%%%%-%%%%");
    fs::create_directories(dir);
    std::string path = (dir / "hashwarp.json").string();

    // Write
    {
        Options opts;
        CLI::App app;
        registerOptions(app, opts);
        std::vector<std::string> args = {"hashwarp", "--pool", "a.example.com:1,b.example.com:2",
            "--wallet", c_wallet, "--tls", "--temp-stop", "85,80", "--display-interval", "30"};
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(&a[0]);
        app.parse((int)argv.size(), argv.data());
        std::ofstream(path) << configToJson(app);
    }

    // Read back, with a comment and an override on the command line
    {
        std::ifstream in(path);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(json.find("\"temp-stop\" : [85, 80]"), std::string::npos) << json;
        std::ofstream(path) << "// my rig\n" << json;
    }
    auto o = parse({"--config", path, "--display-interval", "15"});
    EXPECT_EQ(o.pools.size(), 2u);
    EXPECT_EQ(o.wallet, c_wallet);
    EXPECT_TRUE(o.tls);
    EXPECT_EQ(o.tempStop, (std::vector<unsigned>{85, 80}));
    EXPECT_EQ(o.displayInterval, 15u);  // the command line wins

    // Unknown keys are errors (typos)
    std::ofstream(path) << "{ \"walet\": \"0x\" }";
    EXPECT_THROW(parse({"--config", path}), CLI::ConfigError);

    fs::remove_all(dir);
}
