#include "util.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <set>

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void log(const std::string& msg) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    std::cout << "[" << format_time(std::time(nullptr), false, "%H:%M:%S") << "] " << msg << std::endl;
}

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string format_time(std::time_t t, bool utc, const char* fmt) {
    std::tm tm{};
    if (utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
    char buf[128];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}

std::string strip_think(std::string s) {
    for (;;) {
        auto a = s.find("<think>");
        if (a == std::string::npos) break;
        auto b = s.find("</think>", a);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b + 8 - a);
    }
    auto b = s.find("</think>");
    if (b != std::string::npos) s.erase(0, b + 8);
    return trim(s);
}

static bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

std::string utf8_head(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && is_continuation(static_cast<unsigned char>(s[n]))) --n;
    return s.substr(0, n);
}

std::string utf8_tail(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t start = s.size() - max_bytes;
    while (start < s.size() && is_continuation(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

std::vector<std::string> split_for_telegram(const std::string& text, size_t limit) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t len = std::min(limit, text.size() - pos);
        if (pos + len < text.size()) {
            // Prefer a paragraph break, then a line break, then a space, in the back half.
            size_t cut = std::string::npos;
            for (const char* sep : {"\n\n", "\n", " "}) {
                size_t f = text.rfind(sep, pos + len);
                if (f != std::string::npos && f > pos + limit / 2) { cut = f + std::string(sep).size(); break; }
            }
            if (cut != std::string::npos) len = cut - pos;
            while (len > 0 && is_continuation(static_cast<unsigned char>(text[pos + len]))) --len;
            if (len == 0) len = std::min(limit, text.size() - pos);
        }
        std::string chunk = trim(text.substr(pos, len));
        if (!chunk.empty()) out.push_back(chunk);
        pos += len;
    }
    return out;
}

std::string sanitize_untrusted(std::string s) {
    // Generic "<|...|>" and DeepSeek-style "<｜...｜>" (fullwidth bar, EF BD 9C) control tokens.
    for (const auto& pair : {std::make_pair(std::string("<|"), std::string("|>")),
                             std::make_pair(std::string("<\xEF\xBD\x9C"), std::string("\xEF\xBD\x9C>"))}) {
        size_t pos = 0;
        while ((pos = s.find(pair.first, pos)) != std::string::npos) {
            size_t end = s.find(pair.second, pos + pair.first.size());
            if (end != std::string::npos && end - pos <= 64) s.erase(pos, end + pair.second.size() - pos);
            else pos += pair.first.size();
        }
    }
    static const char* literal[] = {"<tool_call>", "</tool_call>", "<tool_response>", "</tool_response>",
                                    "<think>", "</think>", "[INST]", "[/INST]", "<<SYS>>", "<</SYS>>",
                                    "<s>", "</s>", "<start_of_turn>", "<end_of_turn>", "[TOOL_CALLS]",
                                    "[AVAILABLE_TOOLS]", "[/AVAILABLE_TOOLS]", "[TOOL_RESULTS]", "[/TOOL_RESULTS]"};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* tok : literal) {
            size_t pos;
            while ((pos = s.find(tok)) != std::string::npos) { s.erase(pos, std::string(tok).size()); changed = true; }
        }
    }
    return s;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

static void append_utf8(std::string& out, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string html_decode_entities(const std::string& s) {
    static const std::pair<const char*, const char*> named[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"},
        {"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"}, {"ldquo", "\xE2\x80\x9C"},
        {"rdquo", "\xE2\x80\x9D"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"deg", "\xC2\xB0"},
        {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"}, {"trade", "\xE2\x84\xA2"}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        bool done = false;
        if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? std::stoul(ent.substr(2), nullptr, 16)
                                                                          : std::stoul(ent.substr(1));
                append_utf8(out, cp);
                done = true;
            } catch (...) {}
        } else {
            for (const auto& n : named)
                if (ent == n.first) { out += n.second; done = true; break; }
        }
        if (done) i = semi;
        else out += s[i];
    }
    return out;
}

std::string collapse_spaces(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string strip_tags(const std::string& html) {
    std::string out;
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>' && in_tag) in_tag = false;
        else if (!in_tag) out += c;
    }
    return collapse_spaces(html_decode_entities(out));
}

std::string html_to_text(const std::string& html, std::string* title_out) {
    const std::string low = lower(html);
    if (title_out) {
        auto a = low.find("<title");
        if (a != std::string::npos) {
            auto gt = low.find('>', a);
            auto b = gt == std::string::npos ? gt : low.find("</title>", gt);
            if (b != std::string::npos) *title_out = strip_tags(html.substr(gt + 1, b - gt - 1));
        }
    }
    static const std::set<std::string> skip = {"script", "style", "noscript", "svg", "template", "iframe", "head", "nav", "footer"};
    static const std::set<std::string> block = {"p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
                                                "section", "article", "header", "ul", "ol", "table", "blockquote",
                                                "pre", "hr", "dd", "dt", "main", "figure", "figcaption"};
    std::string out;
    out.reserve(html.size() / 3);
    size_t i = 0;
    auto body = low.find("<body");
    if (body != std::string::npos) i = body;
    while (i < html.size()) {
        if (html[i] != '<') {
            size_t next = html.find('<', i);
            if (next == std::string::npos) next = html.size();
            out.append(html, i, next - i);
            i = next;
            continue;
        }
        if (low.compare(i, 4, "<!--") == 0) {
            auto e = low.find("-->", i + 4);
            i = e == std::string::npos ? html.size() : e + 3;
            continue;
        }
        auto gt = low.find('>', i);
        if (gt == std::string::npos) break;
        std::string tag = low.substr(i + 1, gt - i - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        size_t ns = closing ? 1 : 0;
        size_t ne = tag.find_first_of(" \t\r\n/", ns);
        std::string name = tag.substr(ns, ne == std::string::npos ? std::string::npos : ne - ns);
        if (!closing && skip.count(name)) {
            auto e = low.find("</" + name, gt);
            if (e == std::string::npos) { i = html.size(); continue; }
            auto e2 = low.find('>', e);
            i = e2 == std::string::npos ? html.size() : e2 + 1;
            continue;
        }
        if (block.count(name)) out += '\n';
        if (!closing && name == "li") out += "- ";
        if (name == "td" || name == "th") out += ' ';
        i = gt + 1;
    }
    out = html_decode_entities(out);

    // Tidy whitespace: collapse spaces within lines, drop blank-line runs.
    std::string result, line;
    int blank_run = 0;
    auto flush = [&](const std::string& l) {
        std::string t = collapse_spaces(l);
        if (t.empty()) {
            if (++blank_run == 1 && !result.empty()) result += '\n';
            return;
        }
        blank_run = 0;
        result += t;
        result += '\n';
    };
    for (char c : out) {
        if (c == '\n') { flush(line); line.clear(); }
        else line += c;
    }
    flush(line);
    return trim(result);
}
