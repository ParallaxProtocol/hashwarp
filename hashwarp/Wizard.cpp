// Hashwarp first run configuration wizard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Wizard.h"
#include "Options.h"

#include <json/json.h>

#include <boost/asio/ip/host_name.hpp>

#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#include <io.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace hashwarp
{
namespace
{
const char* const c_poolsGuide = "https://docs.parallaxprotocol.org/v2.1.x/guides/mining/pool";

struct EndOfInput
{
};

std::string trim(std::string _s)
{
    _s.erase(0, _s.find_first_not_of(" \t\r\n"));
    _s.erase(_s.find_last_not_of(" \t\r\n") + 1);
    return _s;
}

std::string ask(std::istream& _in, std::ostream& _out, const std::string& _question,
    const std::string& _default = "")
{
    _out << _question;
    if (!_default.empty())
        _out << " [" << _default << "]";
    _out << ": " << std::flush;
    std::string answer;
    if (!std::getline(_in, answer))
        throw EndOfInput();
    answer = trim(answer);
    return answer.empty() ? _default : answer;
}

bool askYesNo(std::istream& _in, std::ostream& _out, const std::string& _question, bool _default)
{
    while (true)
    {
        std::string a = ask(_in, _out, _question + (_default ? " (Y/n)" : " (y/N)"));
        if (a.empty())
            return _default;
        char c = (char)std::tolower((unsigned char)a[0]);
        if (c == 'y')
            return true;
        if (c == 'n')
            return false;
    }
}

std::string defaultWorker()
{
    std::string name;
    try
    {
        name = boost::asio::ip::host_name();
    }
    catch (...)
    {
    }
    // Keep it simple for pools: letters, digits, dashes and underscores
    std::string ret;
    for (char c : name)
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_')
            ret += c;
    return ret.empty() ? "rig" : ret.substr(0, 32);
}
}  // namespace

bool isInteractive()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

bool runWizard(const std::string& _path, std::istream& _in, std::ostream& _out)
{
    Json::Value config(Json::objectValue);
    try
    {
        _out << "Welcome to hashwarp! No configuration was found, let's create one.\n"
             << "(press Ctrl+C to quit at any time)\n\n"
             << "How do you want to mine?\n"
             << "  1) On a mining pool\n"
             << "  2) Solo, on your own Parallax node\n";
        std::string mode;
        while (mode != "1" && mode != "2")
            mode = ask(_in, _out, "Choice", "1");

        if (mode == "1")
        {
            _out << "\nParallax pools are listed at " << c_poolsGuide << "\n";
            std::string pool;
            while (true)
            {
                pool = ask(_in, _out, "Pool address as host:port");
                if (pool.find("://") != std::string::npos ||
                    (pool.find(':') != std::string::npos && pool.back() != ':'))
                    break;
                _out << "Please include the port, e.g. pool.example.com:3333\n";
            }
            config["pool"] = Json::Value(Json::arrayValue);
            config["pool"].append(pool);
            if (pool.find("://") == std::string::npos &&
                askYesNo(_in, _out, "Does the pool use TLS (SSL) on this port?", false))
                config["tls"] = true;

            std::string wallet;
            while (!isAddress(wallet))
            {
                wallet = ask(_in, _out, "Your Parallax address (0x...)");
                if (!isAddress(wallet))
                    _out << "That's not a Parallax address: 0x followed by 40 hex digits\n";
            }
            config["wallet"] = wallet;
            config["worker"] = ask(_in, _out, "Worker name", defaultWorker());
        }
        else
        {
            _out << "\nStart your node with mining enabled, e.g.\n"
                 << "  parallaxd --http --http.api eth,net,web3,miner --mine "
                    "--miner.coinbase <your address>\n"
                 << "Rewards go to the node's --miner.coinbase address.\n";
            config["solo"] = ask(_in, _out, "Node URL", c_defaultSoloUrl);
        }

        _out << "\n";
        if (!askYesNo(_in, _out, "Save this configuration to " + _path + "?", true))
            return false;
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "    ";
        std::ofstream out(_path);
        if (!out)
            throw std::runtime_error("Can't write " + _path);
        out << Json::writeString(builder, config) << "\n";
        out.close();
        _out << "Saved. Edit it any time, or run 'hashwarp --help' for all the options.\n\n";

        return askYesNo(_in, _out, "Start mining now?", true);
    }
    catch (const EndOfInput&)
    {
        _out << "\n";
        return false;
    }
}

}  // namespace hashwarp
