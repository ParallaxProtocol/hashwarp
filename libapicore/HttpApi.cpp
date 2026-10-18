// Hashwarp HTTP API: request parsing, summary, Prometheus metrics and dashboard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "HttpApi.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hashwarp
{
namespace api
{
namespace
{
std::string lower(std::string _s)
{
    std::transform(_s.begin(), _s.end(), _s.begin(), [](unsigned char c) { return std::tolower(c); });
    return _s;
}

std::string trim(const std::string& _s)
{
    size_t b = _s.find_first_not_of(" \t");
    size_t e = _s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : _s.substr(b, e - b + 1);
}

const char* reasonPhrase(int _status)
{
    switch (_status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

std::string escapeLabel(const std::string& _s)
{
    std::string ret;
    for (char c : _s)
    {
        if (c == '\\' || c == '"')
            ret += '\\';
        if (c == '\n')
        {
            ret += "\\n";
            continue;
        }
        ret += c;
    }
    return ret;
}

class Metrics
{
public:
    Metrics() { m_out.precision(15); }

    void family(const std::string& _name, const char* _type, const char* _help)
    {
        m_out << "# HELP " << _name << " " << _help << "\n# TYPE " << _name << " " << _type << "\n";
    }

    void sample(const std::string& _name, const std::string& _labels, double _value)
    {
        m_out << _name;
        if (!_labels.empty())
            m_out << "{" << _labels << "}";
        m_out << " " << _value << "\n";
    }

    std::string str() const { return m_out.str(); }

private:
    std::ostringstream m_out;
};

std::string gpuLabels(const Json::Value& _gpu)
{
    return "gpu=\"" + std::to_string(_gpu["index"].asInt()) + "\",name=\"" +
           escapeLabel(_gpu["name"].asString()) + "\"";
}
}  // namespace

bool looksLikeHttp(const std::string& _buffer)
{
    static const char* const methods[] = {"GET ", "POST ", "PUT ", "DELETE ", "HEAD ", "OPTIONS "};
    for (const char* m : methods)
    {
        std::string method(m);
        size_t n = std::min(method.size(), _buffer.size());
        if (_buffer.compare(0, n, method, 0, n) == 0)
            return true;
    }
    return false;
}

ParseResult parseHttpRequest(const std::string& _buffer, HttpRequest& _request, size_t& _consumed)
{
    size_t headerEnd = _buffer.find("\r\n\r\n");
    size_t sepLen = 4;
    if (headerEnd == std::string::npos)
    {
        headerEnd = _buffer.find("\n\n");
        sepLen = 2;
    }
    if (headerEnd == std::string::npos)
        return _buffer.size() > c_maxRequestSize ? ParseResult::Invalid : ParseResult::Incomplete;

    std::istringstream head(_buffer.substr(0, headerEnd));
    std::string line;
    if (!std::getline(head, line))
        return ParseResult::Invalid;
    std::istringstream requestLine(line);
    std::string target;
    if (!(requestLine >> _request.method >> target >> _request.version) || target.empty() ||
        target[0] != '/' || _request.version.compare(0, 5, "HTTP/") != 0)
        return ParseResult::Invalid;
    _request.version = trim(_request.version);
    size_t q = target.find('?');
    _request.path = target.substr(0, q);
    _request.query = (q == std::string::npos) ? "" : target.substr(q + 1);

    _request.headers.clear();
    while (std::getline(head, line))
    {
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        _request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    size_t bodyLength = 0;
    auto cl = _request.headers.find("content-length");
    if (cl != _request.headers.end())
    {
        try
        {
            bodyLength = std::stoul(cl->second);
        }
        catch (const std::exception&)
        {
            return ParseResult::Invalid;
        }
    }
    size_t total = headerEnd + sepLen + bodyLength;
    if (total > c_maxRequestSize)
        return ParseResult::Invalid;
    if (_buffer.size() < total)
        return ParseResult::Incomplete;
    _request.body = _buffer.substr(headerEnd + sepLen, bodyLength);
    _consumed = total;
    return ParseResult::Complete;
}

std::string httpResponse(int _status, const std::string& _contentType, const std::string& _body,
    const std::string& _extraHeaders)
{
    std::ostringstream ss;
    ss << "HTTP/1.1 " << _status << " " << reasonPhrase(_status) << "\r\n"
       << "Server: hashwarp\r\n"
       << "Content-Type: " << _contentType << "\r\n"
       << "Content-Length: " << _body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << _extraHeaders << "Connection: close\r\n\r\n"
       << _body;
    return ss.str();
}

bool secretEquals(const std::string& _a, const std::string& _b)
{
    unsigned char diff = _a.size() != _b.size();
    for (size_t i = 0; i < _a.size(); i++)
        diff |= (unsigned char)(_a[i] ^ (i < _b.size() ? _b[i] : 0));
    return diff == 0;
}

std::string metricsFromSummary(const Json::Value& _s)
{
    Metrics m;
    const Json::Value& gpus = _s["gpus"];

    m.family("hashwarp_info", "gauge", "Hashwarp version");
    m.sample("hashwarp_info", "version=\"" + escapeLabel(_s["miner"]["version"].asString()) + "\"", 1);
    m.family("hashwarp_uptime_seconds", "gauge", "Seconds since hashwarp started");
    m.sample("hashwarp_uptime_seconds", "", _s["miner"]["uptime"].asDouble());

    m.family("hashwarp_hashrate", "gauge", "Hashes per second, total and per GPU");
    for (const char* w : {"now", "60s", "15m"})
    {
        m.sample("hashwarp_hashrate", std::string("window=\"") + w + "\"",
            _s["hashrate"][w].asDouble());
        for (const auto& g : gpus)
            m.sample("hashwarp_hashrate", gpuLabels(g) + ",window=\"" + w + "\"",
                g["hashrate"][w].asDouble());
    }
    m.family("hashwarp_effective_hashrate", "gauge",
        "Hashes per second the accepted shares of the last 15 minutes are worth");
    m.sample("hashwarp_effective_hashrate", "", _s["hashrate"]["effective"].asDouble());

    m.family("hashwarp_shares_total", "counter", "Shares found, by result");
    for (const char* r : {"accepted", "rejected", "failed"})
    {
        m.sample("hashwarp_shares_total", std::string("result=\"") + r + "\"",
            _s["shares"][r].asDouble());
        for (const auto& g : gpus)
            m.sample("hashwarp_shares_total", gpuLabels(g) + ",result=\"" + r + "\"",
                g["shares"][r].asDouble());
    }

    m.family("hashwarp_gpu_paused", "gauge", "Whether a GPU is paused");
    for (const auto& g : gpus)
        m.sample("hashwarp_gpu_paused", gpuLabels(g), g["paused"].asBool() ? 1 : 0);

    m.family("hashwarp_gpu_temperature_celsius", "gauge", "GPU temperatures");
    for (const auto& g : gpus)
        for (const char* sensor : {"core", "hotspot", "memory"})
            if (g["temperature"][sensor].asInt() > 0)
                m.sample("hashwarp_gpu_temperature_celsius",
                    gpuLabels(g) + ",sensor=\"" + sensor + "\"",
                    g["temperature"][sensor].asDouble());
    m.family("hashwarp_gpu_fan_percent", "gauge", "GPU fan speed");
    m.family("hashwarp_gpu_power_watts", "gauge", "GPU power draw");
    m.family("hashwarp_gpu_power_limit_watts", "gauge", "GPU power limit");
    m.family("hashwarp_gpu_clock_mhz", "gauge", "GPU clocks");
    for (const auto& g : gpus)
    {
        if (g["temperature"]["core"].asInt() > 0)
            m.sample("hashwarp_gpu_fan_percent", gpuLabels(g), g["fan"].asDouble());
        if (g["power"].asDouble() > 0)
            m.sample("hashwarp_gpu_power_watts", gpuLabels(g), g["power"].asDouble());
        if (g["power_limit"].asDouble() > 0)
            m.sample("hashwarp_gpu_power_limit_watts", gpuLabels(g), g["power_limit"].asDouble());
        if (g["clocks"]["core"].asInt() > 0)
        {
            m.sample("hashwarp_gpu_clock_mhz", gpuLabels(g) + ",clock=\"core\"",
                g["clocks"]["core"].asDouble());
            m.sample("hashwarp_gpu_clock_mhz", gpuLabels(g) + ",clock=\"memory\"",
                g["clocks"]["memory"].asDouble());
        }
    }

    const Json::Value& pool = _s["pool"];
    m.family("hashwarp_pool_connected", "gauge", "Whether hashwarp is connected to a pool or node");
    m.sample("hashwarp_pool_connected", "host=\"" + escapeLabel(pool["host"].asString()) + "\"",
        pool["connected"].asBool() ? 1 : 0);
    m.family("hashwarp_pool_difficulty_hashes", "gauge", "Expected hashes per share");
    m.sample("hashwarp_pool_difficulty_hashes", "", pool["difficulty"].asDouble());
    m.family("hashwarp_pool_switches_total", "counter", "Pool connection switches");
    m.sample("hashwarp_pool_switches_total", "", pool["switches"].asDouble());
    m.family("hashwarp_epoch", "gauge", "Current XHash epoch");
    m.sample("hashwarp_epoch", "", pool["epoch"].asDouble());
    if (pool["block"].asInt() >= 0)
    {
        m.family("hashwarp_block_height", "gauge", "Height of the block being mined");
        m.sample("hashwarp_block_height", "", pool["block"].asDouble());
    }
    return m.str();
}

const char* const c_dashboardHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hashwarp</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1b1f24;--muted:#5d6670;--line:#e3e6ea;--accent:#2f6fde;--ok:#1a8a4a;--warn:#b76e00;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#0f1216;--card:#171b21;--text:#e6e9ed;--muted:#97a0aa;--line:#262c34;--accent:#6ea2ff;--ok:#43c37a;--warn:#e8a33d;--bad:#ef5350}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:baseline;padding:16px;border-bottom:1px solid var(--line);background:var(--card)}
h1{font-size:18px;margin:0}
.muted{color:var(--muted)}
main{max-width:1100px;margin:0 auto;padding:16px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin-bottom:16px;align-items:start}
a{color:var(--accent)}
.tile{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}
.tile .v{font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}
.tile .l{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
svg.spark{width:100%;height:28px;display:block;margin-top:4px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;overflow-x:auto}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
th,td{padding:8px 10px;text-align:right;white-space:nowrap;border-bottom:1px solid var(--line)}
th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){text-align:left}
th{color:var(--muted);font-weight:500;font-size:12px}
tr:last-child td{border-bottom:none}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--ok)}
.dot.off{background:var(--bad)}
.paused{color:var(--warn)}
button{font:inherit;border:1px solid var(--line);background:var(--bg);color:var(--text);border-radius:6px;padding:3px 10px;cursor:pointer}
button:hover{border-color:var(--accent)}
#error{color:var(--bad);margin:0 0 12px}
footer{padding:16px;text-align:center;color:var(--muted);font-size:12px}
</style>
</head>
<body>
<header><h1>Hashwarp</h1><span id="version" class="muted"></span><span id="pool"></span><span id="uptime" class="muted"></span><span id="actions"></span></header>
<main>
<p id="error" hidden></p>
<section class="tiles">
<div class="tile"><div class="l">Hashrate</div><div class="v" id="hr">-</div><svg class="spark" viewBox="0 0 120 40" preserveAspectRatio="none"><polyline id="spark" fill="none" stroke="var(--accent)" stroke-width="1.5" points=""/></svg></div>
<div class="tile"><div class="l">15 min average</div><div class="v" id="hr15">-</div></div>
<div class="tile"><div class="l">Effective (pool)</div><div class="v" id="eff">-</div></div>
<div class="tile"><div class="l">Shares</div><div class="v" id="shares">-</div></div>
</section>
<div class="card"><table><thead><tr><th>GPU</th><th>Name</th><th>Hashrate</th><th>15m</th><th>Shares</th><th>Temp</th><th>Mem</th><th>Fan</th><th>Power</th><th>kh/W</th><th></th></tr></thead><tbody id="gpus"></tbody></table></div>
</main>
<footer>Updates every 5 seconds &middot; <a href="/api/v1/summary">JSON</a> &middot; <a href="/metrics">Prometheus</a></footer>
<script>
"use strict";
const history = [];
let token = sessionStorage.getItem("hashwarp-token") || "";
function headers(){ return token ? {"Authorization":"Bearer " + token} : {}; }
function fmt(h){ const u=[["Th/s",1e12],["Gh/s",1e9],["Mh/s",1e6],["Kh/s",1e3]]; for (const [n,d] of u) if (h >= d) return (h/d).toFixed(2)+" "+n; return h.toFixed(0)+" h/s"; }
function temp(c){ return c > 0 ? c + "°C" : "–"; }
function uptime(s){ const d=Math.floor(s/86400), h=Math.floor(s%86400/3600), m=Math.floor(s%3600/60); return "up " + (d ? d+"d " : "") + h + ":" + String(m).padStart(2,"0"); }
function td(text, cls){ const e=document.createElement("td"); e.textContent=text; if (cls) e.className=cls; return e; }
async function control(action, gpu){
  const body = {action}; if (gpu !== undefined) body.gpu = gpu;
  const r = await fetch("/api/v1/control", {method:"POST", headers:Object.assign({"Content-Type":"application/json"}, headers()), body:JSON.stringify(body)});
  if (!r.ok) showError("Control failed: " + r.status + " " + await r.text()); else refresh();
}
function showError(msg){ const e=document.getElementById("error"); e.textContent=msg; e.hidden=!msg; }
function render(s){
  document.getElementById("version").textContent = "v" + s.miner.version;
  const pool = document.getElementById("pool");
  pool.innerHTML = ""; const dot=document.createElement("span"); dot.className="dot"+(s.pool.connected?"":" off"); pool.append(dot, document.createTextNode((s.pool.connected?"":"not connected · ") + s.pool.host + " · epoch " + s.pool.epoch));
  document.getElementById("uptime").textContent = uptime(s.miner.uptime);
  document.getElementById("hr").textContent = fmt(s.hashrate.now);
  document.getElementById("hr15").textContent = fmt(s.hashrate["15m"]);
  document.getElementById("eff").textContent = s.hashrate.effective > 0 ? fmt(s.hashrate.effective) : "–";
  document.getElementById("shares").textContent = s.shares.accepted + (s.shares.rejected ? " / " + s.shares.rejected + " rej" : "");
  history.push(s.hashrate.now); if (history.length > 120) history.shift();
  const max = Math.max(...history, 1);
  document.getElementById("spark").setAttribute("points", history.map((v,i) => (i*120/Math.max(history.length-1,1)).toFixed(1)+","+(38-v/max*34).toFixed(1)).join(" "));
  const actions = document.getElementById("actions"); actions.innerHTML = "";
  if (s.control){ const b=document.createElement("button"); b.textContent="Restart"; b.onclick=()=>control("restart"); actions.append(b); }
  const body = document.getElementById("gpus"); body.innerHTML = "";
  for (const g of s.gpus){
    const tr = document.createElement("tr");
    tr.append(td(g.backend + g.index), td(g.name));
    if (g.paused){ const c=td("paused" + (g.pause_reason ? " (" + g.pause_reason + ")" : ""), "paused"); c.colSpan=2; tr.append(c); }
    else tr.append(td(fmt(g.hashrate.now)), td(fmt(g.hashrate["15m"])));
    tr.append(td(g.shares.accepted + (g.shares.rejected ? "/" + g.shares.rejected : "")), td(temp(g.temperature.core)), td(temp(g.temperature.memory || g.temperature.hotspot)),
      td(g.temperature.core > 0 ? g.fan + "%" : "–"), td(g.power > 0 ? g.power.toFixed(0) + " W" : "–"), td(g.power > 0 ? (g.hashrate.now/1e3/g.power).toFixed(0) : "–"));
    const act = document.createElement("td");
    if (s.control){ const b=document.createElement("button"); b.textContent = g.paused ? "Resume" : "Pause"; b.onclick=()=>control(g.paused ? "resume" : "pause", g.index); act.append(b); }
    tr.append(act); body.append(tr);
  }
}
async function refresh(){
  try {
    const r = await fetch("/api/v1/summary", {headers:headers()});
    if (r.status === 401){ token = prompt("API token") || ""; sessionStorage.setItem("hashwarp-token", token); return; }
    if (!r.ok) throw new Error(r.status + " " + r.statusText);
    render(await r.json()); showError("");
  } catch (e) { showError("Can't reach hashwarp: " + e.message); }
}
refresh(); setInterval(refresh, 5000);
</script>
</body>
</html>
)HTML";

}  // namespace api
}  // namespace hashwarp
