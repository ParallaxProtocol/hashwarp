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

#include "Log.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef __APPLE__
#include <pthread.h>
#endif

#include "Guards.h"

using namespace std;
using namespace dev;

// Logging
unsigned g_logOptions = 0;
LogLevel g_logLevel = LogLevel::Info;
bool g_logNoColor = false;
bool g_logSyslog = false;
bool g_logStdout = false;

namespace
{
std::mutex x_logOutput;  // Serializes writes so lines from different threads never interleave

struct LogFile
{
    std::string path;
    std::ofstream out;
    size_t size = 0;
    size_t maxBytes = 0;
    unsigned keep = 0;
} g_logFile;

std::string stripColors(const std::string& _s)
{
    std::string ret;
    ret.reserve(_s.size());
    bool skip = false;
    for (char c : _s)
    {
        if (!skip && c == '\x1b')
            skip = true;
        else if (skip && c == 'm')
            skip = false;
        else if (!skip)
            ret += c;
    }
    return ret;
}

// Must hold x_logOutput
void rotateLogFile()
{
    g_logFile.out.close();
    for (unsigned i = g_logFile.keep; i > 0; i--)
    {
        std::string from = (i == 1) ? g_logFile.path : g_logFile.path + "." + to_string(i - 1);
        std::string to = g_logFile.path + "." + to_string(i);
        std::remove(to.c_str());
        std::rename(from.c_str(), to.c_str());
    }
    if (!g_logFile.keep)
        std::remove(g_logFile.path.c_str());
    g_logFile.out.open(g_logFile.path, std::ios::out | std::ios::trunc);
    g_logFile.size = 0;
}

// Must hold x_logOutput
void writeConsole(const std::string& _line)
{
    std::ostream& os = g_logStdout ? std::cout : std::clog;
    os << (g_logNoColor ? stripColors(_line) : _line) << '\n';
    os.flush();
}

// Must hold x_logOutput
void writeFile(const std::string& _line)
{
    if (!g_logFile.out.is_open())
        return;
    std::string line = stripColors(_line) + '\n';
    g_logFile.out << line;
    g_logFile.out.flush();
    g_logFile.size += line.size();
    if (g_logFile.maxBytes && g_logFile.size >= g_logFile.maxBytes)
        rotateLogFile();
}

std::string formatTime(std::chrono::system_clock::time_point _tp, const char* _fmt)
{
    time_t rawTime = std::chrono::system_clock::to_time_t(_tp);
    struct tm tmBuf;
#if defined(_WIN32)
    localtime_s(&tmBuf, &rawTime);
#else
    localtime_r(&rawTime, &tmBuf);
#endif
    char buf[32];
    if (strftime(buf, sizeof(buf), _fmt, &tmBuf) == 0)
        buf[0] = '\0';  // empty if case strftime fails
    return buf;
}
}  // namespace

LogLevel dev::parseLogLevel(const std::string& _level)
{
    static const std::map<std::string, LogLevel> levels = {{"error", LogLevel::Error},
        {"warn", LogLevel::Warn}, {"warning", LogLevel::Warn}, {"info", LogLevel::Info},
        {"debug", LogLevel::Debug}, {"trace", LogLevel::Trace}};
    auto it = levels.find(_level);
    if (it == levels.end())
        throw std::invalid_argument(
            "Invalid log level '" + _level + "'. Use one of error, warn, info, debug, trace");
    return it->second;
}

const char* dev::logLevelName(LogLevel _level)
{
    switch (_level)
    {
    case LogLevel::Error:
        return "error";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Trace:
        return "trace";
    }
    return "info";
}

void dev::setLogFile(const std::string& _path, size_t _maxBytes, unsigned _keep)
{
    std::lock_guard<std::mutex> l(x_logOutput);
    g_logFile.out.close();
    g_logFile.path = _path;
    g_logFile.maxBytes = _maxBytes;
    g_logFile.keep = _keep;
    g_logFile.out.open(_path, std::ios::out | std::ios::app);
    if (!g_logFile.out.is_open())
        throw std::runtime_error("Can't open log file " + _path);
    g_logFile.out.seekp(0, std::ios::end);
    g_logFile.size = (size_t)g_logFile.out.tellp();
}

const char* LogChannel::name()
{
    return EthGray "..";
}
const char* ErrorChannel::name()
{
    return EthRedBold "!!";
}
const char* WarnChannel::name()
{
    return EthRed " X";
}
const char* NoteChannel::name()
{
    return EthBlue " i";
}
const char* DebugChannel::name()
{
    return EthCoal " d";
}

LogOutputStreamBase::LogOutputStreamBase(char const* _id, LogLevel _level)
  : m_id(_id), m_level(_level), m_enabled(_level <= g_logLevel)
{
    static std::locale logLocl = std::locale("");
    if (m_enabled)
        m_sstr.imbue(logLocl);
}

void LogOutputStreamBase::flush()
{
    if (!m_enabled)
        return;
    try
    {
        write();
    }
    catch (...)
    {
        // Called from destructors: never throw
    }
}

void LogOutputStreamBase::write()
{
    auto now = std::chrono::system_clock::now();
    std::string thread = getThreadName();
    thread.resize(std::max<size_t>(thread.size(), 8), ' ');
    std::string msg = m_sstr.str();

    std::string console;
    if (g_logSyslog)
        console = thread + " " EthReset + msg;
    else
        console = std::string(m_id) + " " EthViolet + formatTime(now, "%H:%M:%S") +
                  " " EthBlue + thread + " " EthReset + msg;

    std::lock_guard<std::mutex> l(x_logOutput);
    writeConsole(console);
    if (g_logFile.out.is_open())
    {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        char msBuf[8];
        snprintf(msBuf, sizeof(msBuf), ".%03d", (int)ms.count());
        writeFile(formatTime(now, "%Y-%m-%d %H:%M:%S") + msBuf + " " +
                  std::string(m_id) + " " + thread + " " + msg);
    }
}

/// Associate a name with each thread for nice logging.
struct ThreadLocalLogName
{
    ThreadLocalLogName(char const* _name) { name = _name; }
    thread_local static char const* name;
};

thread_local char const* ThreadLocalLogName::name;

ThreadLocalLogName g_logThreadName("main");

string dev::getThreadName()
{
#if defined(__linux__) || defined(__APPLE__)
    char buffer[128];
    pthread_getname_np(pthread_self(), buffer, 127);
    buffer[127] = 0;
    return buffer;
#else
    return ThreadLocalLogName::name ? ThreadLocalLogName::name : "<unknown>";
#endif
}

void dev::setThreadName(char const* _n)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), _n);
#elif defined(__APPLE__)
    pthread_setname_np(_n);
#else
    ThreadLocalLogName::name = _n;
#endif
}

void dev::simpleDebugOut(std::string const& _s)
{
    try
    {
        std::lock_guard<std::mutex> l(x_logOutput);
        writeConsole(_s);
        writeFile(_s);
    }
    catch (...)
    {
        return;
    }
}
