// facts.hpp - facts.txt: notes about people and chats that the bot reads for context.
//
// The file is meant to be edited by hand. The bot reloads it whenever it changes, and admin
// commands (/fact, /unfact) edit it line by line, so your comments and layout are kept.
//
//   [global]
//   - The bot's owner is Kaiser.
//
//   [user 8434412045 Ryan]
//   - loves tacos
//
//   [chat -1003932332088 Nathan Chat]
//   - Friday is movie night
#pragma once
#include <nlohmann/json.hpp>

#include <ctime>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

struct FactRef {
    std::string type;  // "global", "user", "chat"
    long long id = 0;  // user or chat id (0 for global)
    std::string label; // name/title from the header
    std::string text;
};

class Facts {
public:
    explicit Facts(std::string path);

    void create_if_missing();
    // Text block for the system prompt: global facts, this chat's facts, and facts about the listed people.
    std::string context(long long chat_id, const std::vector<std::pair<long long, std::string>>& people, size_t max_chars);
    // Search everything by name, id or keyword (for the look_up_facts tool).
    json lookup(const std::string& query, size_t max_results = 40);
    // All facts in one section, in file order.
    std::vector<FactRef> section(const std::string& type, long long id);
    bool add(const std::string& type, long long id, const std::string& label, const std::string& text, std::string& error);
    bool remove(const FactRef& f);
    size_t count();

private:
    struct Section {
        std::string type, label;
        long long id = 0;
        size_t header_line = 0;
        std::vector<std::pair<size_t, std::string>> facts;  // (line index, text)
    };
    std::string path_;
    std::mutex mu_;
    std::vector<std::string> lines_;
    std::vector<Section> sections_;
    std::time_t mtime_ = 0;

    void reload_locked(bool force = false);
    void parse_locked();
    bool write_locked();
    Section* find_locked(const std::string& type, long long id);
};
