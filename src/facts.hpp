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
//
// A fact line starting with "- !" is owner-protected: regular admins can't delete it.
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
    bool protected_ = false;
    long long chat_id = 0; // for user facts: 0 = everywhere, otherwise only this chat
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
    std::vector<FactRef> section(const std::string& type, long long id, long long chat_id = 0, bool all_user_scopes = false);
    bool add(const std::string& type, long long id, const std::string& label, const std::string& text, std::string& error, bool protected_fact = false, long long chat_id = 0);
    bool remove(const FactRef& f, bool as_owner = false);
    // Delete every fact in one section (Task D /forget). Owner-protected lines are only
    // removed when as_owner; returns how many facts were deleted.
    size_t clear_section(const std::string& type, long long id, bool as_owner, long long chat_id = 0);
    // Rewrite the whole file to the empty template (Task I /wipefacts, owner-only at the call site).
    bool wipe();
    size_t count();

private:
    struct StoredFact {
        size_t line = 0;
        std::string text;
        bool protected_ = false;
    };
    struct Section {
        std::string type, label;
        long long id = 0;
        long long chat_id = 0;
        size_t header_line = 0;
        std::vector<StoredFact> facts;
    };
    std::string path_;
    std::mutex mu_;
    std::vector<std::string> lines_;
    std::vector<Section> sections_;
    std::time_t mtime_ = 0;

    void reload_locked(bool force = false);
    void parse_locked();
    bool write_locked();
    Section* find_locked(const std::string& type, long long id, long long chat_id = 0);
};
