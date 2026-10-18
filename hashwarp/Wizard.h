// Hashwarp first run configuration wizard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <iosfwd>
#include <string>

namespace hashwarp
{
/// Whether stdin and stdout are terminals a user can answer questions on
bool isInteractive();

/// Asks how to mine and writes the answers as a JSON configuration to _path.
/// Returns false if the user gave up (end of input) or doesn't want to mine now
bool runWizard(const std::string& _path, std::istream& _in, std::ostream& _out);

}  // namespace hashwarp
