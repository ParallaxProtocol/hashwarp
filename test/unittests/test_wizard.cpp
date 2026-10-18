// First run configuration wizard

#include <hashwarp/Options.h>
#include <hashwarp/Wizard.h>

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

TEST_F(WizardTest, pool)
{
    std::istringstream in(
        "1\n"
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
    std::istringstream in("1\npool.example.com:3333\n");
    std::ostringstream out;
    EXPECT_FALSE(runWizard(path, in, out));
    EXPECT_FALSE(fs::exists(path));
}
