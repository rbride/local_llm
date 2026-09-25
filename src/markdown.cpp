#include "markdown.hpp"

#include <cctype>
#include <sstream>
#include <vector>

#include "util.hpp"

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n'; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) & 0x80); }

std::string inline_md(const std::string& s, int depth);

// Try a paired delimiter like ** or ~~ at position i. On success appends and advances i.
bool try_pair(const std::string& s, size_t& i, const std::string& delim, const char* open, const char* close,
              int depth, std::string& out) {
    if (s.compare(i, delim.size(), delim) != 0) return false;
    size_t start = i + delim.size();
    size_t e = s.find(delim, start);
    if (e == std::string::npos || e == start) return false;
    std::string inner = s.substr(start, e - start);
    if (is_space(inner.front()) || is_space(inner.back()) || inner.find('\n') != std::string::npos) return false;
    out += open;
    out += inline_md(inner, depth + 1);
    out += close;
    i = e + delim.size();
    return true;
}

bool try_italic(const std::string& s, size_t& i, int depth, std::string& out) {
    char c = s[i];
    if (c != '*' && c != '_') return false;
    if (i + 1 >= s.size() || is_space(s[i + 1]) || s[i + 1] == c) return false;
    if (i > 0 && is_word(s[i - 1])) return false;  // snake_case, 2*3
    for (size_t e = i + 2; e < s.size(); ++e) {
        if (s[e] == '\n') return false;
        if (s[e] != c) continue;
        if (is_space(s[e - 1]) || s[e - 1] == c) continue;
        if (e + 1 < s.size() && (is_word(s[e + 1]) || s[e + 1] == c)) continue;
        out += "<i>";
        out += inline_md(s.substr(i + 1, e - i - 1), depth + 1);
        out += "</i>";
        i = e + 1;
        return true;
    }
    return false;
}

bool try_link(const std::string& s, size_t& i, int depth, std::string& out) {
    if (s[i] != '[') return false;
    size_t mid = s.find("](", i + 1);
    if (mid == std::string::npos) return false;
    size_t end = s.find(')', mid + 2);
    if (end == std::string::npos) return false;
    std::string text = s.substr(i + 1, mid - i - 1);
    std::string url = s.substr(mid + 2, end - mid - 2);
    if (text.empty() || text.find('\n') != std::string::npos || text.find('[') != std::string::npos) return false;
    if (!starts_with(url, "http://") && !starts_with(url, "https://")) return false;
    if (url.find_first_of(" \"<>\n") != std::string::npos) return false;
    out += "<a href=\"" + html_escape(url) + "\">" + inline_md(text, depth + 1) + "</a>";
    i = end + 1;
    return true;
}

std::string inline_md(const std::string& s, int depth) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '`') {
            size_t e = s.find('`', i + 1);
            if (e != std::string::npos && e > i + 1) {
                out += "<code>" + html_escape(s.substr(i + 1, e - i - 1)) + "</code>";
                i = e + 1;
                continue;
            }
        }
        if (depth < 4) {
            if (try_pair(s, i, "**", "<b>", "</b>", depth, out)) continue;
            if (try_pair(s, i, "__", "<b>", "</b>", depth, out)) continue;
            if (try_pair(s, i, "~~", "<s>", "</s>", depth, out)) continue;
            if (try_pair(s, i, "||", "<tg-spoiler>", "</tg-spoiler>", depth, out)) continue;
            if (try_italic(s, i, depth, out)) continue;
            if (try_link(s, i, depth, out)) continue;
        }
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
        ++i;
    }
    return out;
}

bool is_rule(const std::string& t) {
    if (t.size() < 3) return false;
    for (char c : t) if (c != '-' && c != '*' && c != '_' && c != ' ') return false;
    return true;
}

bool is_table_separator(const std::string& t) {
    for (char c : t) if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    return true;
}

}  // namespace

std::string markdown_to_html(const std::string& md) {
    std::vector<std::string> lines;
    {
        std::stringstream ss(md);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    std::string out, code, lang;
    std::vector<std::string> quote, table;
    bool in_code = false;

    auto flush_quote = [&] {
        if (quote.empty()) return;
        out += "<blockquote>";
        for (size_t k = 0; k < quote.size(); ++k) out += (k ? "\n" : "") + inline_md(quote[k], 0);
        out += "</blockquote>\n";
        quote.clear();
    };
    auto flush_table = [&] {
        if (table.empty()) return;
        out += "<pre>";
        for (const auto& row : table) out += html_escape(row) + "\n";
        out += "</pre>\n";
        table.clear();
    };
    auto emit_code = [&] {
        bool lang_ok = !lang.empty() && lang.size() < 20;
        for (char c : lang) lang_ok &= std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '#' || c == '-';
        if (!code.empty() && code.back() == '\n') code.pop_back();
        out += lang_ok ? "<pre><code class=\"language-" + lang + "\">" : "<pre><code>";
        out += html_escape(code) + "</code></pre>\n";
        code.clear();
    };

    for (const auto& line : lines) {
        std::string t = trim(line);
        if (starts_with(t, "```")) {
            flush_quote();
            flush_table();
            if (!in_code) { in_code = true; lang = trim(t.substr(3)); code.clear(); }
            else { emit_code(); in_code = false; }
            continue;
        }
        if (in_code) { code += line + "\n"; continue; }

        if (starts_with(t, ">")) { flush_table(); quote.push_back(trim(t.substr(1))); continue; }
        flush_quote();
        if (t.size() >= 2 && t.front() == '|' && t.back() == '|') {
            if (!is_table_separator(t)) table.push_back(t);
            continue;
        }
        flush_table();

        size_t hashes = 0;
        while (hashes < t.size() && t[hashes] == '#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes < t.size() && t[hashes] == ' ') {
            out += "<b>" + inline_md(trim(t.substr(hashes)), 0) + "</b>\n";
            continue;
        }
        if (is_rule(t)) { out += "\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\n"; continue; }
        if (t.size() > 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            size_t indent = line.find_first_not_of(" \t");
            out += std::string(indent == std::string::npos ? 0 : (indent / 2) * 2, ' ');
            out += "\xE2\x80\xA2 " + inline_md(t.substr(2), 0) + "\n";
            continue;
        }
        out += inline_md(line, 0) + "\n";
    }
    if (in_code) emit_code();
    flush_quote();
    flush_table();
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
