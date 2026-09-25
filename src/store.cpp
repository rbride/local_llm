#include "store.hpp"

#include <cstdio>
#include <fstream>

#include "util.hpp"

void Store::load() {
    std::ifstream f(path_);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("chats") || !j["chats"].is_object()) {
        log("Warning: couldn't read " + path_ + ", starting fresh");
        return;
    }
    for (auto& [key, v] : j["chats"].items()) {
        ChatSettings s;
        s.persona = v.value("persona", "");
        s.thinking = v.value("thinking", -1);
        if (v.contains("memories") && v["memories"].is_array())
            for (const auto& m : v["memories"])
                s.memories.push_back({m.value("text", ""), m.value("by", ""), m.value("date", "")});
        try { chats[std::stoll(key)] = s; } catch (...) {}
    }
    log("Loaded saved state for " + std::to_string(chats.size()) + " chat(s) from " + path_);
}

void Store::save() {
    json j = {{"chats", json::object()}};
    for (const auto& [id, s] : chats) {
        if (s.persona.empty() && s.thinking == -1 && s.memories.empty()) continue;
        json mem = json::array();
        for (const auto& m : s.memories) mem.push_back({{"text", m.text}, {"by", m.by}, {"date", m.date}});
        j["chats"][std::to_string(id)] = {{"persona", s.persona}, {"thinking", s.thinking}, {"memories", mem}};
    }
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) { log("Warning: couldn't write " + tmp); return; }
        f << j.dump(2, ' ', false, json::error_handler_t::replace);
    }
    std::rename(tmp.c_str(), path_.c_str());
}
