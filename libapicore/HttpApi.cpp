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
        if (cl->second.empty() || cl->second.size() > 9 ||
            cl->second.find_first_not_of("0123456789") != std::string::npos)
            return ParseResult::Invalid;
        bodyLength = std::stoul(cl->second);
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

std::string metricsFromRigs(const Json::Value& _rigs)
{
    Metrics m;
    m.family("hashwarp_rig_up", "gauge", "Whether a rig's API answers");
    for (const auto& r : _rigs)
        m.sample("hashwarp_rig_up", "rig=\"" + escapeLabel(r["name"].asString()) + "\"",
            r["online"].asBool() ? 1 : 0);
    m.family("hashwarp_rig_hashrate", "gauge", "Hashes per second of a rig");
    m.family("hashwarp_rig_shares_total", "counter", "Shares found by a rig, by result");
    m.family("hashwarp_rig_power_watts", "gauge", "Power draw of a rig's GPUs");
    for (const auto& r : _rigs)
    {
        if (!r["online"].asBool())
            continue;
        const Json::Value& s = r["summary"];
        std::string rig = "rig=\"" + escapeLabel(r["name"].asString()) + "\"";
        m.sample("hashwarp_rig_hashrate", rig, s["hashrate"]["now"].asDouble());
        for (const char* result : {"accepted", "rejected", "failed"})
            m.sample("hashwarp_rig_shares_total", rig + ",result=\"" + result + "\"",
                s["shares"][result].asDouble());
        double power = 0;
        for (const auto& g : s["gpus"])
            power += g["power"].asDouble();
        if (power > 0)
            m.sample("hashwarp_rig_power_watts", rig, power);
    }
    return m.str();
}

const char* const c_dashboardHtml =
    R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hashwarp</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1b1f24;--muted:#5d6670;--line:#e3e6ea;--accent:#2f6fde;--ok:#1a8a4a;--warn:#b76e00;--bad:#c62828;--hover:#eef2f8}
@media (prefers-color-scheme:dark){:root{--bg:#0f1216;--card:#171b21;--text:#e6e9ed;--muted:#97a0aa;--line:#262c34;--accent:#6ea2ff;--ok:#43c37a;--warn:#e8a33d;--bad:#ef5350;--hover:#1e242c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:baseline;padding:16px;border-bottom:1px solid var(--line);background:var(--card)}
h1{font-size:18px;margin:0}
h2{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:20px 0 8px}
.muted{color:var(--muted)}
main{max-width:1200px;margin:0 auto;padding:16px}
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
tr.rig{cursor:pointer}
tr.rig:hover{background:var(--hover)}
tr.total td{font-weight:600}
tr.sub td{background:var(--bg);font-size:13px}
tr.sub td:first-child{padding-left:24px}
td.left{text-align:left}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--ok)}
.dot.off{background:var(--bad)}
.dot.warn{background:var(--warn)}
.paused,.alert{color:var(--warn)}
.down{color:var(--bad)}
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
<section id="rigs" hidden>
<section class="tiles">
<div class="tile"><div class="l">Rigs online</div><div class="v" id="rigsOnline">-</div></div>
<div class="tile"><div class="l">Total hashrate</div><div class="v" id="rigsHr">-</div><svg class="spark" viewBox="0 0 120 40" preserveAspectRatio="none"><polyline id="rigsSpark" fill="none" stroke="var(--accent)" stroke-width="1.5" points=""/></svg></div>
<div class="tile"><div class="l">Total power</div><div class="v" id="rigsPower">-</div></div>
<div class="tile"><div class="l">Shares</div><div class="v" id="rigsShares">-</div></div>
</section>
<div class="card"><table><thead><tr><th>Rig</th><th>Status</th><th>Hashrate</th><th>15m</th><th>Shares</th><th>GPUs</th><th>Hottest</th><th>Power</th><th>kH/J</th><th>Pool</th><th>Uptime</th></tr></thead><tbody id="rigRows"></tbody></table></div>
</section>
<section id="local" hidden>
<h2 id="localTitle" hidden>This rig</h2>
<section class="tiles">
<div class="tile"><div class="l">Hashrate</div><div class="v" id="hr">-</div><svg class="spark" viewBox="0 0 120 40" preserveAspectRatio="none"><polyline id="spark" fill="none" stroke="var(--accent)" stroke-width="1.5" points=""/></svg></div>
<div class="tile"><div class="l">15 min average</div><div class="v" id="hr15">-</div></div>
<div class="tile"><div class="l">Effective (pool)</div><div class="v" id="eff">-</div></div>
<div class="tile"><div class="l">Shares</div><div class="v" id="shares">-</div></div>
</section>
<div class="card"><table><thead><tr><th>GPU</th><th>Name</th><th>Hashrate</th><th>15m</th><th>Shares</th><th>Temp</th><th>Mem</th><th>Fan</th><th>Power</th><th>kH/J</th><th></th></tr></thead><tbody id="gpus"></tbody></table></div>
</section>
</main>
<footer>Updates every 5 seconds &middot; <a href="/api/v1/rigs">JSON</a> &middot; <a href="/metrics">Prometheus</a></footer>
)HTML"
    // Two literals: MSVC limits each to 16 kB
    R"HTML(<script>
"use strict";
const HOT = 85;  // °C: a rig's alert
const history = [], rigsHistory = [], expanded = new Set();
let token = sessionStorage.getItem("hashwarp-token") || "";
function headers(){ return token ? {"Authorization":"Bearer " + token} : {}; }
function fmt(h){ const u=[["TH/s",1e12],["GH/s",1e9],["MH/s",1e6],["kH/s",1e3]]; for (const [n,d] of u) if (h >= d*0.9995) return (h/d).toFixed(2)+" "+n; return h.toFixed(0)+" H/s"; }
function temp(c){ return c > 0 ? c + "°C" : "–"; }
function ago(s){ return s < 90 ? s + " s" : s < 5400 ? Math.round(s/60) + " min" : Math.round(s/3600) + " h"; }
function uptime(s){ const d=Math.floor(s/86400), h=Math.floor(s%86400/3600), m=Math.floor(s%3600/60); return (d ? d+"d " : "") + h + ":" + String(m).padStart(2,"0"); }
function td(text, cls){ const e=document.createElement("td"); e.textContent=text; if (cls) e.className=cls; return e; }
function gpuName(g){ return (g.backend === "cp" ? "cpu" : "gpu") + g.index; }
function power(s){ return s.gpus.reduce((p,g) => p + (g.power > 0 ? g.power : 0), 0); }
function spark(id, hist, v){ hist.push(v); if (hist.length > 120) hist.shift(); const max = Math.max(...hist, 1);
  document.getElementById(id).setAttribute("points", hist.map((x,i) => (i*120/Math.max(hist.length-1,1)).toFixed(1)+","+(38-x/max*34).toFixed(1)).join(" ")); }
async function control(action, gpu){
  const body = {action}; if (gpu !== undefined) body.gpu = gpu;
  const r = await fetch("/api/v1/control", {method:"POST", headers:Object.assign({"Content-Type":"application/json"}, headers()), body:JSON.stringify(body)});
  if (!r.ok) showError("Control failed: " + r.status + " " + await r.text()); else refresh();
}
function showError(msg){ const e=document.getElementById("error"); e.textContent=msg; e.hidden=!msg; }

// What is wrong with a rig, empty if nothing
function alerts(r){
  if (!r.online) return [(r.error || "offline") + (r.age >= 0 ? " · seen " + ago(r.age) + " ago" : "")];
  const s = r.summary, a = [];
  if (!s.pool.connected) a.push("not connected");
  const paused = s.gpus.filter(g => g.paused).length; if (paused) a.push(paused + " paused");
  const hot = Math.max(0, ...s.gpus.map(g => g.temperature.core)); if (hot >= HOT) a.push("hot " + hot + "°C");
  if (s.shares.rejected + s.shares.failed > Math.max(2, s.shares.accepted * 0.05)) a.push("rejected shares");
  return a;
}

function renderRigs(rigs){
  const body = document.getElementById("rigRows"); body.innerHTML = "";
  let online = 0, hr = 0, hr15 = 0, watts = 0, acc = 0, rej = 0, gpus = 0;
  for (const r of rigs){
    const a = alerts(r), s = r.online ? r.summary : null;
    const tr = document.createElement("tr"); tr.className = "rig";
    const name = td(r.name + (r.local ? " (this rig)" : "")); name.title = r.local ? "" : r.host;
    const status = document.createElement("td"); status.className = "left";
    const dot = document.createElement("span"); dot.className = "dot" + (!r.online ? " off" : a.length ? " warn" : "");
    status.append(dot, document.createTextNode(a.length ? a.join(" · ") : "mining"));
    if (!r.online) status.classList.add("down"); else if (a.length) status.classList.add("alert");
    tr.append(name, status);
    if (s){
      online++; const w = power(s); hr += s.hashrate.now; hr15 += s.hashrate["15m"]; watts += w; acc += s.shares.accepted; rej += s.shares.rejected; gpus += s.gpus.length;
      const hot = Math.max(0, ...s.gpus.map(g => g.temperature.core));
      tr.append(td(fmt(s.hashrate.now)), td(fmt(s.hashrate["15m"])), td(s.shares.accepted + (s.shares.rejected ? "/" + s.shares.rejected : "")),
        td(s.gpus.length + (s.gpus.some(g => g.paused) ? " (" + s.gpus.filter(g => g.paused).length + " paused)" : "")),
        td(temp(hot), hot >= HOT ? "alert" : ""), td(w > 0 ? w.toFixed(0) + " W" : "–"), td(w > 0 ? (s.hashrate.now/1e3/w).toFixed(0) : "–"),
        td(s.pool.host, "left"), td(uptime(s.miner.uptime)));
    } else { const c = td("–"); c.colSpan = 9; c.className = "muted left"; tr.append(c); }
    tr.onclick = () => { if (expanded.has(r.name)) expanded.delete(r.name); else expanded.add(r.name); renderRigs(rigs); };
    body.append(tr);
    if (s && expanded.has(r.name)) for (const g of s.gpus){
      const sub = document.createElement("tr"); sub.className = "sub";
      sub.append(td(gpuName(g)), td(g.name, "left"));
      if (g.paused){ const c=td("paused" + (g.pause_reason ? " (" + g.pause_reason + ")" : ""), "paused"); c.colSpan=2; sub.append(c); }
      else sub.append(td(fmt(g.hashrate.now)), td(fmt(g.hashrate["15m"])));
      sub.append(td(g.shares.accepted + (g.shares.rejected ? "/" + g.shares.rejected : "")), td(g.temperature.core > 0 ? g.fan + "% fan" : "–"),
        td(temp(g.temperature.core), g.temperature.core >= HOT ? "alert" : ""), td(g.power > 0 ? g.power.toFixed(0) + " W" : "–"),
        td(g.power > 0 ? (g.hashrate.now/1e3/g.power).toFixed(0) : "–"), td(""), td(""));
      body.append(sub);
    }
  }
  if (rigs.length > 1){
    const tr = document.createElement("tr"); tr.className = "total";
    tr.append(td("Total"), td(online + "/" + rigs.length + " online", "left"), td(fmt(hr)), td(fmt(hr15)), td(acc + (rej ? "/" + rej : "")), td(String(gpus)),
      td(""), td(watts > 0 ? watts.toFixed(0) + " W" : "–"), td(watts > 0 ? (hr/1e3/watts).toFixed(0) : "–"), td(""), td(""));
    body.append(tr);
  }
  document.getElementById("rigsOnline").textContent = online + " / " + rigs.length;
  document.getElementById("rigsOnline").className = "v" + (online < rigs.length ? " down" : "");
  document.getElementById("rigsHr").textContent = fmt(hr);
  document.getElementById("rigsPower").textContent = watts > 0 ? watts.toFixed(0) + " W" : "–";
  document.getElementById("rigsShares").textContent = acc + (rej ? " / " + rej + " rej" : "");
  spark("rigsSpark", rigsHistory, hr);
}

function renderLocal(s){
  document.getElementById("version").textContent = "v" + s.miner.version;
  const pool = document.getElementById("pool");
  pool.innerHTML = ""; const dot=document.createElement("span"); dot.className="dot"+(s.pool.connected?"":" off"); pool.append(dot, document.createTextNode((s.pool.connected?"":"not connected · ") + s.pool.host + " · epoch " + s.pool.epoch));
  document.getElementById("uptime").textContent = "up " + uptime(s.miner.uptime);
  document.getElementById("hr").textContent = fmt(s.hashrate.now);
  document.getElementById("hr15").textContent = fmt(s.hashrate["15m"]);
  document.getElementById("eff").textContent = s.hashrate.effective > 0 ? fmt(s.hashrate.effective) : "–";
  document.getElementById("shares").textContent = s.shares.accepted + (s.shares.rejected ? " / " + s.shares.rejected + " rej" : "");
  spark("spark", history, s.hashrate.now);
  const actions = document.getElementById("actions"); actions.innerHTML = "";
  if (s.control){ const b=document.createElement("button"); b.textContent="Restart"; b.onclick=()=>control("restart"); actions.append(b); }
  const body = document.getElementById("gpus"); body.innerHTML = "";
  for (const g of s.gpus){
    const tr = document.createElement("tr");
    tr.append(td(gpuName(g)), td(g.name));
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
    const r = await fetch("/api/v1/rigs", {headers:headers()});
    if (r.status === 401){ token = prompt("API token") || ""; sessionStorage.setItem("hashwarp-token", token); return; }
    if (!r.ok) throw new Error(r.status + " " + r.statusText);
    const data = await r.json();
    const multi = !data.mining || data.rigs.length > 1;
    document.getElementById("rigs").hidden = !multi;
    document.getElementById("local").hidden = !data.mining;
    document.getElementById("localTitle").hidden = !multi;
    if (multi) renderRigs(data.rigs);
    if (data.mining) renderLocal(data.rigs[0].summary);
    else document.getElementById("version").textContent = "monitor";
    showError("");
  } catch (e) { showError("Can't reach hashwarp: " + e.message); }
}
refresh(); setInterval(refresh, 5000);
</script>
</body>
</html>
)HTML";

}  // namespace api
}  // namespace hashwarp
