#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "util.hpp"

const char* const ALL_TOOLS[] = {"web_search", "fetch_url", "wikipedia", "weather", "calculator",
                                 "roll_dice", "random", "get_datetime", "look_up_facts", nullptr};

static std::set<long long> parse_ids(const std::string& s, bool* star) {
    std::set<long long> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        if (item == "*") { if (star) *star = true; continue; }
        try { out.insert(std::stoll(item)); }
        catch (...) { throw std::runtime_error("Bad user ID in config: '" + item + "'"); }
    }
    return out;
}

void Config::load() {
    load_file("bot.env");
    token = get("TELEGRAM_BOT_TOKEN");
    if (token.empty()) throw std::runtime_error("TELEGRAM_BOT_TOKEN is not set (env var or bot.env)");

    telegram_api = get("TELEGRAM_API_URL", "https://api.telegram.org");
    while (!telegram_api.empty() && telegram_api.back() == '/') telegram_api.pop_back();
    allowed_users = parse_ids(get("ALLOWED_USER_IDS"), &allow_everyone);
    admin_users = parse_ids(get("ADMIN_USER_IDS"), nullptr);
    trusted_chats = parse_ids(get("TRUSTED_CHAT_IDS"), nullptr);
    allow_group_members = get_bool("ALLOW_GROUP_MEMBERS", true);
    leave_untrusted_groups = get_bool("LEAVE_UNTRUSTED_GROUPS", true);
    {
        public_commands.clear();
        std::stringstream ss(lower(get("PUBLIC_COMMANDS", "help,start,retry,stop,stats,id,tools")));
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) public_commands.insert(item[0] == '/' ? item : "/" + item);
        }
    }
    rate_limit_per_min = std::stoi(get("RATE_LIMIT_PER_MIN", "8"));
    max_queue = std::stoi(get("MAX_QUEUE", "20"));

    llm_url = get("LLM_URL", "http://127.0.0.1:1234/v1/chat/completions");
    llm_model = get("LLM_MODEL", "local-model");
    llm_api_key = get("LLM_API_KEY");
    system_prompt = get("SYSTEM_PROMPT", "You are a helpful, funny assistant chatting over Telegram. Keep replies concise.");
    persona_reminder = get_bool("PERSONA_REMINDER", true);
    max_history = std::stoul(get("MAX_HISTORY", "20"));
    max_tokens = std::stoi(get("MAX_TOKENS", "6000"));
    retry_max_tokens = std::stoi(get("RETRY_MAX_TOKENS", "2048"));
    temperature = std::stod(get("TEMPERATURE", "0.7"));
    llm_timeout = std::stol(get("LLM_TIMEOUT", "600"));
    thinking = get_bool("THINKING", true);

    streaming = get_bool("STREAMING", true);
    draft_interval_ms = std::stol(get("DRAFT_INTERVAL_MS", "900"));
    edit_interval_ms = std::stol(get("EDIT_INTERVAL_MS", "3500"));
    stop_button = get_bool("STOP_BUTTON", true);

    std::string t = lower(trim(get("TOOLS", "all")));
    tools.clear();
    if (t == "all") {
        for (int i = 0; ALL_TOOLS[i]; ++i) tools.insert(ALL_TOOLS[i]);
    } else if (t != "none" && !t.empty()) {
        std::stringstream ss(t);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (item == "remember" || item == "forget") {
                log("Note: the remember/forget tools were replaced by facts.txt; ignoring '" + item + "' in TOOLS");
                continue;
            }
            bool known = false;
            for (int i = 0; ALL_TOOLS[i]; ++i) known |= item == ALL_TOOLS[i];
            if (!known) throw std::runtime_error("Unknown tool in TOOLS: '" + item + "'");
            tools.insert(item);
        }
    }
    max_tool_rounds = std::stoi(get("MAX_TOOL_ROUNDS", "5"));
    searxng_url = get("SEARXNG_URL");
    while (!searxng_url.empty() && searxng_url.back() == '/') searxng_url.pop_back();
    brave_api_key = get("BRAVE_API_KEY");
    search_results = std::stoi(get("SEARCH_RESULTS", "5"));
    fetch_max_chars = std::stoul(get("FETCH_MAX_CHARS", "8000"));
    weather_units = lower(get("WEATHER_UNITS", "metric"));
    facts_file = get("FACTS_FILE", "facts.txt");
    facts_context_chars = std::stoul(get("FACTS_CONTEXT_CHARS", "3000"));
    show_tool_footer = get_bool("SHOW_TOOL_FOOTER", true);
    state_file = get("STATE_FILE", "state.json");
    git_repo_dir = get("GIT_REPO_DIR");
    git_remote = get("GIT_REMOTE", "origin");
    git_branch = get("GIT_BRANCH", "main");
    build_command = get("BUILD_COMMAND", "make");
}

void Config::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        if (file_vals_.count(k)) log("Note: " + k + " appears more than once in " + path + "; using the last one");
        file_vals_[k] = v;
    }
    log("Loaded settings from " + path);
}

std::string Config::get(const std::string& key, const std::string& def) const {
    if (const char* e = std::getenv(key.c_str()); e && *e) return e;
    auto it = file_vals_.find(key);
    return it != file_vals_.end() ? it->second : def;
}

bool Config::get_bool(const std::string& key, bool def) const {
    std::string v = lower(trim(get(key, def ? "1" : "0")));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
