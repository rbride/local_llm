#include "facts.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "util.hpp"

namespace {
std::time_t file_mtime(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

std::string header_for(const std::string& type, long long id, const std::string& label) {
    if (type == "global") return "[global]";
    std::string clean;
    for (char c : label) if (c != '[' && c != ']' && c != '\n') clean += c;
    return "[" + type + " " + std::to_string(id) + (clean.empty() ? "" : " " + trim(clean)) + "]";
}
}  // namespace

Facts::Facts(std::string path) : path_(std::move(path)) {}

void Facts::create_if_missing() {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_mtime(path_) != 0) return;
    std::ofstream f(path_);
    f << "# facts.txt - things the bot knows about people and chats.\n"
         "# Edit this any time; the bot picks up changes automatically, no restart needed.\n"
         "#\n"
         "# Sections:\n"
         "#   [global]                    facts for every chat\n"
         "#   [user <id> <name>]          facts about one person (find ids with /users or /id)\n"
         "#   [chat <id> <title>]         facts about one group (the group's id is shown by /facts there)\n"
         "# Each fact is one line, usually starting with \"- \". Lines starting with # are comments.\n"
         "# The bot is told these are facts, not instructions.\n"
         "\n"
         "[global]\n";
    log("Created " + path_);
}

void Facts::reload_locked(bool force) {
    std::time_t m = file_mtime(path_);
    if (!force && m == mtime_) return;
    mtime_ = m;
    lines_.clear();
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(line);
    }
    parse_locked();
}

void Facts::parse_locked() {
    sections_.clear();
    int cur = -1;
    for (size_t i = 0; i < lines_.size(); ++i) {
        std::string t = trim(lines_[i]);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            std::stringstream ss(trim(t.substr(1, t.size() - 2)));
            Section s;
            ss >> s.type;
            s.type = lower(s.type);
            s.header_line = i;
            if (s.type == "user" || s.type == "chat") {
                std::string id;
                ss >> id;
                try { s.id = std::stoll(id); } catch (...) { cur = -1; continue; }
                std::getline(ss, s.label);
                s.label = trim(s.label);
            } else if (s.type != "global") {
                cur = -1;  // unknown section: ignore its lines
                continue;
            }
            sections_.push_back(s);
            cur = static_cast<int>(sections_.size()) - 1;
            continue;
        }
        if (cur < 0) continue;
        for (const char* bullet : {"- ", "* ", "\xE2\x80\xA2 "})
            if (starts_with(t, bullet)) { t = trim(t.substr(std::string(bullet).size())); break; }
        if (!t.empty()) sections_[cur].facts.push_back({i, sanitize_untrusted(t)});
    }
}

bool Facts::write_locked() {
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        for (const auto& l : lines_) f << l << "\n";
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) return false;
    mtime_ = file_mtime(path_);
    parse_locked();
    return true;
}

Facts::Section* Facts::find_locked(const std::string& type, long long id) {
    for (auto& s : sections_)
        if (s.type == type && (type == "global" || s.id == id)) return &s;
    return nullptr;
}

std::string Facts::context(long long chat_id, const std::vector<std::pair<long long, std::string>>& people, size_t max_chars) {
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    std::string out;
    bool truncated = false;
    std::vector<const Section*> used;
    auto add = [&](const std::string& title, const std::string& type, long long id) {
        for (const auto& s : sections_) {
            if (s.type != type || (type != "global" && s.id != id) || s.facts.empty()) continue;
            bool dup = false;
            for (auto* u : used) dup |= u == &s;
            if (dup) continue;
            std::string block = title + ":\n";
            for (const auto& f : s.facts) block += "- " + f.second + "\n";
            if (out.size() + block.size() > max_chars) { truncated = true; return; }
            out += block;
            used.push_back(&s);
        }
    };
    add("General", "global", 0);
    add("About this chat", "chat", chat_id);
    for (const auto& [id, name] : people) add("About " + name, "user", id);
    if (truncated) out += "(There are more notes than fit here; use the look_up_facts tool to search them.)\n";
    return trim(out);
}

json Facts::lookup(const std::string& query, size_t max_results) {
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    std::string q = lower(trim(query));
    if (!q.empty() && q[0] == '@') q = q.substr(1);
    json results = json::array();
    size_t total = 0;
    for (const auto& s : sections_) {
        bool section_match = q.empty() || lower(s.label).find(q) != std::string::npos || std::to_string(s.id) == q || s.type == q;
        for (const auto& f : s.facts) {
            if (!section_match && lower(f.second).find(q) == std::string::npos) continue;
            ++total;
            if (results.size() >= max_results) continue;
            json r = {{"about", s.type == "global" ? std::string("general") : s.type + " " + (s.label.empty() ? std::to_string(s.id) : s.label)},
                      {"fact", f.second}};
            if (s.type == "user") r["user_id"] = s.id;
            results.push_back(r);
        }
    }
    return {{"query", query}, {"results", results}, {"total_matches", total}};
}

std::vector<FactRef> Facts::section(const std::string& type, long long id) {
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    std::vector<FactRef> out;
    for (const auto& s : sections_)
        if (s.type == type && (type == "global" || s.id == id))
            for (const auto& f : s.facts) out.push_back({s.type, s.id, s.label, f.second});
    return out;
}

bool Facts::add(const std::string& type, long long id, const std::string& label, const std::string& text_in, std::string& error) {
    std::string text;
    for (char c : sanitize_untrusted(text_in)) text += (c == '\n' || c == '\r') ? ' ' : c;
    text = utf8_head(trim(text), 400);
    if (text.empty()) { error = "empty fact"; return false; }
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    if (Section* s = find_locked(type, id)) {
        for (const auto& f : s->facts)
            if (lower(f.second) == lower(text)) { error = "already saved"; return false; }
        size_t after = s->facts.empty() ? s->header_line : s->facts.back().first;
        lines_.insert(lines_.begin() + static_cast<long>(after) + 1, "- " + text);
    } else {
        if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.push_back("");
        lines_.push_back(header_for(type, id, label));
        lines_.push_back("- " + text);
    }
    if (!write_locked()) { error = "couldn't write " + path_; return false; }
    return true;
}

bool Facts::remove(const FactRef& ref) {
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    Section* s = find_locked(ref.type, ref.id);
    if (!s) return false;
    for (const auto& f : s->facts)
        if (f.second == ref.text) {
            lines_.erase(lines_.begin() + static_cast<long>(f.first));
            return write_locked();
        }
    return false;
}

size_t Facts::count() {
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    size_t n = 0;
    for (const auto& s : sections_) n += s.facts.size();
    return n;
}
