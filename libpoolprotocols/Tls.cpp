// TLS helpers: the system's CA certificates and a small HTTPS client.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Tls.h"

#include <hashwarp/buildinfo.h>
#include <libdevcore/Log.h>

#include <boost/asio.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#include <wincrypt.h>
#endif

using boost::asio::ip::tcp;

namespace dev
{
bool loadCaCertificates(boost::asio::ssl::context& _ctx)
{
#ifdef _WIN32
    HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
    if (hStore == nullptr)
    {
        cwarn << "Can't open the Windows certificate store, TLS certificate checks will fail";
        return false;
    }

    X509_STORE* store = X509_STORE_new();
    PCCERT_CONTEXT pContext = nullptr;
    while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != nullptr)
    {
        X509* x509 = d2i_X509(
            nullptr, (const unsigned char**)&pContext->pbCertEncoded, pContext->cbCertEncoded);
        if (x509 != nullptr)
        {
            X509_STORE_add_cert(store, x509);
            X509_free(x509);
        }
    }

    CertFreeCertificateContext(pContext);
    CertCloseStore(hStore, 0);

    SSL_CTX_set_cert_store(_ctx.native_handle(), store);
    return true;
#else
    // OpenSSL is linked statically, its built in default paths point into the build
    // machine: look for the distribution's CA bundle
    const char* certPath = getenv("SSL_CERT_FILE");
    const char* const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch, HiveOS
        "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",              // openSUSE
        "/etc/ssl/cert.pem",                   // Alpine
    };
    for (const char* bundle : bundles)
    {
        boost::system::error_code ec;
        _ctx.load_verify_file(certPath ? certPath : bundle, ec);
        if (!ec)
            return true;
        if (certPath)
            break;
    }
    cwarn << "Can't load the CA certificates from "
          << (certPath ? certPath : "the usual locations")
          << ", TLS certificate checks will likely fail. Set SSL_CERT_FILE to the CA bundle";
    return false;
#endif
}

std::string httpsGet(
    const std::string& _host, const std::string& _path, std::chrono::milliseconds _timeout)
{
    namespace ssl = boost::asio::ssl;
    const size_t maxResponse = 4 << 20;

    boost::asio::io_context io;
    ssl::context ctx(ssl::context::tls_client);
    loadCaCertificates(ctx);
    ssl::stream<tcp::socket> stream(io, ctx);
    // The name for servers with several sites (SNI), and the one the certificate must have
    if (!SSL_set_tlsext_host_name(stream.native_handle(), _host.c_str()))
        throw std::runtime_error("invalid host name " + _host);
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(ssl::host_name_verification(_host));

    // HTTP/1.0: the answer ends with the connection, never in chunks
    auto* bi = hashwarp_get_buildinfo();
    std::string version = bi->project_version;
    std::string request = "GET " + _path + " HTTP/1.0\r\nHost: " + _host +
                          "\r\nUser-Agent: Hashwarp/" + version.substr(0, version.find('+')) +
                          "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    std::string response, error;
    bool done = false;

    tcp::resolver resolver(io);
    auto fail = [&](const boost::system::error_code& _ec) { error = _ec.message(); };
    resolver.async_resolve(_host, "443",
        [&](const boost::system::error_code& _ec, tcp::resolver::results_type _results) {
            if (_ec)
                return fail(_ec);
            boost::asio::async_connect(stream.next_layer(), _results,
                [&](const boost::system::error_code& _ec, const tcp::endpoint&) {
                    if (_ec)
                        return fail(_ec);
                    stream.async_handshake(
                        ssl::stream_base::client, [&](const boost::system::error_code& _ec) {
                            if (_ec)
                                return fail(_ec);
                            boost::asio::async_write(stream, boost::asio::buffer(request),
                                [&](const boost::system::error_code& _ec, size_t) {
                                    if (_ec)
                                        return fail(_ec);
                                    boost::asio::async_read(stream,
                                        boost::asio::dynamic_buffer(response, maxResponse),
                                        [&](const boost::system::error_code& _ec, size_t) {
                                            // The server closes the connection at the end,
                                            // often without a TLS close_notify
                                            if (_ec == boost::asio::error::eof ||
                                                _ec == ssl::error::stream_truncated)
                                                done = true;
                                            else
                                                fail(_ec);
                                        });
                                });
                        });
                });
        });
    io.run_for(_timeout);
    if (!done)
        throw std::runtime_error(error.empty() ? "no answer from " + _host : error);

    int status = 0;
    size_t end = response.find("\r\n\r\n");
    if (end == std::string::npos || sscanf(response.c_str(), "HTTP/%*s %d", &status) != 1)
        throw std::runtime_error("not an HTTP answer from " + _host);
    if (status != 200)
        throw std::runtime_error(_host + _path + " answered HTTP " + std::to_string(status));
    return response.substr(end + 4);
}

}  // namespace dev
