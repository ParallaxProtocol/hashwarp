// Hashwarp interactive keys.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Hotkeys.h"
#include "Wizard.h"

#include <chrono>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace hashwarp
{
namespace
{
#if !defined(_WIN32)
// Terminal settings to restore on exit
std::mutex x_terminal;
bool g_terminalChanged = false;
struct termios g_savedTerminal;

void restoreTerminal()
{
    std::lock_guard<std::mutex> l(x_terminal);
    if (g_terminalChanged)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTerminal);
        g_terminalChanged = false;
    }
}

bool rawTerminal()
{
    std::lock_guard<std::mutex> l(x_terminal);
    if (tcgetattr(STDIN_FILENO, &g_savedTerminal) != 0)
        return false;
    struct termios raw = g_savedTerminal;
    raw.c_lflag &= ~(ICANON | ECHO);  // keep ISIG: Ctrl+C still stops hashwarp
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return false;
    if (!g_terminalChanged)
    {
        static bool registered = false;
        if (!registered)
            std::atexit(restoreTerminal);
        registered = true;
    }
    g_terminalChanged = true;
    return true;
}
#endif
}  // namespace

Hotkeys::Hotkeys(std::function<void(char)> _onKey) : m_onKey(std::move(_onKey))
{
    if (!isInteractive())
        return;
#if !defined(_WIN32)
    if (!rawTerminal())
        return;
#endif
    m_thread = std::thread([this]() { readLoop(); });
}

Hotkeys::~Hotkeys()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
#if !defined(_WIN32)
    restoreTerminal();
#endif
}

void Hotkeys::readLoop()
{
    while (!m_stop)
    {
#if defined(_WIN32)
        if (_kbhit())
        {
            int c = _getch();
            if (c == 0 || c == 0xE0)  // function and arrow keys come in two parts
                _getch();
            else
                m_onKey((char)c);
            continue;
        }
        Sleep(100);
#else
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0)
            continue;
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1)
            m_onKey(c);
        else
            return;  // end of input
#endif
    }
}

}  // namespace hashwarp
