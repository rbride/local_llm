#include "aliases.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "util.hpp"

namespace {

std::time_t file_mtime(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

std::string strip_at(const std::string& s) {
    return (!s.empty() && s[0] == '@') ? s.substr(1) : s;
}

std::string unescape_alias_input(const std::string& s) {
    return (s.size() >= 2 && s[0] == '\\' && s[1] == '!') ? trim(s.substr(2)) : s;
}

long long parse_id(const std::string& s) {
    try {
        size_t used = 0;
        long long id = std::stoll(s, &used);
        return used == s.size() ? id : 0;
    } catch (...) {
        return 0;
    }
}

size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

}  // namespace

Aliases::Aliases(std::string path) : path_(std::move(path)) {}

void Aliases::create_if_missing() {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_mtime(path_) != 0) return;
    std::ofstream f(path_);
    if (!f) {
        log("Couldn't create " + path_);
        return;
    }
    f << "# aliases.txt - nicknames for people. One line per person: <user_id>: name, name, @username\n"
         "# Matching is case-insensitive; a leading @ is optional. Edit any time; reloads automatically.\n"
         "# An alias starting with ! is owner-protected: regular admins can't delete it.\n"
         "# To keep an alias that really starts with !, write \\! before it.\n"
         "# Don't use global, me, or everywhere as an alias.\n"
         "\n";
    log("Created " + path_);
    reload_locked(true);
}

void Aliases::reload_locked(bool force) {
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

void Aliases::parse_locked() {
    people_.clear();
    for (size_t i = 0; i < lines_.size(); ++i) {
        std::string t = trim(lines_[i]);
        if (t.empty() || t[0] == '#') continue;
        auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        long long id = parse_id(trim(t.substr(0, colon)));
        if (id == 0) continue;
        Person p;
        p.user_id = id;
        p.line = i;
        std::stringstream ss(t.substr(colon + 1));
        std::string token;
        while (std::getline(ss, token, ',')) {
            token = trim(token);
            if (token.empty()) continue;
            bool protected_alias = false;
            if (token.size() >= 2 && token[0] == '\\' && token[1] == '!') {
                token = trim(token.substr(2));
            } else if (token[0] == '!') {
                protected_alias = true;
                token = trim(token.substr(1));
            }
            token = trim(sanitize_untrusted(token));
            if (token.empty()) continue;
            StoredAlias a;
            a.display = token;
            a.match = lower(strip_at(token));
            a.protected_ = protected_alias;
            p.aliases.push_back(a);
        }
        if (!p.aliases.empty()) people_.push_back(p);
    }
}

bool Aliases::write_locked() {
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

std::string Aliases::render_line(const Person& p) const {
    std::string out = std::to_string(p.user_id) + ":";
    for (size_t i = 0; i < p.aliases.size(); ++i) {
        out += i == 0 ? " " : ", ";
        const auto& a = p.aliases[i];
        if (a.protected_) out += "!";
        else if (!a.display.empty() && a.display[0] == '!') out += "\\";
        out += a.display;
    }
    return out;
}

Aliases::Person* Aliases::find_person_locked(long long user_id) {
    for (auto& p : people_)
        if (p.user_id == user_id) return &p;
    return nullptr;
}

size_t Aliases::count_aliases_locked(long long user_id) {
    size_t n = 0;
    for (const auto& p : people_)
        if (p.user_id == user_id) n += p.aliases.size();
    return n;
}

long long Aliases::resolve(const std::string& name_in) {
    std::string name = unescape_alias_input(trim(sanitize_untrusted(name_in)));
    std::string m = lower(strip_at(name));
    if (m.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    for (const auto& p : people_)
        for (const auto& a : p.aliases)
            if (a.match == m) return p.user_id;
    if (m.size() > 1 && m[0] == '!') {
        std::string stripped = lower(m.substr(1));
        for (const auto& p : people_)
            for (const auto& a : p.aliases)
                if (a.match == stripped) return p.user_id;
    }
    return 0;
}

std::vector<std::string> Aliases::names_for(long long user_id) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    for (const auto& p : people_)
        if (p.user_id == user_id)
            for (const auto& a : p.aliases) out.push_back(a.display);
    return out;
}

std::vector<AliasInfo> Aliases::entries(long long user_id) {
    std::vector<AliasInfo> out;
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    for (const auto& p : people_)
        if (p.user_id == user_id)
            for (const auto& a : p.aliases) out.push_back({a.display, a.protected_});
    return out;
}

std::map<long long, std::vector<AliasInfo>> Aliases::all() {
    std::map<long long, std::vector<AliasInfo>> out;
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();
    for (const auto& p : people_)
        for (const auto& a : p.aliases) out[p.user_id].push_back({a.display, a.protected_});
    return out;
}

bool Aliases::add(long long user_id, const std::string& name_in, bool protected_alias, std::string& error) {
    if (user_id == 0) {
        error = "no user id";
        return false;
    }
    std::string name = trim(sanitize_untrusted(name_in));
    if (name.size() >= 2 && name[0] == '\\' && name[1] == '!') {
        name = trim(name.substr(2));
    } else if (protected_alias && !name.empty() && name[0] == '!') {
        protected_alias = true;
        name = trim(name.substr(1));
    }
    if (name.empty()) {
        error = "empty alias";
        return false;
    }
    if (utf8_length(name) > 40) {
        error = "alias is too long (max 40 characters)";
        return false;
    }
    if (name.find_first_of(",:\n\r") != std::string::npos) {
        error = "alias can't contain commas, colons, or newlines";
        return false;
    }
    std::string m = lower(strip_at(name));
    if (m == "global" || m == "me" || m == "everywhere") {
        error = "'" + name + "' is reserved for fact scopes";
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();

    bool already_same = false;
    long long other = 0;
    std::string other_name;
    for (const auto& p : people_) {
        for (const auto& a : p.aliases) {
            if (a.match != m) continue;
            if (p.user_id == user_id) already_same = true;
            else if (!other) {
                other = p.user_id;
                other_name = a.display;
            }
        }
    }
    if (other) {
        if (other_name.empty()) other_name = std::to_string(other);
        error = "'" + name + "' already means " + other_name + " \xE2\x80\x94 /unalias it first.";
        return false;
    }
    if (already_same) {
        error = "already saved";
        return false;
    }
    if (count_aliases_locked(user_id) >= 20) {
        error = "that person already has 20 aliases";
        return false;
    }

    if (Person* p = find_person_locked(user_id)) {
        p->aliases.push_back({name, m, protected_alias});
        if (p->line < lines_.size()) lines_[p->line] = render_line(*p);
    } else {
        if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.push_back("");
        Person person;
        person.user_id = user_id;
        person.line = lines_.size();
        person.aliases.push_back({name, m, protected_alias});
        lines_.push_back(render_line(person));
        people_.push_back(std::move(person));
    }
    if (!write_locked()) {
        error = "couldn't write " + path_;
        return false;
    }
    return true;
}

bool Aliases::remove(const std::string& name_in, bool as_owner, std::string& error) {
    std::string raw = unescape_alias_input(trim(sanitize_untrusted(name_in)));
    std::vector<std::string> candidates;
    auto add_candidate = [&](const std::string& s) {
        std::string m = lower(strip_at(s));
        if (!m.empty() && std::find(candidates.begin(), candidates.end(), m) == candidates.end()) candidates.push_back(m);
    };
    add_candidate(raw);
    if (raw.size() > 1 && raw[0] == '!') add_candidate(raw.substr(1));
    if (candidates.empty()) {
        error = "empty alias";
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    reload_locked();

    size_t pidx = 0, aidx = 0;
    bool found = false;
    for (const auto& m : candidates) {
        for (size_t i = 0; i < people_.size() && !found; ++i) {
            for (size_t j = 0; j < people_[i].aliases.size(); ++j) {
                if (people_[i].aliases[j].match == m) {
                    pidx = i;
                    aidx = j;
                    found = true;
                    break;
                }
            }
        }
        if (found) break;
    }
    if (!found) {
        error = "I don't know that alias.";
        return false;
    }
    if (people_[pidx].aliases[aidx].protected_ && !as_owner) {
        error = "That alias is owner-protected.";
        return false;
    }
    if (people_[pidx].aliases.size() == 1) {
        if (people_[pidx].line < lines_.size()) lines_.erase(lines_.begin() + static_cast<long>(people_[pidx].line));
        people_.erase(people_.begin() + static_cast<long>(pidx));
    } else {
        people_[pidx].aliases.erase(people_[pidx].aliases.begin() + static_cast<long>(aidx));
        if (people_[pidx].line < lines_.size()) lines_[people_[pidx].line] = render_line(people_[pidx]);
    }
    if (!write_locked()) {
        error = "couldn't write " + path_;
        return false;
    }
    return true;
}
