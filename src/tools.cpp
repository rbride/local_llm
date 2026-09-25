#include "tools.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cmath>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>

#include "http.hpp"
#include "util.hpp"

namespace {

const char* BROWSER_UA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36";
const char* API_UA = "tgbot/2.0 (personal Telegram bot; https://github.com/)";

json err(const std::string& msg) { return {{"error", msg}}; }

std::mt19937_64& rng() {
    static thread_local std::mt19937_64 g{std::random_device{}()};
    return g;
}

json fn(const std::string& name, const std::string& desc, json props, json required) {
    return {{"type", "function"},
            {"function",
             {{"name", name},
              {"description", desc},
              {"parameters", {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}}}}}};
}

HttpResponse get(const std::string& url, const std::string& ua = API_UA, std::vector<std::string> headers = {}) {
    HttpOptions o;
    o.url = url;
    o.timeout = 20;
    o.max_bytes = 3 * 1024 * 1024;
    o.compressed = true;
    o.follow_redirects = true;  // only used for fixed, trusted API hosts
    o.user_agent = ua;
    o.headers = std::move(headers);
    return http_request(o);
}

json get_json(const std::string& url, std::vector<std::string> headers = {}) {
    HttpResponse r = get(url, API_UA, std::move(headers));
    if (!r.error.empty()) throw std::runtime_error("request failed: " + r.error);
    if (r.status != 200) throw std::runtime_error("HTTP " + std::to_string(r.status));
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("bad JSON from API");
    return j;
}

// ---------------------------------------------------------------- address safety

bool v4_public(uint32_t a) {
    auto in = [a](uint32_t net, int bits) {
        uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
        return (a & mask) == (net & mask);
    };
    return !(in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) || in(0x7F000000, 8) ||
             in(0xA9FE0000, 16) || in(0xAC100000, 12) || in(0xC0000000, 24) || in(0xC0000200, 24) ||
             in(0xC0A80000, 16) || in(0xC6120000, 15) || in(0xC6336400, 24) || in(0xCB007100, 24) ||
             in(0xE0000000, 4) || in(0xF0000000, 4));
}

bool v6_public(const unsigned char* b) {
    static const unsigned char zero[16] = {};
    if (std::memcmp(b, zero, 10) == 0 && b[10] == 0xff && b[11] == 0xff)
        return v4_public((uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15]);
    if (std::memcmp(b, zero, 12) == 0) return false;               // ::, ::1, v4-compatible
    if ((b[0] & 0xfe) == 0xfc) return false;                       // fc00::/7 unique local
    if (b[0] == 0xfe && (b[1] & 0x80) == 0x80) return false;       // fe80::/10, fec0::/10
    if (b[0] == 0xff) return false;                                // multicast
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return false;  // NAT64
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // documentation
    if (b[0] == 0x20 && b[1] == 0x02)                                                 // 6to4
        return v4_public((uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) | (uint32_t(b[4]) << 8) | b[5]);
    return true;
}

// ---------------------------------------------------------------- calculator

class Calc {
public:
    explicit Calc(const std::string& s) : s_(s) {}
    double run() {
        double v = expr();
        skip();
        if (i_ != s_.size()) fail("unexpected '" + std::string(1, s_[i_]) + "'");
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int depth_ = 0;

    [[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }
    void skip() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    bool eat(char c) {
        skip();
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }
    struct Guard {
        int& d;
        explicit Guard(int& d_) : d(d_) { if (++d > 60) throw std::runtime_error("expression too deeply nested"); }
        ~Guard() { --d; }
    };

    double expr() {
        Guard g(depth_);
        double v = term();
        for (;;) {
            if (eat('+')) v += term();
            else if (eat('-')) v -= term();
            else return v;
        }
    }
    double term() {
        double v = unary();
        for (;;) {
            skip();
            if (eat('*')) v *= unary();
            else if (eat('/')) {
                double d = unary();
                if (d == 0) fail("division by zero");
                v /= d;
            } else if (eat('%')) {
                double d = unary();
                if (d == 0) fail("modulo by zero");
                v = std::fmod(v, d);
            } else return v;
        }
    }
    double unary() {
        Guard g(depth_);
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return power();
    }
    double power() {
        double base = postfix();
        skip();
        if (eat('^')) return std::pow(base, unary());
        if (i_ + 1 < s_.size() && s_[i_] == '*' && s_[i_ + 1] == '*') {
            i_ += 2;
            return std::pow(base, unary());
        }
        return base;
    }
    double postfix() {
        double v = primary();
        while (eat('!')) {
            if (v < 0 || v > 170 || v != std::floor(v)) fail("factorial needs a whole number from 0 to 170");
            double f = 1;
            for (int k = 2; k <= static_cast<int>(v); ++k) f *= k;
            v = f;
        }
        return v;
    }
    double primary() {
        skip();
        if (i_ >= s_.size()) fail("unexpected end of expression");
        if (eat('(')) {
            double v = expr();
            if (!eat(')')) fail("missing ')'");
            return v;
        }
        char c = s_[i_];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t used = 0;
            std::string rest = s_.substr(i_);
            double v;
            try { v = std::stod(rest, &used); } catch (...) { fail("bad number"); }
            i_ += used;
            return v;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) {
            size_t st = i_;
            while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '_')) ++i_;
            std::string name = lower(s_.substr(st, i_ - st));
            if (name == "pi") return M_PI;
            if (name == "e") return M_E;
            if (name == "tau") return 2 * M_PI;
            if (!eat('(')) fail("unknown name '" + name + "'");
            std::vector<double> args;
            if (!eat(')')) {
                do args.push_back(expr());
                while (eat(','));
                if (!eat(')')) fail("missing ')' after arguments");
            }
            return call(name, args);
        }
        fail("unexpected '" + std::string(1, c) + "'");
    }
    double call(const std::string& n, const std::vector<double>& a) {
        auto need = [&](size_t k) { if (a.size() != k) fail(n + "() takes " + std::to_string(k) + " argument(s)"); };
        using F = double (*)(double);
        static const std::pair<const char*, F> one[] = {
            {"sqrt", [](double x) { return std::sqrt(x); }}, {"cbrt", [](double x) { return std::cbrt(x); }},
            {"abs", [](double x) { return std::fabs(x); }},  {"sin", [](double x) { return std::sin(x); }},
            {"cos", [](double x) { return std::cos(x); }},   {"tan", [](double x) { return std::tan(x); }},
            {"asin", [](double x) { return std::asin(x); }}, {"acos", [](double x) { return std::acos(x); }},
            {"atan", [](double x) { return std::atan(x); }}, {"sinh", [](double x) { return std::sinh(x); }},
            {"cosh", [](double x) { return std::cosh(x); }}, {"tanh", [](double x) { return std::tanh(x); }},
            {"ln", [](double x) { return std::log(x); }},    {"log", [](double x) { return std::log10(x); }},
            {"log10", [](double x) { return std::log10(x); }}, {"log2", [](double x) { return std::log2(x); }},
            {"exp", [](double x) { return std::exp(x); }},   {"floor", [](double x) { return std::floor(x); }},
            {"ceil", [](double x) { return std::ceil(x); }}, {"round", [](double x) { return std::round(x); }},
            {"deg", [](double x) { return x * 180 / M_PI; }}, {"rad", [](double x) { return x * M_PI / 180; }}};
        for (const auto& [name, f] : one)
            if (n == name) { need(1); return f(a[0]); }
        if (n == "pow") { need(2); return std::pow(a[0], a[1]); }
        if (n == "hypot") { need(2); return std::hypot(a[0], a[1]); }
        if (n == "atan2") { need(2); return std::atan2(a[0], a[1]); }
        if (n == "min" || n == "max") {
            if (a.empty()) fail(n + "() needs arguments");
            double v = a[0];
            for (double x : a) v = n == "min" ? std::min(v, x) : std::max(v, x);
            return v;
        }
        fail("unknown function '" + n + "'");
    }
};

std::string fmt_number(double v) {
    if (std::isnan(v)) return "NaN (undefined)";
    if (std::isinf(v)) return v > 0 ? "infinity" : "-infinity";
    char buf[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

// ---------------------------------------------------------------- search backends

std::string attr(const std::string& tag, const std::string& name) {
    auto p = tag.find(name + "=\"");
    if (p == std::string::npos) return "";
    p += name.size() + 2;
    auto e = tag.find('"', p);
    return e == std::string::npos ? "" : tag.substr(p, e - p);
}

const char* weather_desc(int code) {
    switch (code) {
        case 0: return "clear sky";
        case 1: return "mainly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45: case 48: return "fog";
        case 51: case 53: case 55: return "drizzle";
        case 56: case 57: return "freezing drizzle";
        case 61: return "light rain";
        case 63: return "rain";
        case 65: return "heavy rain";
        case 66: case 67: return "freezing rain";
        case 71: return "light snow";
        case 73: return "snow";
        case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: case 81: return "rain showers";
        case 82: return "violent rain showers";
        case 85: case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96: case 99: return "thunderstorm with hail";
        default: return "unknown";
    }
}

std::string arg_str(const json& a, const char* key) {
    if (!a.is_object() || !a.contains(key)) return "";
    if (a[key].is_string()) return trim(a[key].get<std::string>());
    if (a[key].is_number()) return a[key].dump();
    return "";
}

}  // namespace

json parse_duckduckgo(const std::string& html, int n) {
    json out = json::array();
    size_t pos = 0;
    while (static_cast<int>(out.size()) < n) {
        auto a = html.find("result__a", pos);
        if (a == std::string::npos) break;
        auto tag_start = html.rfind("<a", a);
        auto tag_end = html.find('>', a);
        if (tag_start == std::string::npos || tag_end == std::string::npos) break;
        std::string href = html_decode_entities(attr(html.substr(tag_start, tag_end - tag_start), "href"));
        auto close = html.find("</a>", tag_end);
        if (close == std::string::npos) break;
        std::string title = strip_tags(html.substr(tag_end + 1, close - tag_end - 1));
        pos = close;

        std::string snippet;
        auto next = html.find("result__a", pos);
        auto s = html.find("result__snippet", pos);
        if (s != std::string::npos && (next == std::string::npos || s < next)) {
            auto st = html.find('>', s);
            auto e1 = html.find("</a>", st), e2 = html.find("</div>", st), e3 = html.find("</td>", st);
            auto se = std::min({e1, e2, e3});
            if (st != std::string::npos && se != std::string::npos) snippet = strip_tags(html.substr(st + 1, se - st - 1));
        }
        if (href.find("duckduckgo.com/y.js") != std::string::npos) continue;  // ad
        auto u = href.find("uddg=");
        if (u != std::string::npos) {
            auto e = href.find('&', u);
            href = url_decode(href.substr(u + 5, e == std::string::npos ? std::string::npos : e - u - 5));
        }
        if (starts_with(href, "//")) href = "https:" + href;
        if (!starts_with(href, "http") || title.empty()) continue;
        out.push_back({{"title", title}, {"url", href}, {"snippet", snippet}});
    }
    return out;
}

// ================================================================ public

json Tools::definitions() const {
    json all = json::array();
    auto add = [&](const std::string& name, json def) { if (cfg_.tool_enabled(name)) all.push_back(std::move(def)); };

    add("web_search", fn("web_search",
        "Search the web. Use for current events, news, prices, facts you're unsure of, or anything after your training data.",
        {{"query", {{"type", "string"}, {"description", "search query"}}},
         {"count", {{"type", "integer"}, {"description", "number of results, 1-10"}}}},
        {"query"}));
    add("fetch_url", fn("fetch_url",
        "Download a web page and return its readable text. Use when someone shares a link or you need details from a search result.",
        {{"url", {{"type", "string"}, {"description", "http(s) URL"}}}}, {"url"}));
    add("wikipedia", fn("wikipedia",
        "Look up a topic on Wikipedia and return a summary of the best-matching article.",
        {{"query", {{"type", "string"}, {"description", "topic to look up"}}},
         {"lang", {{"type", "string"}, {"description", "language code, default en"}}}},
        {"query"}));
    add("weather", fn("weather", "Current weather and a 3-day forecast for a place.",
        {{"location", {{"type", "string"}, {"description", "city name, e.g. 'Paris' or 'Austin, Texas'"}}},
         {"units", {{"type", "string"}, {"enum", {"metric", "imperial"}}}}},
        {"location"}));
    add("calculator", fn("calculator",
        "Evaluate a math expression exactly. Supports + - * / % ^ ! parentheses, sqrt, sin, cos, tan, ln, log, log2, exp, "
        "abs, floor, ceil, round, min, max, pow, hypot, pi, e. Use this instead of doing arithmetic in your head.",
        {{"expression", {{"type", "string"}, {"description", "e.g. '(3.5*12)^2 / sqrt(7)'"}}}}, {"expression"}));
    add("roll_dice", fn("roll_dice", "Roll dice in tabletop notation, e.g. 'd20', '3d6+2', '2d8+1d4-1'.",
        {{"notation", {{"type", "string"}}}}, {"notation"}));
    add("random", fn("random",
        "Pick randomly: give 'options' to choose from a list (coin flips, who pays, etc.), or 'min'/'max' for a random integer.",
        {{"options", {{"type", "array"}, {"items", {{"type", "string"}}}}},
         {"count", {{"type", "integer"}, {"description", "how many to pick, default 1"}}},
         {"min", {{"type", "integer"}}},
         {"max", {{"type", "integer"}}}},
        json::array()));
    add("get_datetime", fn("get_datetime", "Get the current date, time and weekday.", json::object(), json::array()));
    add("remember", fn("remember",
        "Save a short fact about this chat or its people for later conversations (e.g. 'Ryan is allergic to cats'). "
        "Only save things people clearly want remembered.",
        {{"fact", {{"type", "string"}}}}, {"fact"}));
    add("forget", fn("forget", "Delete a saved note by its number from the notes list.",
        {{"number", {{"type", "integer"}}}}, {"number"}));
    return all;
}

std::string Tools::status_line(const std::string& name, const std::string& arguments) const {
    json a = json::parse(arguments, nullptr, false);
    auto q = [&](const char* k) { return utf8_head(arg_str(a, k), 80); };
    if (name == "web_search") return "\xF0\x9F\x94\x8E Searching: " + q("query");                 // 🔎
    if (name == "fetch_url") return "\xF0\x9F\x8C\x90 Reading: " + q("url");                     // 🌐
    if (name == "wikipedia") return "\xF0\x9F\x93\x9A Wikipedia: " + q("query");                 // 📚
    if (name == "weather") return "\xE2\x9B\x85 Weather: " + q("location");                      // ⛅
    if (name == "calculator") return "\xF0\x9F\xA7\xAE Calculating: " + q("expression");         // 🧮
    if (name == "roll_dice") return "\xF0\x9F\x8E\xB2 Rolling " + q("notation");                 // 🎲
    if (name == "random") return "\xF0\x9F\x8E\xB0 Picking at random";                           // 🎰
    if (name == "get_datetime") return "\xF0\x9F\x95\x92 Checking the time";                     // 🕒
    if (name == "remember") return "\xF0\x9F\x93\x9D Remembering: " + q("fact");                 // 📝
    if (name == "forget") return "\xF0\x9F\xA7\xB9 Forgetting note #" + q("number");             // 🧹
    return "\xF0\x9F\x94\xA7 " + name;                                                            // 🔧
}

std::string Tools::run(const std::string& name, const std::string& arguments, const ToolContext& ctx) const {
    if (!cfg_.tool_enabled(name)) return err("unknown or disabled tool: " + name).dump();
    json a = json::parse(arguments.empty() ? "{}" : arguments, nullptr, false);
    if (a.is_discarded() || !a.is_object()) return err("arguments must be a JSON object").dump();
    try {
        if (name == "web_search") return web_search(a);
        if (name == "fetch_url") return fetch_url(a);
        if (name == "wikipedia") return wikipedia(a);
        if (name == "weather") return weather(a);
        if (name == "calculator") return json{{"result", calculate(arg_str(a, "expression"))}}.dump();
        if (name == "roll_dice") return roll(arg_str(a, "notation"));
        if (name == "random") return random(a);
        if (name == "get_datetime") return datetime();
        if (name == "remember") return remember(a, ctx);
        if (name == "forget") return forget(a, ctx);
    } catch (const std::exception& e) {
        return err(e.what()).dump();
    }
    return err("unknown tool: " + name).dump();
}

// ---------------------------------------------------------------- individual tools

std::string Tools::calculate(const std::string& expr) {
    if (trim(expr).empty()) throw std::runtime_error("empty expression");
    if (expr.size() > 300) throw std::runtime_error("expression too long");
    std::string cleaned;
    for (size_t i = 0; i < expr.size(); ++i) {
        auto digit = [&](size_t k) { return k < expr.size() && std::isdigit(static_cast<unsigned char>(expr[k])); };
        // "1,000,000" -> "1000000", but keep commas that separate function arguments
        if (expr[i] == ',' && i > 0 && digit(i - 1) && digit(i + 1) && digit(i + 2) && digit(i + 3) && !digit(i + 4)) continue;
        if (expr.compare(i, 2, "\xC3\x97") == 0) { cleaned += '*'; ++i; continue; }  // multiplication sign
        if (expr.compare(i, 2, "\xC3\xB7") == 0) { cleaned += '/'; ++i; continue; }  // division sign
        cleaned += expr[i];
    }
    Calc c(cleaned);
    return fmt_number(c.run());
}

std::string Tools::roll(const std::string& notation) {
    std::string s;
    for (char ch : lower(notation)) if (!std::isspace(static_cast<unsigned char>(ch))) s += ch;
    if (s.empty()) throw std::runtime_error("empty dice notation");
    json parts = json::array();
    long long total = 0;
    int dice_used = 0;
    size_t i = 0;
    while (i < s.size()) {
        int sign = 1;
        if (s[i] == '+' || s[i] == '-') { sign = s[i] == '-' ? -1 : 1; ++i; }
        size_t st = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        long long count = st == i ? -1 : std::stoll(s.substr(st, i - st));
        if (i < s.size() && s[i] == 'd') {
            ++i;
            size_t fs = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (fs == i) throw std::runtime_error("missing number of sides after 'd'");
            long long sides = std::stoll(s.substr(fs, i - fs));
            if (count == -1) count = 1;
            if (count < 1 || sides < 2) throw std::runtime_error("need at least 1 die with 2+ sides");
            dice_used += static_cast<int>(count);
            if (dice_used > 200 || sides > 100000) throw std::runtime_error("too many dice or sides");
            std::uniform_int_distribution<long long> d(1, sides);
            json rolls = json::array();
            long long sum = 0;
            for (long long k = 0; k < count; ++k) {
                long long r = d(rng());
                rolls.push_back(r);
                sum += r;
            }
            total += sign * sum;
            parts.push_back({{"dice", (sign < 0 ? "-" : "") + std::to_string(count) + "d" + std::to_string(sides)},
                             {"rolls", rolls}, {"sum", sign * sum}});
        } else {
            if (count == -1) throw std::runtime_error("bad dice notation near '" + s.substr(st) + "'");
            total += sign * count;
            parts.push_back({{"modifier", sign * count}});
        }
    }
    return json{{"notation", notation}, {"parts", parts}, {"total", total}}.dump();
}

bool Tools::url_is_safe(const std::string& url, std::string& resolve_entry, std::string& error) {
    resolve_entry.clear();
    CURLU* h = curl_url();
    if (curl_url_set(h, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        error = "invalid URL";
        return false;
    }
    char *scheme = nullptr, *host = nullptr, *port = nullptr;
    curl_url_get(h, CURLUPART_SCHEME, &scheme, 0);
    curl_url_get(h, CURLUPART_HOST, &host, 0);
    curl_url_get(h, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
    std::string sc = scheme ? lower(scheme) : "", hs = host ? host : "", pt = port ? port : "";
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_url_cleanup(h);

    if (sc != "http" && sc != "https") { error = "only http and https links are allowed"; return false; }
    if (hs.empty()) { error = "URL has no host"; return false; }
    std::string bare = hs;
    if (bare.front() == '[' && bare.back() == ']') bare = bare.substr(1, bare.size() - 2);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(bare.c_str(), pt.c_str(), &hints, &res) != 0 || !res) {
        error = "couldn't resolve host " + bare;
        return false;
    }
    std::string first_ip;
    bool ok = true;
    for (addrinfo* p = res; p; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {};
        bool pub = false;
        if (p->ai_family == AF_INET) {
            auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            pub = v4_public(ntohl(sa->sin_addr.s_addr));
            inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf);
            if (first_ip.empty()) first_ip = buf;
        } else if (p->ai_family == AF_INET6) {
            auto* sa = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            pub = v6_public(sa->sin6_addr.s6_addr);
            inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof buf);
            if (first_ip.empty()) first_ip = std::string("[") + buf + "]";
        }
        if (!pub) { ok = false; break; }
    }
    freeaddrinfo(res);
    if (!ok) { error = "that address points to a private or local network, which is blocked"; return false; }

    in6_addr tmp6;
    in_addr tmp4;
    bool literal = inet_pton(AF_INET, bare.c_str(), &tmp4) == 1 || inet_pton(AF_INET6, bare.c_str(), &tmp6) == 1;
    // Pin the address we just checked, so a second DNS lookup can't swap in a private one.
    if (!literal && !first_ip.empty()) resolve_entry = hs + ":" + pt + ":" + first_ip;
    return true;
}

std::string Tools::fetch_url(const json& a) const {
    std::string url = arg_str(a, "url");
    if (url.empty()) return err("missing url").dump();
    if (url.find("://") == std::string::npos) url = "https://" + url;

    HttpResponse r;
    for (int hop = 0; hop < 6; ++hop) {
        std::string resolve, why;
        if (!url_is_safe(url, resolve, why)) return err(why).dump();
        HttpOptions o;
        o.url = url;
        o.resolve = resolve;
        o.timeout = 20;
        o.max_bytes = 3 * 1024 * 1024;
        o.compressed = true;
        o.user_agent = BROWSER_UA;
        o.headers = {"Accept: text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.5",
                     "Accept-Language: en-US,en;q=0.8"};
        r = http_request(o);
        if (!r.error.empty()) return err("couldn't download: " + r.error).dump();
        if (r.status >= 300 && r.status < 400 && !r.redirect_url.empty()) { url = r.redirect_url; continue; }
        break;
    }
    if (r.status >= 300 && r.status < 400) return err("too many redirects").dump();

    std::string ct = lower(r.content_type), title, text;
    if (ct.empty() || ct.find("html") != std::string::npos || ct.find("xml") != std::string::npos) text = html_to_text(r.body, &title);
    else if (starts_with(ct, "text/") || ct.find("json") != std::string::npos) text = r.body;
    else return err("can't read this type of file (" + ct + ")").dump();

    bool truncated = r.truncated || text.size() > cfg_.fetch_max_chars;
    text = utf8_head(text, cfg_.fetch_max_chars);
    return json{{"url", url}, {"status", r.status}, {"title", title}, {"content", text}, {"truncated", truncated}}
        .dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string Tools::web_search(const json& a) const {
    std::string q = arg_str(a, "query");
    if (q.empty()) return err("missing query").dump();
    int n = cfg_.search_results;
    if (a.contains("count") && a["count"].is_number_integer()) n = a["count"].get<int>();
    n = std::max(1, std::min(n, 10));

    json results = json::array();
    std::string engine;
    if (!cfg_.searxng_url.empty()) {
        engine = "searxng";
        json j = get_json(cfg_.searxng_url + "/search?format=json&q=" + url_encode(q));
        if (j.contains("results") && j["results"].is_array())
            for (const auto& x : j["results"]) {
                if (static_cast<int>(results.size()) >= n) break;
                results.push_back({{"title", x.value("title", "")}, {"url", x.value("url", "")}, {"snippet", x.value("content", "")}});
            }
    } else if (!cfg_.brave_api_key.empty()) {
        engine = "brave";
        json j = get_json("https://api.search.brave.com/res/v1/web/search?count=" + std::to_string(n) + "&q=" + url_encode(q),
                          {"Accept: application/json", "X-Subscription-Token: " + cfg_.brave_api_key});
        if (j.contains("web") && j["web"].contains("results") && j["web"]["results"].is_array())
            for (const auto& x : j["web"]["results"])
                results.push_back({{"title", strip_tags(x.value("title", ""))}, {"url", x.value("url", "")},
                                   {"snippet", strip_tags(x.value("description", ""))}});
    } else {
        engine = "duckduckgo";
        HttpOptions o;
        o.method = "POST";
        o.url = "https://html.duckduckgo.com/html/";
        o.body = "q=" + url_encode(q) + "&kl=us-en";
        o.headers = {"Content-Type: application/x-www-form-urlencoded", "Accept-Language: en-US,en;q=0.8"};
        o.timeout = 20;
        o.compressed = true;
        o.user_agent = BROWSER_UA;
        HttpResponse r = http_request(o);
        if (!r.error.empty()) return err("search failed: " + r.error).dump();
        results = parse_duckduckgo(r.body, n);
        if (results.empty())
            return err("no results (DuckDuckGo may be rate-limiting this bot; set SEARXNG_URL or BRAVE_API_KEY for reliable search)").dump();
    }
    return json{{"query", q}, {"engine", engine}, {"results", results}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string Tools::wikipedia(const json& a) const {
    std::string q = arg_str(a, "query"), lang = lower(arg_str(a, "lang"));
    if (q.empty()) return err("missing query").dump();
    if (lang.empty()) lang = "en";
    for (char c : lang) if (!(std::islower(static_cast<unsigned char>(c)) || c == '-')) return err("bad language code").dump();
    if (lang.size() > 12) return err("bad language code").dump();
    std::string base = "https://" + lang + ".wikipedia.org";

    json s = get_json(base + "/w/api.php?action=query&list=search&format=json&srlimit=4&srsearch=" + url_encode(q));
    if (!s.contains("query") || !s["query"].contains("search") || s["query"]["search"].empty())
        return err("no Wikipedia article found for '" + q + "'").dump();
    std::string title = s["query"]["search"][0].value("title", "");
    json others = json::array();
    for (size_t k = 1; k < s["query"]["search"].size(); ++k) others.push_back(s["query"]["search"][k].value("title", ""));

    std::string path = title;
    for (auto& c : path) if (c == ' ') c = '_';
    json sum = get_json(base + "/api/rest_v1/page/summary/" + url_encode(path));
    std::string url;
    if (sum.contains("content_urls") && sum["content_urls"].contains("desktop"))
        url = sum["content_urls"]["desktop"].value("page", "");
    return json{{"title", sum.value("title", title)},
                {"description", sum.value("description", "")},
                {"summary", sum.value("extract", "")},
                {"url", url},
                {"other_matches", others}}
        .dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string Tools::weather(const json& a) const {
    std::string loc = arg_str(a, "location"), units = lower(arg_str(a, "units"));
    if (loc.empty()) return err("missing location").dump();
    if (units != "metric" && units != "imperial") units = cfg_.weather_units == "imperial" ? "imperial" : "metric";

    auto geocode = [](const std::string& name) {
        return get_json("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + url_encode(name));
    };
    json g = geocode(loc);
    if ((!g.contains("results") || g["results"].empty()) && loc.find(',') != std::string::npos)
        g = geocode(trim(loc.substr(0, loc.find(','))));  // "Austin, Texas" -> "Austin"
    if (!g.contains("results") || g["results"].empty()) return err("couldn't find a place called '" + loc + "'").dump();
    const auto& p = g["results"][0];
    double lat = p.value("latitude", 0.0), lon = p.value("longitude", 0.0);

    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + std::to_string(lat) + "&longitude=" + std::to_string(lon) +
                      "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,precipitation"
                      "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                      "&timezone=auto&forecast_days=3";
    if (units == "imperial") url += "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch";
    json w = get_json(url);

    json out = {{"place", p.value("name", "") + (p.contains("admin1") ? ", " + p.value("admin1", "") : "") + ", " + p.value("country", "")},
                {"units", units == "imperial" ? "°F, mph, inches" : "°C, km/h, mm"}};
    if (w.contains("current")) {
        const auto& c = w["current"];
        out["now"] = {{"temperature", c.value("temperature_2m", 0.0)},
                      {"feels_like", c.value("apparent_temperature", 0.0)},
                      {"humidity_percent", c.value("relative_humidity_2m", 0)},
                      {"wind", c.value("wind_speed_10m", 0.0)},
                      {"precipitation", c.value("precipitation", 0.0)},
                      {"conditions", weather_desc(c.value("weather_code", -1))},
                      {"local_time", c.value("time", "")}};
    }
    if (w.contains("daily") && w["daily"].contains("time")) {
        const auto& d = w["daily"];
        json days = json::array();
        for (size_t k = 0; k < d["time"].size(); ++k) {
            auto at = [&](const char* key) { return d.contains(key) && d[key].size() > k ? d[key][k] : json(); };
            days.push_back({{"date", at("time")},
                            {"high", at("temperature_2m_max")},
                            {"low", at("temperature_2m_min")},
                            {"rain_chance_percent", at("precipitation_probability_max")},
                            {"conditions", weather_desc(at("weather_code").is_number() ? at("weather_code").get<int>() : -1)}});
        }
        out["forecast"] = days;
    }
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string Tools::random(const json& a) const {
    if (a.contains("options") && a["options"].is_array() && !a["options"].empty()) {
        std::vector<std::string> opts;
        for (const auto& o : a["options"]) opts.push_back(o.is_string() ? o.get<std::string>() : o.dump());
        int count = a.contains("count") && a["count"].is_number_integer() ? a["count"].get<int>() : 1;
        count = std::max(1, std::min<int>(count, static_cast<int>(opts.size())));
        std::shuffle(opts.begin(), opts.end(), rng());
        opts.resize(count);
        return json{{"picked", opts}}.dump();
    }
    long long lo = a.contains("min") && a["min"].is_number() ? a["min"].get<long long>() : 1;
    long long hi = a.contains("max") && a["max"].is_number() ? a["max"].get<long long>() : 100;
    if (lo > hi) std::swap(lo, hi);
    std::uniform_int_distribution<long long> d(lo, hi);
    return json{{"number", d(rng())}, {"min", lo}, {"max", hi}}.dump();
}

std::string Tools::datetime() const {
    std::time_t t = std::time(nullptr);
    return json{{"local", format_time(t, false, "%A, %B %d, %Y %H:%M:%S %Z (UTC%z)")},
                {"utc", format_time(t, true, "%Y-%m-%d %H:%M:%S UTC")},
                {"unix", static_cast<long long>(t)}}
        .dump();
}

std::string Tools::remember(const json& a, const ToolContext& ctx) const {
    std::string fact = utf8_head(sanitize_untrusted(arg_str(a, "fact")), 300);
    if (fact.empty()) return err("missing fact").dump();
    std::lock_guard<std::mutex> lock(store_.mu);
    auto& mems = store_.chats[ctx.chat_id].memories;
    if (mems.size() >= cfg_.memory_max) return err("notes are full (" + std::to_string(cfg_.memory_max) + "); forget one first").dump();
    for (const auto& m : mems) if (lower(m.text) == lower(fact)) return json{{"ok", true}, {"note", "already saved"}}.dump();
    mems.push_back({fact, ctx.user_name, format_time(std::time(nullptr), false, "%Y-%m-%d")});
    store_.save();
    return json{{"ok", true}, {"saved_as_number", mems.size()}}.dump();
}

std::string Tools::forget(const json& a, const ToolContext& ctx) const {
    if (!a.contains("number") || !a["number"].is_number_integer()) return err("missing note number").dump();
    int n = a["number"].get<int>();
    std::lock_guard<std::mutex> lock(store_.mu);
    auto& mems = store_.chats[ctx.chat_id].memories;
    if (n < 1 || n > static_cast<int>(mems.size())) return err("no note #" + std::to_string(n)).dump();
    std::string removed = mems[n - 1].text;
    mems.erase(mems.begin() + (n - 1));
    store_.save();
    return json{{"ok", true}, {"removed", removed}}.dump();
}
