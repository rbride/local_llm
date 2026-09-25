// telegram.hpp - Telegram Bot API client
#pragma once
#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

struct TgResult {
    bool ok = false;
    int code = 0;              // Telegram error_code (or 0 for network errors)
    std::string description;
    int retry_after = 0;       // seconds, on 429
    json result;
};

class Telegram {
public:
    Telegram(const std::string& api_base, const std::string& token);

    // Raw call; never throws.
    TgResult request(const std::string& method, const json& params, long timeout_s = 30) const;
    // Same, but sleeps and retries (up to twice) on 429 Too Many Requests.
    TgResult request_retry(const std::string& method, const json& params, long timeout_s = 30) const;
    // Throws on failure and returns "result".
    json call(const std::string& method, const json& params, long timeout_s = 30) const;

    // Send Markdown-ish text as Telegram HTML (falls back to plain text if Telegram rejects it).
    // Long text is split. Returns the id of the first message sent (0 on failure).
    long long send_markdown(long long chat_id, const std::string& md, long long reply_to = 0) const;
    // Plain text, split as needed.
    long long send_plain(long long chat_id, const std::string& text, long long reply_to = 0) const;
    // Edit a message to new Markdown content (single chunk). Returns false if it couldn't.
    bool edit_markdown(long long chat_id, long long message_id, const std::string& md) const;
    TgResult edit_plain(long long chat_id, long long message_id, const std::string& text) const;
    TgResult send_draft(long long chat_id, long long draft_id, const std::string& text, bool stop_button) const;
    void chat_action(long long chat_id, const std::string& action = "typing") const;

private:
    std::string base_;
    TgResult send_one(long long chat_id, const std::string& md_chunk, long long reply_to) const;
};
