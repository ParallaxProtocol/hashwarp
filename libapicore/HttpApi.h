// Hashwarp HTTP API: request parsing, summary, Prometheus metrics and dashboard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <json/json.h>

#include <map>
#include <string>

namespace hashwarp
{
namespace api
{
struct HttpRequest
{
    std::string method;
    std::string path;   // Without the query string
    std::string query;  // After '?', if any
    std::string version;
    std::map<std::string, std::string> headers;  // Lower case names
    std::string body;
};

enum class ParseResult
{
    Incomplete,  // Wait for more data
    Complete,
    Invalid
};

/// Largest request accepted (headers and body)
static const size_t c_maxRequestSize = 65536;

/// Whether _buffer starts like an HTTP request (as opposed to a JSON-RPC line)
bool looksLikeHttp(const std::string& _buffer);

/// Parses the HTTP request at the start of _buffer. On Complete, _consumed is
/// its length (headers and body)
ParseResult parseHttpRequest(const std::string& _buffer, HttpRequest& _request, size_t& _consumed);

/// A complete HTTP/1.1 response which closes the connection
std::string httpResponse(int _status, const std::string& _contentType, const std::string& _body,
    const std::string& _extraHeaders = "");

/// Compares secrets in constant time
bool secretEquals(const std::string& _a, const std::string& _b);

/// Prometheus text format of an /api/v1/summary document
std::string metricsFromSummary(const Json::Value& _summary);

/// Prometheus text format of the rigs of /api/v1/rigs
std::string metricsFromRigs(const Json::Value& _rigs);

/// The single page dashboard served on /
extern const char* const c_dashboardHtml;

}  // namespace api
}  // namespace hashwarp
