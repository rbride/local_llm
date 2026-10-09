// store.hpp - bot state. History and recent speakers live in memory; the rest is saved to state.json.
#pragma once
#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

// Every setting starts out unset (-1 / empty) and falls through the layers:
// this chat's override -> Store::global_settings -> the bot.env default.
struct ChatSettings {
    std::string persona;        // empty = use the layer below
    int thinking = -1;          // -1 = use the layer below, 0 = off, 1 = on
    double temperature = -1;    // -1 = use the layer below
    int max_history = -1;       // -1 = use the layer below
    int max_tokens = -1;        // -1 = use the layer below
    int think_budget = -1;      // -1 = use the layer below; 0 = no cap; >0 = thinking cap (Task H)
    int persona_reminder = -1;  // -1 = use the layer below

    // Names of the fields this layer actually sets; drives the /defaults replies and
    // has_overrides(), so a future field can't be forgotten at save time.
    std::vector<std::string> overrides_named() const {
        std::vector<std::string> names;
        if (!persona.empty()) names.push_back("persona");
        if (thinking != -1) names.push_back("thinking");
        if (temperature != -1.0) names.push_back("temperature");
        if (max_history != -1) names.push_back("history");
        if (max_tokens != -1) names.push_back("max tokens");
        if (think_budget != -1) names.push_back("think budget");
        if (persona_reminder != -1) names.push_back("reminder");
        return names;
    }
    bool has_overrides() const { return !overrides_named().empty(); }
};

// The three layers, highest priority first: per-chat override (admin), the owner's
// global override, the factory default from bot.env. `unset` is this struct's
// sentinel for "this layer doesn't decide". Every *_for helper funnels through here.
template <typename T>
T resolve_setting(const T& chat_val, const T& global_val, const T& def, const T& unset) {
    if (!(chat_val == unset)) return chat_val;
    if (!(global_val == unset)) return global_val;
    return def;
}

// Ranges the admin /context and /temp commands clamp to (Task C), so a typo can't
// ask for a 0-message history or a wild temperature. The message-history count is
// not the model's token context window (-c), which is fixed at llama-server launch.
inline int clamp_history(int n) { return n < 2 ? 2 : (n > 500 ? 500 : n); }
inline double clamp_temperature(double t) { return t < 0.0 ? 0.0 : (t > 2.0 ? 2.0 : t); }

// The answer cap the admin /maxtokens command clamps to (Task EG1): big enough for a
// real answer, never so big that a runaway reply ties up the model forever.
inline int clamp_max_tokens(int n) { return n < 64 ? 64 : (n > 16000 ? 16000 : n); }

// The thinking cap the admin /think budget command clamps to (Task H): 0 = no cap
// (thinking runs free under MAX_TOTAL_TOKENS). The -1 sentinel means "follow the layer
// below" and never reaches the clamp; /think budget default clears to it instead.
inline int clamp_think_budget(int n) { return n < 0 ? 0 : (n > 1000000 ? 1000000 : n); }

// Persona reminder cadence (Task M): 0 = off, N = every Nth user message. The first
// user message after a reset or persona change (count == 1) always gets the reminder.
inline int clamp_reminder(int n) { return n < 0 ? 0 : (n > 50 ? 50 : n); }
inline bool remind_now(int n, size_t user_msgs) {
    return n > 0 && user_msgs > 0 && (user_msgs == 1 || user_msgs % n == 0);
}

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
    ChatSettings global_settings;  // owner-set; applies to every chat without its own override
    std::map<long long, AllowedUser> allowed;       // added automatically or with /allow
    std::set<long long> denied;                     // removed with /deny; never auto-added again
    std::map<long long, std::string> trusted_groups;  // id -> title
    std::map<long long, KnownUser> known;           // everyone the bot has seen
    std::vector<LegacyMemory> legacy_memories;      // old "remember" notes, migrated to facts.txt once
    long long update_offset = 0;                    // last Telegram update_id + 1, saved so /restart doesn't replay

    void note_speaker(long long chat_id, long long user_id, const std::string& name);  // call with mu held
    long long find_user(const std::string& ref) const;  // "@name", "123", or a first name; 0 if unknown

private:
    std::string path_;
};
