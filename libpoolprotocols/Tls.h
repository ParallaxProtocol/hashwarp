// TLS helpers: the system's CA certificates and a small HTTPS client.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <boost/asio/ssl.hpp>

#include <chrono>
#include <string>

namespace dev
{
/// Loads the system's CA certificates into _ctx: the Windows store, or the distribution's bundle
/// (SSL_CERT_FILE overrides it). Returns false, and logs why, when none could be loaded
bool loadCaCertificates(boost::asio::ssl::context& _ctx);

/// GETs https://_host_path and returns the body of a 200 answer, within _timeout. Throws
/// std::runtime_error with the reason otherwise
std::string httpsGet(
    const std::string& _host, const std::string& _path, std::chrono::milliseconds _timeout);

}  // namespace dev
