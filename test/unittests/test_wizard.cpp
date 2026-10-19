// First run configuration wizard

#include <hashwarp/Options.h>
#include <hashwarp/Wizard.h>
#include <libpoolprotocols/PoolCheck.h>

#include <CLI/CLI.hpp>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <sstream>

using namespace hashwarp;
namespace fs = boost::filesystem;

namespace
{
struct WizardTest : public ::testing::Test
{
    fs::path dir;
    std::string path;

    void SetUp() override
    {
        dir = fs::temp_directory_path() / fs::unique_path("hashwarp-wiz-%%%%-%%%%");
        fs::create_directories(dir);
        path = (dir / "hashwarp.json").string();
    }
    void TearDown() override { fs::remove_all(dir); }

    // Loads the written configuration like hashwarp --config does
    Options load()
    {
        Options opts;
        CLI::App app;
        registerOptions(app, opts);
        std::vector<std::string> args = {"hashwarp", "--config", path};
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(&a[0]);
        app.parse((int)argv.size(), argv.data());
        opts.soloRequested = app.count("--solo") > 0;
        return opts;
    }
};
}  // namespace

TEST_F(WizardTest, known_pool)
{
    std::istringstream in(
        "\n"                                            // pool (default)
        "x\n"                                           // not a choice: asked again
        "\n"                                            // first listed pool (default)
        "0x1234567890abcdef1234567890abcdef12345678\n"
        "\n"                                            // default worker name
        "\n"                                            // save
        "n\n");                                         // don't start now
    std::ostringstream out;
    EXPECT_FALSE(runWizard(path, in, out));

    Options o = load();
    ASSERT_EQ(o.pools.size(), 1u);
    EXPECT_EQ(o.pools[0], "stratum-eu.rplant.xyz:17159");
    EXPECT_TRUE(o.tls);
    EXPECT_FALSE(o.worker.empty());
    ASSERT_EQ(buildPoolUris(o).size(), 1u);
    EXPECT_EQ(buildPoolUris(o)[0].compare(0, 12, "stratumss://"), 0);
}

TEST_F(WizardTest, majority_pool_listed_last)
{
    dev::eth::NetworkShares shares = dev::eth::parseNetworkShares(
        "{\"decentralization\":{\"blocks\":100,\"topMiners\":["
        "{\"name\":\"rplant.xyz\",\"share\":0.86},{\"name\":\"coolpool.top\",\"share\":0.14}]}}");
    std::istringstream in(
        "\n"                                            // pool (default)
        "\n"                                            // first listed pool (default)
        "0x1234567890abcdef1234567890abcdef12345678\n"
        "\n"                                            // default worker name
        "\n"                                            // save
        "n\n");                                         // don't start now
    std::ostringstream out;
    EXPECT_FALSE(runWizard(path, in, out, &shares));
    EXPECT_NE(out.str().find("1) coolpool.top, PPLNS 1% · 14% of the blocks"), std::string::npos)
        << out.str();
    EXPECT_NE(out.str().find("rplant.xyz, Russia, PPLNS 1% · 86% of the blocks, majority"),
        std::string::npos);
    EXPECT_NE(out.str().find("please choose another one"), std::string::npos);
    EXPECT_EQ(load().pools[0], "prlx.coolpool.top:3003");
}

TEST_F(WizardTest, other_pool)
{
    std::istringstream in(
        "1\n"
        "9\n"                                           // another pool
        "pool.example.com\n"                            // no port: asked again
        "pool.example.com:4443\n"
        "y\n"                                           // TLS
        "0x123\n"                                       // not an address: asked again
        "0x1234567890abcdef1234567890abcdef12345678\n"
        "rig7\n"
        "\n"                                            // save (default yes)
        "n\n");                                         // don't start now
    std::ostringstream out;
    EXPECT_FALSE(runWizard(path, in, out));
    EXPECT_NE(out.str().find("include the port"), std::string::npos);
    EXPECT_NE(out.str().find("not a Parallax address"), std::string::npos);

    Options o = load();
    ASSERT_EQ(o.pools.size(), 1u);
    EXPECT_EQ(o.pools[0], "pool.example.com:4443");
    EXPECT_TRUE(o.tls);
    EXPECT_EQ(o.wallet, "0x1234567890abcdef1234567890abcdef12345678");
    EXPECT_EQ(o.worker, "rig7");
    EXPECT_EQ(buildPoolUris(o).size(), 1u);
}

TEST_F(WizardTest, solo_and_start)
{
    std::istringstream in("2\n\n\n\n");  // solo, default node URL, save, start
    std::ostringstream out;
    EXPECT_TRUE(runWizard(path, in, out));

    Options o = load();
    EXPECT_TRUE(o.soloRequested);
    EXPECT_EQ(o.solo, c_defaultSoloUrl);
}

TEST_F(WizardTest, end_of_input_gives_up)
{
    std::istringstream in("1\n9\npool.example.com:3333\n");
    std::ostringstream out;
    EXPECT_FALSE(runWizard(path, in, out));
    EXPECT_FALSE(fs::exists(path));
}
