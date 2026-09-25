// tgbot.cpp - Telegram bot <-> local LLM (any OpenAI-compatible server, e.g. LM Studio)
//
// Config comes from environment variables, or from a "bot.env" file (KEY=VALUE lines)
// in the working directory. Real environment variables win over the file.
//
//   TELEGRAM_BOT_TOKEN  (required) token from @BotFather
//   ALLOWED_USER_IDS    comma-separated Telegram user IDs allowed to chat, or * for anyone.
//                       If empty, the bot tells each user their ID and refuses to chat.
//   LLM_URL             default http://127.0.0.1:1234/v1/chat/completions
//   LLM_MODEL           model identifier as shown by GET /v1/models
//   LLM_API_KEY         optional bearer token (LM Studio doesn't need one)
//   SYSTEM_PROMPT       optional system prompt
//   MAX_HISTORY         messages of history kept per chat (default 20)
//   MAX_TOKENS          max tokens per reply (default 4096)
//   TEMPERATURE         default 0.7
//   LLM_TIMEOUT         seconds to wait for the model (default 600)

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------- utilities

static std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static void log(const std::string& msg) {
    auto t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    std::cout << "[" << buf << "] " << msg << std::endl;
}

static std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Remove every "@username" from text (case-insensitive). Returns true if any were found.
static bool remove_mention(std::string& text, const std::string& username) {
    if (username.empty()) return false;
    const std::string needle = "@" + lower(username);
    bool found = false;
    for (;;) {
        auto pos = lower(text).find(needle);
        if (pos == std::string::npos) break;
        text.erase(pos, needle.size());
        found = true;
    }
    if (found) text = trim(text);
    return found;
}

// Strip <think>...</think> blocks that reasoning models (Qwen3 etc.) may emit.
static std::string strip_think(std::string s) {
    for (;;) {
        auto a = s.find("<think>");
        if (a == std::string::npos) break;
        auto b = s.find("</think>", a);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b + 8 - a);
    }
    // Some chat templates put the opening <think> in the prompt, so only </think> shows up.
    auto b = s.find("</think>");
    if (b != std::string::npos) s.erase(0, b + 8);
    return trim(s);
}

// ---------------------------------------------------------------- config

class Config {
public:
    std::string token, llm_url, llm_model, llm_api_key, system_prompt;
    std::set<long long> allowed_users;
    bool allow_everyone = false;
    size_t max_history = 20;
    int max_tokens = 4096;
    double temperature = 0.7;
    long llm_timeout = 600;

    void load() {
        load_file("bot.env");
        token         = get("TELEGRAM_BOT_TOKEN");
        llm_url       = get("LLM_URL", "http://127.0.0.1:1234/v1/chat/completions");
        llm_model     = get("LLM_MODEL", "local-model");
        llm_api_key   = get("LLM_API_KEY");
        system_prompt = get("SYSTEM_PROMPT", "You are a helpful assistant chatting over Telegram. Keep replies concise.");
        max_history   = std::stoul(get("MAX_HISTORY", "20"));
        max_tokens    = std::stoi(get("MAX_TOKENS", "4096"));
        temperature   = std::stod(get("TEMPERATURE", "0.7"));
        llm_timeout   = std::stol(get("LLM_TIMEOUT", "600"));

        std::string ids = get("ALLOWED_USER_IDS");
        if (trim(ids) == "*") allow_everyone = true;
        std::stringstream ss(ids);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty() && item != "*") allowed_users.insert(std::stoll(item));
        }
        if (token.empty()) throw std::runtime_error("TELEGRAM_BOT_TOKEN is not set (env var or bot.env)");
    }

private:
    std::map<std::string, std::string> file_vals;

    void load_file(const std::string& path) {
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
            file_vals[k] = v;
        }
        log("Loaded settings from " + path);
    }

    std::string get(const std::string& key, const std::string& def = "") {
        if (const char* e = std::getenv(key.c_str()); e && *e) return e;
        auto it = file_vals.find(key);
        return it != file_vals.end() ? it->second : def;
    }
};

// ---------------------------------------------------------------- http

struct HttpResult { long status = 0; std::string body, error; };

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

static HttpResult http_post_json(const std::string& url, const json& payload,
                                 long timeout_s, const std::string& bearer = "") {
    HttpResult r;
    CURL* c = curl_easy_init();
    if (!c) { r.error = "curl_easy_init failed"; return r; }

    // "replace" keeps us from throwing if the model ever emits invalid UTF-8
    std::string body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!bearer.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return r;
}

// ---------------------------------------------------------------- telegram

class Telegram {
public:
    explicit Telegram(const std::string& token) : base_("https://api.telegram.org/bot" + token + "/") {}

    json call(const std::string& method, const json& params, long timeout_s = 30) {
        HttpResult r = http_post_json(base_ + method, params, timeout_s);
        if (!r.error.empty()) throw std::runtime_error(method + ": " + r.error);
        json j = json::parse(r.body, nullptr, false);
        if (j.is_discarded() || !j.value("ok", false))
            throw std::runtime_error(method + " failed (HTTP " + std::to_string(r.status) + "): " + r.body.substr(0, 300));
        return j["result"];
    }

    // reply_to: message id to reply to (0 = plain message). Used in group chats.
    void send_message(long long chat_id, const std::string& text, long long reply_to = 0) {
        // Telegram caps messages at 4096 chars; split on byte count with margin,
        // preferring newlines and never cutting a UTF-8 sequence in half.
        const size_t limit = 3500;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t len = std::min(limit, text.size() - pos);
            if (pos + len < text.size()) {
                size_t nl = text.rfind('\n', pos + len);
                if (nl != std::string::npos && nl > pos + limit / 2) len = nl - pos + 1;
                while (len > 0 && (static_cast<unsigned char>(text[pos + len]) & 0xC0) == 0x80) --len;
            }
            std::string chunk = text.substr(pos, len);
            pos += len;
            if (trim(chunk).empty()) continue;
            try {
                json params = {{"chat_id", chat_id}, {"text", chunk}};
                if (reply_to)
                    params["reply_parameters"] = {{"message_id", reply_to}, {"allow_sending_without_reply", true}};
                call("sendMessage", params);
                reply_to = 0;  // only the first chunk is a reply
            } catch (const std::exception& e) {
                log(std::string("sendMessage error: ") + e.what());
            }
        }
    }

    void typing(long long chat_id) {
        try { call("sendChatAction", {{"chat_id", chat_id}, {"action", "typing"}}, 10); }
        catch (...) {}
    }

private:
    std::string base_;
};

// Keeps the "typing..." indicator alive while the model works (Telegram clears it after ~5s).
class TypingIndicator {
public:
    TypingIndicator(Telegram& tg, long long chat_id) : running_(true) {
        worker_ = std::thread([this, &tg, chat_id] {
            while (running_) {
                tg.typing(chat_id);
                for (int i = 0; i < 40 && running_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    ~TypingIndicator() { running_ = false; worker_.join(); }
private:
    std::atomic<bool> running_;
    std::thread worker_;
};

// ---------------------------------------------------------------- llm

static std::string ask_llm(const Config& cfg, const std::vector<json>& history) {
    json messages = json::array();
    if (!cfg.system_prompt.empty())
        messages.push_back({{"role", "system"}, {"content", cfg.system_prompt}});
    for (const auto& m : history) messages.push_back(m);

    json payload = {
        {"model", cfg.llm_model},
        {"messages", messages},
        {"temperature", cfg.temperature},
        {"max_tokens", cfg.max_tokens},
        {"stream", false},
    };

    HttpResult r = http_post_json(cfg.llm_url, payload, cfg.llm_timeout, cfg.llm_api_key);
    if (!r.error.empty())
        throw std::runtime_error("Can't reach the model server at " + cfg.llm_url + " (" + r.error + "). Is the LM Studio server running?");
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error("Model server returned non-JSON (HTTP " + std::to_string(r.status) + ")");
    if (j.contains("error")) {
        const auto& e = j["error"];
        throw std::runtime_error("Model server error: " + (e.is_object() ? e.value("message", e.dump()) : e.dump()));
    }
    const auto& msg = j.at("choices").at(0).at("message");
    std::string content = msg.contains("content") && msg["content"].is_string() ? msg["content"].get<std::string>() : "";
    content = strip_think(content);
    if (content.empty())
        content = "(The model returned no visible text. It may have spent all its tokens thinking; try raising MAX_TOKENS.)";
    return content;
}

// ---------------------------------------------------------------- main loop

static const char* HELP_TEXT =
    "Just send me a message and I'll pass it to the local model.\n"
    "In groups, @mention me or reply to one of my messages.\n\n"
    "/reset - forget this conversation\n"
    "/id - show your Telegram user ID\n"
    "/help - this message";

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);

    Config cfg;
    try { cfg.load(); }
    catch (const std::exception& e) { std::cerr << "Config error: " << e.what() << std::endl; return 1; }

    Telegram tg(cfg.token);
    std::string bot_username;
    long long bot_id = 0;
    try {
        json me = tg.call("getMe", json::object());
        bot_username = me.value("username", "");
        bot_id = me.value("id", 0LL);
        log("Logged in as @" + bot_username);
    } catch (const std::exception& e) {
        std::cerr << "Couldn't log in to Telegram (bad token or no internet?): " << e.what() << std::endl;
        return 1;
    }
    log("Model endpoint: " + cfg.llm_url + "  model: " + cfg.llm_model);
    if (cfg.allow_everyone)
        log("WARNING: ALLOWED_USER_IDS=* - anyone who finds this bot can use your GPU.");
    else if (cfg.allowed_users.empty())
        log("No ALLOWED_USER_IDS set. Message the bot and it will reply with your ID; add it to bot.env and restart.");

    std::map<long long, std::vector<json>> histories;
    long long offset = 0;

    for (;;) {
        json updates;
        try {
            updates = tg.call("getUpdates",
                              {{"offset", offset}, {"timeout", 30},
                               {"allowed_updates", {"message", "channel_post", "my_chat_member"}}}, 45);
        } catch (const std::exception& e) {
            log(std::string("getUpdates error: ") + e.what() + " (retrying in 5s)");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        for (const auto& u : updates) {
            offset = u.value("update_id", 0LL) + 1;

            // Log every update exactly as Telegram sent it, before any filtering.
            if (u.contains("message") || u.contains("channel_post")) {
                const auto& mm = u.contains("message") ? u["message"] : u["channel_post"];
                std::string kind = u.contains("message") ? "message" : "channel_post";
                log("RAW " + kind + " from chat " + std::to_string(mm["chat"].value("id", 0LL)) +
                    " (" + mm["chat"].value("type", "?") + ", \"" + mm["chat"].value("title", "private") + "\"): " +
                    mm.value("text", "<no text>").substr(0, 80));
            } else if (u.contains("my_chat_member")) {
                const auto& cm = u["my_chat_member"];
                log("RAW bot membership in \"" + cm["chat"].value("title", "?") + "\" (" + cm["chat"].value("type", "?") +
                    ") is now: " + cm["new_chat_member"].value("status", "?"));
            }
            if (!u.contains("message")) continue;
            const auto& m = u["message"];
            if (!m.contains("text") || !m.contains("chat")) continue;

            long long chat_id = m["chat"].value("id", 0LL);
            long long user_id = m.contains("from") ? m["from"].value("id", 0LL) : 0;
            std::string name  = m.contains("from") ? m["from"].value("first_name", "?") : "?";
            std::string text  = m["text"].get<std::string>();

            long long msg_id      = m.value("message_id", 0LL);
            std::string chat_type = m["chat"].value("type", "private");
            bool is_group = chat_type == "group" || chat_type == "supergroup";

            // Commands: first word, split into "/cmd" and an optional "@botname" target.
            std::string cmd, cmd_target;
            if (!text.empty() && text[0] == '/') {
                std::string word = text.substr(0, text.find_first_of(" \n"));
                auto at = word.find('@');
                cmd = word.substr(0, at);
                if (at != std::string::npos) cmd_target = word.substr(at + 1);
            }
            bool known_cmd = cmd == "/id" || cmd == "/start" || cmd == "/help" || cmd == "/reset";

            // In groups, only respond when addressed: @mention, a reply to one of our
            // messages, or one of our commands. Everything else is ignored.
            long long reply_to = 0;
            if (is_group) {
                if (!cmd_target.empty() && lower(cmd_target) != lower(bot_username)) continue;  // another bot's command
                bool reply_to_bot = m.contains("reply_to_message") && m["reply_to_message"].contains("from") &&
                                    m["reply_to_message"]["from"].value("id", 0LL) == bot_id;
                bool mentioned = remove_mention(text, bot_username);
                if (!(known_cmd || !cmd_target.empty() || reply_to_bot || mentioned)) continue;
                if (text.empty()) text = "Hi";
                reply_to = msg_id;
            }
            if (!known_cmd) cmd.clear();

            if (cmd == "/id") {
                tg.send_message(chat_id, "Your user ID: " + std::to_string(user_id), reply_to);
                continue;
            }

            bool allowed = cfg.allow_everyone || cfg.allowed_users.count(user_id);
            if (!allowed) {
                log("Rejected user " + std::to_string(user_id) + " (" + name + ")");
                tg.send_message(chat_id, "Not authorized. Your user ID is " + std::to_string(user_id) +
                                         " - add it to ALLOWED_USER_IDS in bot.env and restart the bot.", reply_to);
                continue;
            }

            if (cmd == "/start" || cmd == "/help") { tg.send_message(chat_id, HELP_TEXT, reply_to); continue; }
            if (cmd == "/reset") {
                histories.erase(chat_id);
                tg.send_message(chat_id, "Conversation cleared.", reply_to);
                continue;
            }

            log(name + ": " + text.substr(0, 80));
            auto& hist = histories[chat_id];
            // In groups several people share one history, so tell the model who is talking.
            hist.push_back({{"role", "user"}, {"content", is_group ? name + ": " + text : text}});

            std::string reply;
            try {
                TypingIndicator ti(tg, chat_id);
                auto t0 = std::chrono::steady_clock::now();
                reply = ask_llm(cfg, hist);
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                log("Replied in " + std::to_string(secs) + "s (" + std::to_string(reply.size()) + " bytes)");
                hist.push_back({{"role", "assistant"}, {"content", reply}});
            } catch (const std::exception& e) {
                hist.pop_back();  // don't keep a user turn that never got an answer
                log(std::string("LLM error: ") + e.what());
                reply = std::string("Error: ") + e.what();
            }

            while (hist.size() > cfg.max_history) hist.erase(hist.begin());
            // History must start with a user turn
            while (!hist.empty() && hist.front().value("role", "") != "user") hist.erase(hist.begin());

            tg.send_message(chat_id, reply, reply_to);
        }
    }
}
