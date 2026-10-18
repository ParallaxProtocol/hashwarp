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

#include "XHashAux.h"

// Use xhash implementation for verification so CPU miner (which runs xhash)
// produces results that are verified with the same algorithm.
#include <xhash/global_context.hpp>
#include <xhash/xhash.hpp>

using namespace dev;
using namespace eth;

Result XHashAux::eval(int epoch, h256 const& _headerHash, uint64_t _nonce) noexcept
{
    auto headerHash = xhash::hash256_from_bytes(_headerHash.data());
    // The light context (shared with Farm) is enough to verify a handful of shares:
    // the full one would lazily fill a host buffer as large as the DAG.
    auto& context = xhash::get_global_epoch_context(epoch);
    auto result = xhash::hash(context, headerHash, _nonce);
    h256 mix{reinterpret_cast<byte*>(result.mix_hash.bytes), h256::ConstructFromPointer};
    h256 final{reinterpret_cast<byte*>(result.final_hash.bytes), h256::ConstructFromPointer};
    return {final, mix};
}