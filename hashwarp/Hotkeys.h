// Hashwarp interactive keys.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace hashwarp
{
/// Reads single key presses from the terminal on a thread of its own and
/// hands them to a handler. Does nothing if stdin is not a terminal
class Hotkeys
{
public:
    explicit Hotkeys(std::function<void(char)> _onKey);
    ~Hotkeys();

    bool active() const { return m_thread.joinable(); }

private:
    void readLoop();

    std::function<void(char)> m_onKey;
    std::atomic<bool> m_stop = {false};
    std::thread m_thread;
};

}  // namespace hashwarp
