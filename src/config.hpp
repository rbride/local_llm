// config.hpp - settings from environment variables or bot.env (see bot.env.example)
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>

struct Config {
    // Telegram / access
    std::string token;
    std::string telegram_api = "https://api.telegram.org";  // or a self-hosted Bot API server
    std::set<long long> allowed_users;
    bool allow_everyone = false;
    std::set<long long> owner_users;
    std::set<long long> admin_users;
    std::set<long long> trusted_chats;   // groups trusted without checking (TRUSTED_CHAT_IDS)
    bool allow_group_members = true;     // auto-allow people who talk in trusted groups
    bool leave_untrusted_groups = true;  // leave groups no admin is in
    std::set<std::string> public_commands;
    int rate_limit_per_min = 8;      // per user, 0 = off; admins are exempt
    int max_queue = 20;

    // Model server
    std::string llm_url, llm_model, llm_api_key;
    std::string system_prompt;
    int persona_reminder = 1;            // 0 = off, 1 = every message, N = every Nth message
    size_t persona_reminder_max_chars = 600;  // cap on the copy repeated into the last user message
    size_t max_history = 20;
    int max_tokens = 6000;         // answer cap only; thinking has its own budget
    int max_total_tokens = 32768;  // safety ceiling for the server when thinking has no budget
    // Default thinking cap in tokens (THINK_BUDGET, Task H). 0 = off: no separate cap,
    // thinking runs free under MAX_TOTAL_TOKENS. Admins cap a chat with /think budget <n>.
    int think_budget = 0;
    // Task H: thinking is off, but if reasoning arrives anyway the bot wraps it up at
    // this many reasoning tokens, so it is also the server-ceiling headroom for
    // thinking-off requests (FALLBACK_THINK_TOKENS).
    int fallback_think_tokens = 1024;
    double temperature = 0.7;
    // Mode-specific sampling (Task EG2). Unset *_TEMPERATURE falls back to TEMPERATURE;
    // unset top_p / presence_penalty means the field is not sent to the server at all.
    std::optional<double> temperature_thinking;       // TEMPERATURE_THINKING
    std::optional<double> temperature_no_thinking;    // TEMPERATURE_NO_THINKING
    std::optional<double> top_p_thinking;             // TOP_P_THINKING
    std::optional<double> top_p_no_thinking;          // TOP_P_NO_THINKING
    std::optional<double> presence_penalty_thinking;  // PRESENCE_PENALTY_THINKING
    std::optional<double> presence_penalty_no_thinking;  // PRESENCE_PENALTY_NO_THINKING
    long llm_timeout = 600;          // seconds without any data before giving up
    bool thinking = true;            // default per chat, toggle with /think

    // Model switching (Task J). The bot never loads a model itself; llama-server does.
    // A "model" is a folder directly under models_dir that contains a server.args file.
    // /model writes the chosen folder name to models_dir/.current_model; the external
    // scripts/run-model.sh reads it and (re)launches llama-server. MODEL_RELOAD_CMD, if
    // set, is run (via /bin/sh -c) right after the switch so a supervisor can restart
    // the server; empty means the admin reloads it by hand.
    std::string models_dir = "/models";
    std::string model_reload_cmd;    // MODEL_RELOAD_CMD, empty = manual reload

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
    std::string facts_file = "facts.txt";
    std::string aliases_file = "aliases.txt";
    size_t facts_context_chars = 3000;  // how much of facts.txt goes into each prompt
    bool show_tool_footer = true;

    std::string state_file = "state.json";
    std::string log_file = "tgbot.log";    // LOG_FILE, empty = console only

    // /rebase and /restart
    std::string git_repo_dir;            // empty = folder containing the tgbot binary
    std::string git_remote = "origin";
    std::string git_branch = "main";
    std::string build_command = "make";

    void load();
    // PERSONA_REMINDER value: true/yes/on -> 1, false/no/off -> 0, otherwise a non-negative
    // integer. Invalid input falls back to def and logs a warning.
    static int parse_reminder(const std::string& raw, int def);
    bool is_owner(long long user_id) const { return owner_users.count(user_id) > 0; }
    bool is_admin(long long user_id) const { return owner_users.count(user_id) > 0 || admin_users.count(user_id) > 0; }
    bool tool_enabled(const std::string& name) const { return tools.count(name) > 0; }
    bool is_public_command(const std::string& cmd) const { return public_commands.count(cmd) > 0; }

private:
    std::map<std::string, std::string> file_vals_;
    void load_file(const std::string& path);
    std::string get(const std::string& key, const std::string& def = "") const;
    bool get_bool(const std::string& key, bool def) const;
    std::optional<double> get_opt(const std::string& key) const;  // unset/empty -> nullopt
};

extern const char* const ALL_TOOLS[];
