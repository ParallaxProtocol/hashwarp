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
/** @file Log.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 *
 * The logging subsystem.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <type_traits>

#include "Common.h"
#include "CommonData.h"
#include "FixedHash.h"
#include "Terminal.h"
#include "vector_ref.h"

/// Debug categories (--log-debug / legacy -v bitmask). Lines in these
/// categories are written at debug level.
#define LOG_JSON 1
#define LOG_PER_GPU 2
#define LOG_CONNECT 32
#define LOG_SWITCH 64
#define LOG_SUBMIT 128
#define LOG_PROGRAMFLOW 256
#define LOG_NEXT 512

#if DEV_BUILD
#define DEV_BUILD_LOG_PROGRAMFLOW(_S, _V) \
    if (g_logOptions & LOG_PROGRAMFLOW)   \
    {                                     \
        _S << _V;                         \
    }                                     \
    ((void)(0))
#else
#define DEV_BUILD_LOG_PROGRAMFLOW(_S, _V) ((void)(0))
#endif

/// The source of a file's lines ("pool", "api" ...). Define it before including
/// any header. Without it, the source comes from the thread: gpuN for a GPU's
/// thread, miner for the others
#ifndef LOG_SOURCE
#define LOG_SOURCE nullptr
#endif

namespace dev
{
enum class LogLevel
{
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace
};

enum class LogFormat
{
    Text,  // time, level, source, message
    Json   // one object per line
};

/// Parses "error", "warn", "info", "debug" or "trace". Throws std::invalid_argument
LogLevel parseLogLevel(const std::string& _level);
const char* logLevelName(LogLevel _level);

/// Parses "text" or "json". Throws std::invalid_argument
LogFormat parseLogFormat(const std::string& _format);
}  // namespace dev

extern unsigned g_logOptions;      // Debug categories (LOG_JSON ...)
extern dev::LogLevel g_logLevel;   // Lines above this level are dropped
extern dev::LogFormat g_logFormat;
extern bool g_logNoColor;
extern bool g_logSyslog;
extern bool g_logStdout;
extern bool g_logUtf8;  // Whether the console shows UTF-8; ASCII replacements otherwise

namespace dev
{
/// Logs a line which is already formatted (no prefix is added)
void simpleDebugOut(std::string const&);

/// Writes log lines also to _path (without colors, with full timestamps).
/// The file is rotated when it grows over _maxBytes: _path.1 ... _path.<_keep>.
/// An empty _path closes the file. Throws std::runtime_error if the file can't be opened
void setLogFile(const std::string& _path, size_t _maxBytes = 20 * 1024 * 1024, unsigned _keep = 3);

/// Whether the console can show UTF-8 (from the locale; on Windows it switches the
/// console to UTF-8)
bool consoleUtf8();

/// Set the current thread's log name.
void setThreadName(char const* _n);

/// Set the current thread's log name.
std::string getThreadName();

/// The source of a line logged from this thread: gpuN or cpuN for a miner's thread,
/// miner for the others
std::string threadLogSource();

/// The source of lines about GPU _index: gpuN
std::string gpuLogSource(unsigned _index);

/// Text of a line with colors and glyphs removed or replaced, as the console shows
/// it without colors and UTF-8
std::string plainText(const std::string& _s, bool _utf8 = true);

/// The levels of the logging channels. Channels inherit from LogChannel and set level
struct LogChannel
{
    static const LogLevel level = LogLevel::Info;
};
struct ErrorChannel : public LogChannel
{
    static const LogLevel level = LogLevel::Error;
};
struct WarnChannel : public LogChannel
{
    static const LogLevel level = LogLevel::Warn;
};
struct NoteChannel : public LogChannel
{
};
struct DebugChannel : public LogChannel
{
    static const LogLevel level = LogLevel::Debug;
};

class LogOutputStreamBase
{
public:
    LogOutputStreamBase(char const* _source, LogLevel _level);

    template <class T>
    void append(T const& _t)
    {
        if (m_enabled)
            m_sstr << _t;
    }

    /// A field of the JSON format only (the text has the information already)
    void field(const char* _key, const std::string& _value);
    void field(const char* _key, const char* _value) { field(_key, std::string(_value)); }
    void field(const char* _key, double _value);
    void field(const char* _key, int64_t _value);
    void field(const char* _key, bool _value);

protected:
    /// Formats and writes the accrued entry to the console and the log file
    void flush();
    void write();

    char const* m_source;
    LogLevel m_level;
    bool m_enabled;
    std::stringstream m_sstr;  ///< The accrued log entry.
    std::string m_fields;      ///< ,"key":value ... (JSON)
};

/// Logging class, iostream-like, that can be shifted to.
template <class Id>
class LogOutputStream : LogOutputStreamBase
{
public:
    LogOutputStream(const char* _source = nullptr) : LogOutputStreamBase(_source, Id::level) {}

    /// Destructor. Writes the accrued log entry.
    ~LogOutputStream() { flush(); }

    /// Shift arbitrary data to the log. Spaces will be added between items as required.
    template <class T>
    LogOutputStream& operator<<(T const& _t)
    {
        append(_t);
        return *this;
    }

    /// Adds a field to the JSON line, e.g. cnote.with("gpu", 0) << ...
    template <class T>
    LogOutputStream& with(const char* _key, T const& _value)
    {
        if (!m_enabled || g_logFormat != LogFormat::Json)
            return *this;
        if constexpr (std::is_same<T, bool>::value)
            field(_key, _value);
        else if constexpr (std::is_integral<T>::value)
            field(_key, (int64_t)_value);
        else if constexpr (std::is_floating_point<T>::value)
            field(_key, (double)_value);
        else
            field(_key, std::string(_value));
        return *this;
    }
};

#define clog(X) dev::LogOutputStream<X>(LOG_SOURCE)
/// A line of another source than the file's, e.g. clogs(dev::NoteChannel, "share")
#define clogs(X, S) dev::LogOutputStream<X>(S)

// Simple cout-like stream objects for accessing common log channels.
// Dirties the global namespace, but oh so convenient...
#define cerror clog(dev::ErrorChannel)
#define cwarn clog(dev::WarnChannel)
#define cnote clog(dev::NoteChannel)
#define cdebug clog(dev::DebugChannel)

// Lines about a GPU, from another thread than the GPU's
#define cgpuerror(I) clogs(dev::ErrorChannel, dev::gpuLogSource(I).c_str())
#define cgpuwarn(I) clogs(dev::WarnChannel, dev::gpuLogSource(I).c_str())
#define cgpunote(I) clogs(dev::NoteChannel, dev::gpuLogSource(I).c_str())

}  // namespace dev
