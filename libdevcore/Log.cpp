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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef __APPLE__
#include <pthread.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "Guards.h"

using namespace std;
using namespace dev;

// Logging
unsigned g_logOptions = 0;
LogLevel g_logLevel = LogLevel::Info;
LogFormat g_logFormat = LogFormat::Text;
bool g_logNoColor = false;
bool g_logSyslog = false;
bool g_logStdout = false;
bool g_logUtf8 = true;

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

// What the console can't show, and what it shows instead
const std::pair<const char*, const char*> c_asciiGlyphs[] = {{"\u2714", "+"}, {"\u2718", "x"},
    {"\u2605", "*"}, {"\u00b7", "|"}, {"\u2192", "->"}, {"\u00b0", ""}, {"\u2026", "..."},
    {"\u00d7", "x"}, {"\u2500", "-"}, {"\u2248", "~"}};

std::string asciiGlyphs(std::string _s)
{
    for (const auto& g : c_asciiGlyphs)
    {
        size_t len = strlen(g.first);
        for (size_t pos = _s.find(g.first); pos != std::string::npos;
             pos = _s.find(g.first, pos + strlen(g.second)))
            _s.replace(pos, len, g.second);
    }
    return _s;
}

// Must hold x_logOutput
void writeConsole(const std::string& _line)
{
    std::ostream& os = g_logStdout ? std::cout : std::clog;
    std::string line = g_logNoColor ? stripColors(_line) : _line;
    os << (g_logUtf8 ? line : asciiGlyphs(line)) << '\n';
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

// 2026-10-18T21:44:32.123-03:00
std::string isoTime(std::chrono::system_clock::time_point _tp)
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(_tp.time_since_epoch()) % 1000;
    char msBuf[8];
    snprintf(msBuf, sizeof(msBuf), ".%03d", (int)ms.count());
    std::string zone = formatTime(_tp, "%z");  // -0300
    if (zone.size() == 5)
        zone.insert(3, ":");
    else
        zone.clear();
    return formatTime(_tp, "%Y-%m-%dT%H:%M:%S") + msBuf + zone;
}

void appendJsonString(std::string& _out, const std::string& _s)
{
    _out += '"';
    for (unsigned char c : _s)
    {
        switch (c)
        {
        case '"':
            _out += "\\\"";
            break;
        case '\\':
            _out += "\\\\";
            break;
        case '\n':
            _out += "\\n";
            break;
        case '\t':
            _out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                _out += buf;
            }
            else
                _out += (char)c;
        }
    }
    _out += '"';
}

const char* levelLabel(LogLevel _level)
{
    switch (_level)
    {
    case LogLevel::Error:
        return EthRedBold "ERROR";
    case LogLevel::Warn:
        return EthYellow "WARN ";
    case LogLevel::Info:
        return EthLime "INFO ";
    case LogLevel::Debug:
        return EthCoal "DEBUG";
    case LogLevel::Trace:
        return EthCoal "TRACE";
    }
    return "";
}

// syslog priorities, which journald reads from the start of a line
const char* syslogPriority(LogLevel _level)
{
    switch (_level)
    {
    case LogLevel::Error:
        return "<3>";
    case LogLevel::Warn:
        return "<4>";
    case LogLevel::Info:
        return "<6>";
    default:
        return "<7>";
    }
}

// One color per GPU, so a rig's lines are easy to tell apart
const char* sourceColor(const std::string& _source)
{
    static const char* const gpuColors[] = {
        EthCyan, EthPurple, EthYellow, EthBlue, EthTeal, EthOrange, EthViolet, EthLime};
    if ((_source.compare(0, 3, "gpu") == 0 || _source.compare(0, 3, "cpu") == 0) &&
        _source.size() > 3 && isdigit((unsigned char)_source[3]))
        return gpuColors[atoi(_source.c_str() + 3) % 8];
    return EthWhite;
}

std::string pad(std::string _s, size_t _width)
{
    if (_s.size() < _width)
        _s.resize(_width, ' ');
    return _s;
}
}  // namespace

LogFormat dev::parseLogFormat(const std::string& _format)
{
    if (_format == "text")
        return LogFormat::Text;
    if (_format == "json")
        return LogFormat::Json;
    throw std::invalid_argument("Invalid log format '" + _format + "'. Use text or json");
}

bool dev::consoleUtf8()
{
#ifdef _WIN32
    return SetConsoleOutputCP(CP_UTF8) != 0;
#else
    for (const char* var : {"LC_ALL", "LC_CTYPE", "LANG"})
    {
        const char* v = getenv(var);
        if (!v || !*v)
            continue;
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return tolower(c); });
        return s.find("utf-8") != std::string::npos || s.find("utf8") != std::string::npos;
    }
    return false;
#endif
}

std::string dev::plainText(const std::string& _s, bool _utf8)
{
    std::string s = stripColors(_s);
    return _utf8 ? s : asciiGlyphs(s);
}

std::string dev::gpuLogSource(unsigned _index)
{
    return "gpu" + std::to_string(_index);
}

std::string dev::threadLogSource()
{
    std::string name = getThreadName();
    size_t digits = name.find_last_not_of("0123456789") + 1;
    if (digits > 0 && digits < name.size())
    {
        std::string prefix = name.substr(0, digits);
        if (prefix == "cuda-" || prefix == "cl-")
            return "gpu" + name.substr(digits);
        if (prefix == "cpu-")
            return "cpu" + name.substr(digits);
    }
    if (name == "sim")
        return "pool";
    return "miner";
}

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
    if (_path.empty())
        return;
    g_logFile.out.open(_path, std::ios::out | std::ios::app);
    if (!g_logFile.out.is_open())
        throw std::runtime_error("Can't open log file " + _path);
    g_logFile.out.seekp(0, std::ios::end);
    g_logFile.size = (size_t)g_logFile.out.tellp();
}

LogOutputStreamBase::LogOutputStreamBase(char const* _source, LogLevel _level)
  : m_source(_source), m_level(_level), m_enabled(_level <= g_logLevel)
{
    // The classic locale: no thousands separators in ports, heights and counts
    if (m_enabled)
        m_sstr.imbue(std::locale::classic());
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

void LogOutputStreamBase::field(const char* _key, const std::string& _value)
{
    m_fields += ',';
    appendJsonString(m_fields, _key);
    m_fields += ':';
    appendJsonString(m_fields, _value);
}

void LogOutputStreamBase::field(const char* _key, double _value)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(15) << _value;
    m_fields += ',';
    appendJsonString(m_fields, _key);
    m_fields += ':' + (std::isfinite(_value) ? ss.str() : std::string("null"));
}

void LogOutputStreamBase::field(const char* _key, int64_t _value)
{
    m_fields += ',';
    appendJsonString(m_fields, _key);
    m_fields += ':' + std::to_string(_value);
}

void LogOutputStreamBase::field(const char* _key, bool _value)
{
    m_fields += ',';
    appendJsonString(m_fields, _key);
    m_fields += _value ? ":true" : ":false";
}

void LogOutputStreamBase::write()
{
    auto now = std::chrono::system_clock::now();
    std::string source = m_source ? m_source : threadLogSource();
    std::string msg = m_sstr.str();

    if (g_logFormat == LogFormat::Json)
    {
        std::string line = "{\"time\":\"" + isoTime(now) + "\",\"level\":\"" +
                           logLevelName(m_level) + "\",\"source\":";
        appendJsonString(line, source);
        line += ",\"msg\":";
        appendJsonString(line, stripColors(msg));
        line += m_fields + "}";
        if (g_logLevel >= LogLevel::Debug)
        {
            // Which thread, for debugging
            line.pop_back();
            line += ",\"thread\":";
            appendJsonString(line, getThreadName());
            line += "}";
        }
        std::lock_guard<std::mutex> l(x_logOutput);
        std::ostream& os = g_logStdout ? std::cout : std::clog;
        os << line << '\n';
        os.flush();
        if (g_logFile.out.is_open())
            writeFile(line);
        return;
    }

    // DEBUG  gpu0   message, and the thread when it isn't the source's
    std::string label = std::string(levelLabel(m_level)) + EthReset " " + sourceColor(source) +
                        pad(source, 5) + EthReset "  ";
    if (g_logLevel >= LogLevel::Debug)
    {
        std::string thread = getThreadName();
        if (thread != source)
            msg = EthCoal "[" + thread + "] " EthReset + msg;
    }
    std::string console;
    if (g_logSyslog)
        console = syslogPriority(m_level) + label + msg;
    else
        console = EthCoal + formatTime(now, "%H:%M:%S") + EthReset "  " + label + msg + EthReset;

    std::lock_guard<std::mutex> l(x_logOutput);
    writeConsole(console);
    if (g_logFile.out.is_open())
        writeFile(isoTime(now) + " " + label + msg);
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
