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
#include <ctime>

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

/// Parses "error", "warn", "info", "debug" or "trace". Throws std::invalid_argument
LogLevel parseLogLevel(const std::string& _level);
const char* logLevelName(LogLevel _level);
}  // namespace dev

extern unsigned g_logOptions;      // Debug categories (LOG_JSON ...)
extern dev::LogLevel g_logLevel;   // Lines above this level are dropped
extern bool g_logNoColor;
extern bool g_logSyslog;
extern bool g_logStdout;

namespace dev
{
/// Logs a line which is already formatted (no prefix is added)
void simpleDebugOut(std::string const&);

/// Writes log lines also to _path (without colors, with full timestamps).
/// The file is rotated when it grows over _maxBytes: _path.1 ... _path.<_keep>
/// Throws std::runtime_error if the file can't be opened
void setLogFile(const std::string& _path, size_t _maxBytes = 20 * 1024 * 1024, unsigned _keep = 3);

/// Set the current thread's log name.
void setThreadName(char const* _n);

/// Set the current thread's log name.
std::string getThreadName();

/// The default logging channels. Each has a level and a short prefix (name()).
/// Channels should inherit from LogChannel and may override name() and level.
struct LogChannel
{
    static const char* name();
    static const LogLevel level = LogLevel::Info;
};
struct ErrorChannel : public LogChannel
{
    static const char* name();
    static const LogLevel level = LogLevel::Error;
};
struct WarnChannel : public LogChannel
{
    static const char* name();
    static const LogLevel level = LogLevel::Warn;
};
struct NoteChannel : public LogChannel
{
    static const char* name();
};
struct DebugChannel : public LogChannel
{
    static const char* name();
    static const LogLevel level = LogLevel::Debug;
};

class LogOutputStreamBase
{
public:
    LogOutputStreamBase(char const* _id, LogLevel _level);

    template <class T>
    void append(T const& _t)
    {
        if (m_enabled)
            m_sstr << _t;
    }

protected:
    /// Formats and writes the accrued entry to the console and the log file
    void flush();

    char const* m_id;
    LogLevel m_level;
    bool m_enabled;
    std::stringstream m_sstr;  ///< The accrued log entry.
};

/// Logging class, iostream-like, that can be shifted to.
template <class Id>
class LogOutputStream : LogOutputStreamBase
{
public:
    LogOutputStream() : LogOutputStreamBase(Id::name(), Id::level) {}

    /// Destructor. Writes the accrued log entry.
    ~LogOutputStream() { flush(); }

    /// Shift arbitrary data to the log. Spaces will be added between items as required.
    template <class T>
    LogOutputStream& operator<<(T const& _t)
    {
        append(_t);
        return *this;
    }
};

#define clog(X) dev::LogOutputStream<X>()

// Simple cout-like stream objects for accessing common log channels.
// Dirties the global namespace, but oh so convenient...
#define cerror clog(dev::ErrorChannel)
#define cwarn clog(dev::WarnChannel)
#define cnote clog(dev::NoteChannel)
#define cdebug clog(dev::DebugChannel)

}  // namespace dev
