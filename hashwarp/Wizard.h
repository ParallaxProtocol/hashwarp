// Hashwarp first run configuration wizard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <iosfwd>
#include <string>

namespace dev
{
namespace eth
{
struct NetworkShares;
}
}  // namespace dev

namespace hashwarp
{
/// Whether stdin and stdout are terminals a user can answer questions on
bool isInteractive();

/// Asks how to mine and writes the answers as a JSON configuration to _path. With _shares (the
/// pools' shares of the blocks) a pool with the majority is listed last.
/// Returns false if the user gave up (end of input) or doesn't want to mine now
bool runWizard(const std::string& _path, std::istream& _in, std::ostream& _out,
    const dev::eth::NetworkShares* _shares = nullptr);

}  // namespace hashwarp
