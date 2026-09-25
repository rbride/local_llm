// store.hpp - bot state. History and recent speakers live in memory; the rest is saved to state.json.
#pragma once
#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

struct ChatSettings {
    std::string persona;  // empty = use SYSTEM_PROMPT
    int thinking = -1;    // -1 = default, 0 = off, 1 = on
};

struct KnownUser {
    std::string name, username;  // username without "@", may be empty
};

struct AllowedUser {
    std::string name, via, date;  // via: how they got access, e.g. "talked in Group X"
};

struct LegacyMemory {
    long long chat_id;
    std::string text, by;
};

class Store {
public:
    explicit Store(std::string path) : path_(std::move(path)) {}
    void load();
    void save();  // call with mu held

    std::mutex mu;  // guards everything below
    std::map<long long, std::vector<json>> histories;                          // not saved
    std::map<long long, std::vector<std::pair<long long, std::string>>> speakers;  // chat -> recent (id, name), newest first; not saved
    std::map<long long, ChatSettings> chats;
    std::map<long long, AllowedUser> allowed;       // added automatically or with /allow
    std::set<long long> denied;                     // removed with /deny; never auto-added again
    std::map<long long, std::string> trusted_groups;  // id -> title
    std::map<long long, KnownUser> known;           // everyone the bot has seen
    std::vector<LegacyMemory> legacy_memories;      // old "remember" notes, migrated to facts.txt once

    void note_speaker(long long chat_id, long long user_id, const std::string& name);  // call with mu held
    long long find_user(const std::string& ref) const;  // "@name", "123", or a first name; 0 if unknown

private:
    std::string path_;
};
