// config.hpp - settings from environment variables or bot.env (see bot.env.example)
#pragma once
#include <map>
#include <set>
#include <string>

struct Config {
    // Telegram / access
    std::string token;
    std::string telegram_api = "https://api.telegram.org";  // or a self-hosted Bot API server
    std::set<long long> allowed_users;
    bool allow_everyone = false;
    std::set<long long> admin_users;
    int rate_limit_per_min = 8;      // per user, 0 = off; admins are exempt
    int max_queue = 20;

    // Model server
    std::string llm_url, llm_model, llm_api_key;
    std::string system_prompt;
    bool persona_reminder = true;
    size_t max_history = 20;
    int max_tokens = 6000;
    int retry_max_tokens = 2048;
    double temperature = 0.7;
    long llm_timeout = 600;          // seconds without any data before giving up
    bool thinking = true;            // default per chat, toggle with /think

    // Streaming
    bool streaming = true;
    long draft_interval_ms = 900;    // private chats (sendMessageDraft)
    long edit_interval_ms = 3500;    // groups (edit a real message)
    bool stop_button = true;         // show Telegram's "stop" button on drafts

    // Tools
    std::set<std::string> tools;     // enabled tool names
    int max_tool_rounds = 5;
    std::string searxng_url, brave_api_key;
    int search_results = 5;
    size_t fetch_max_chars = 8000;
    std::string weather_units = "metric";
    size_t memory_max = 50;
    bool show_tool_footer = true;

    std::string state_file = "state.json";

    void load();
    bool is_admin(long long user_id) const { return admin_users.count(user_id) > 0; }
    bool tool_enabled(const std::string& name) const { return tools.count(name) > 0; }

private:
    std::map<std::string, std::string> file_vals_;
    void load_file(const std::string& path);
    std::string get(const std::string& key, const std::string& def = "") const;
    bool get_bool(const std::string& key, bool def) const;
};

extern const char* const ALL_TOOLS[];
