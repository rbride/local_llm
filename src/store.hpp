// store.hpp - per-chat state: history (in memory), persona/settings/memories (saved to disk)
#pragma once
#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

struct Memory {
    std::string text, by, date;
};

struct ChatSettings {
    std::string persona;       // empty = use SYSTEM_PROMPT
    int thinking = -1;         // -1 = default, 0 = off, 1 = on
    std::vector<Memory> memories;
};

class Store {
public:
    explicit Store(std::string path) : path_(std::move(path)) {}
    void load();
    void save();  // call with mu held

    std::mutex mu;  // guards everything below
    std::map<long long, std::vector<json>> histories;  // not persisted
    std::map<long long, ChatSettings> chats;

private:
    std::string path_;
};
