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
    owner_users = parse_ids(get("OWNER_USER_IDS"), nullptr);
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
    persona_reminder = parse_reminder(get("PERSONA_REMINDER", "1"), 1);
    persona_reminder_max_chars = std::stoul(get("PERSONA_REMINDER_MAX_CHARS", "600"));
    max_history = std::stoul(get("MAX_HISTORY", "20"));
    max_tokens = std::stoi(get("MAX_TOKENS", "6000"));
    max_total_tokens = std::stoi(get("MAX_TOTAL_TOKENS", "32768"));
    // THINK_BUDGET / FALLBACK_THINK_TOKENS (Task H). 0 = off; negative is treated like 0.
    think_budget = std::stoi(get("THINK_BUDGET", "0"));
    fallback_think_tokens = std::stoi(get("FALLBACK_THINK_TOKENS", "1024"));
    // RETRY_MAX_TOKENS was retired in Task EG1: the retry path runs with thinking off, so
    // its cap is now the answer cap (MAX_TOKENS / /maxtokens). Warn once if still set.
    if (!get("RETRY_MAX_TOKENS").empty())
        log("Warning: RETRY_MAX_TOKENS is deprecated and ignored; the no-thinking retry now uses the answer cap (MAX_TOKENS or /maxtokens).");
    temperature = std::stod(get("TEMPERATURE", "0.7"));
    temperature_thinking = get_opt("TEMPERATURE_THINKING");
    temperature_no_thinking = get_opt("TEMPERATURE_NO_THINKING");
    top_p_thinking = get_opt("TOP_P_THINKING");
    top_p_no_thinking = get_opt("TOP_P_NO_THINKING");
    presence_penalty_thinking = get_opt("PRESENCE_PENALTY_THINKING");
    presence_penalty_no_thinking = get_opt("PRESENCE_PENALTY_NO_THINKING");
    llm_timeout = std::stol(get("LLM_TIMEOUT", "600"));
    thinking = get_bool("THINKING", true);

    models_dir = get("MODELS_DIR", "/models");
    while (models_dir.size() > 1 && models_dir.back() == '/') models_dir.pop_back();
    model_reload_cmd = get("MODEL_RELOAD_CMD");

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
    aliases_file = get("ALIASES_FILE", "aliases.txt");
    facts_context_chars = std::stoul(get("FACTS_CONTEXT_CHARS", "3000"));
    show_tool_footer = get_bool("SHOW_TOOL_FOOTER", true);
    state_file = get("STATE_FILE", "state.json");
    log_file = get("LOG_FILE", "tgbot.log");
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

int Config::parse_reminder(const std::string& raw, int def) {
    std::string v = lower(trim(raw));
    if (v == "true" || v == "yes" || v == "on") return 1;
    if (v == "false" || v == "no" || v == "off") return 0;
    try {
        size_t used = 0;
        int n = std::stoi(v, &used);
        if (used == v.size() && n >= 0) return n;
    } catch (...) {}
    log("Warning: bad PERSONA_REMINDER value '" + utf8_head(raw, 60) + "', using " + std::to_string(def) +
        " (0 = off, 1 = every message, N = every Nth message)");
    return def;
}

std::optional<double> Config::get_opt(const std::string& key) const {
    std::string v = trim(get(key));
    if (v.empty()) return {};
    return std::stod(v);
}

bool Config::get_bool(const std::string& key, bool def) const {
    std::string v = lower(trim(get(key, def ? "1" : "0")));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
