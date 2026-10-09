#include "store.hpp"

#include <cstdio>
#include <fstream>

#include "util.hpp"

namespace {

// ChatSettings <-> state.json. A new ChatSettings field must be added to both of
// these AND to ChatSettings::overrides_named(), which decides whether a chat is saved at all.
ChatSettings settings_from(const json& v) {
    ChatSettings s;
    if (!v.is_object()) return s;
    s.persona = v.value("persona", "");
    s.thinking = v.value("thinking", -1);
    s.temperature = v.value("temperature", -1.0);
    s.max_history = v.value("max_history", -1);
    s.max_tokens = v.value("max_tokens", -1);
    s.think_budget = v.value("think_budget", -1);
    s.persona_reminder = v.value("persona_reminder", -1);
    return s;
}

json settings_to_json(const ChatSettings& s) {
    return {{"persona", s.persona},
            {"thinking", s.thinking},
            {"temperature", s.temperature},
            {"max_history", s.max_history},
            {"max_tokens", s.max_tokens},
            {"think_budget", s.think_budget},
            {"persona_reminder", s.persona_reminder}};
}

}  // namespace

void Store::load() {
    std::ifstream f(path_);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log("Warning: couldn't read " + path_ + ", starting fresh");
        return;
    }
    auto id_of = [](const std::string& k) -> long long { try { return std::stoll(k); } catch (...) { return 0; } };
    if (j.contains("chats") && j["chats"].is_object())
        for (auto& [key, v] : j["chats"].items()) {
            long long id = id_of(key);
            if (!id) continue;
            chats[id] = settings_from(v);
            if (v.contains("memories") && v["memories"].is_array())  // from v2
                for (const auto& m : v["memories"]) legacy_memories.push_back({id, m.value("text", ""), m.value("by", "")});
        }
    if (j.contains("global_settings")) global_settings = settings_from(j["global_settings"]);
    if (j.contains("allowed") && j["allowed"].is_object())
        for (auto& [key, v] : j["allowed"].items())
            if (long long id = id_of(key)) allowed[id] = {v.value("name", ""), v.value("via", ""), v.value("date", "")};
    if (j.contains("denied") && j["denied"].is_array())
        for (const auto& v : j["denied"]) if (v.is_number_integer()) denied.insert(v.get<long long>());
    if (j.contains("trusted_groups") && j["trusted_groups"].is_object())
        for (auto& [key, v] : j["trusted_groups"].items())
            if (long long id = id_of(key)) trusted_groups[id] = v.is_string() ? v.get<std::string>() : "";
    if (j.contains("known") && j["known"].is_object())
        for (auto& [key, v] : j["known"].items())
            if (long long id = id_of(key)) known[id] = {v.value("name", ""), v.value("username", "")};
    if (j.contains("update_offset") && j["update_offset"].is_number_integer())
        update_offset = j["update_offset"].get<long long>();
    log("Loaded " + path_ + ": " + std::to_string(allowed.size()) + " allowed user(s), " +
        std::to_string(trusted_groups.size()) + " trusted group(s)");
}

void Store::save() {
    json j = {{"update_offset", update_offset},
              {"chats", json::object()}, {"allowed", json::object()}, {"denied", json::array()},
              {"trusted_groups", json::object()}, {"known", json::object()}};
    for (const auto& [id, s] : chats)
        if (s.has_overrides())
            j["chats"][std::to_string(id)] = settings_to_json(s);
    if (global_settings.has_overrides()) j["global_settings"] = settings_to_json(global_settings);
    for (const auto& [id, a] : allowed) j["allowed"][std::to_string(id)] = {{"name", a.name}, {"via", a.via}, {"date", a.date}};
    for (long long id : denied) j["denied"].push_back(id);
    for (const auto& [id, t] : trusted_groups) j["trusted_groups"][std::to_string(id)] = t;
    for (const auto& [id, k] : known) j["known"][std::to_string(id)] = {{"name", k.name}, {"username", k.username}};

    std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) { log("Warning: couldn't write " + tmp); return; }
        f << j.dump(2, ' ', false, json::error_handler_t::replace);
    }
    std::rename(tmp.c_str(), path_.c_str());
}

void Store::note_speaker(long long chat_id, long long user_id, const std::string& name) {
    auto& v = speakers[chat_id];
    for (auto it = v.begin(); it != v.end(); ++it)
        if (it->first == user_id) { v.erase(it); break; }
    v.insert(v.begin(), {user_id, name});
    if (v.size() > 8) v.resize(8);
}

long long Store::find_user(const std::string& ref_in) const {
    std::string ref = trim(ref_in);
    if (ref.empty()) return 0;
    if (ref[0] == '@') {
        std::string u = lower(ref.substr(1));
        for (const auto& [id, k] : known) if (lower(k.username) == u) return id;
        return 0;
    }
    try {
        size_t used = 0;
        long long id = std::stoll(ref, &used);
        if (used == ref.size()) return id;
    } catch (...) {}
    long long match = 0;
    for (const auto& [id, k] : known)
        if (lower(k.name) == lower(ref)) {
            if (match) return 0;  // ambiguous first name
            match = id;
        }
    return match;
}
