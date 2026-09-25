#include "telegram.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "http.hpp"
#include "markdown.hpp"
#include "util.hpp"

Telegram::Telegram(const std::string& api_base, const std::string& token) : base_(api_base + "/bot" + token + "/") {}

TgResult Telegram::request(const std::string& method, const json& params, long timeout_s) const {
    HttpOptions o;
    o.method = "POST";
    o.url = base_ + method;
    o.body = params.dump(-1, ' ', false, json::error_handler_t::replace);
    o.headers = {"Content-Type: application/json"};
    o.timeout = timeout_s;
    HttpResponse r = http_request(o);

    TgResult t;
    if (!r.error.empty()) { t.description = r.error; return t; }
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) { t.code = static_cast<int>(r.status); t.description = "non-JSON reply"; return t; }
    t.ok = j.value("ok", false);
    t.code = j.value("error_code", t.ok ? 0 : static_cast<int>(r.status));
    t.description = j.value("description", "");
    if (j.contains("parameters") && j["parameters"].is_object())
        t.retry_after = j["parameters"].value("retry_after", 0);
    if (j.contains("result")) t.result = j["result"];
    return t;
}

TgResult Telegram::request_retry(const std::string& method, const json& params, long timeout_s) const {
    TgResult r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = request(method, params, timeout_s);
        if (r.ok || r.code != 429) return r;
        int wait = std::min(std::max(r.retry_after, 1), 30);
        log(method + " rate-limited, waiting " + std::to_string(wait) + "s");
        std::this_thread::sleep_for(std::chrono::seconds(wait));
    }
    return r;
}

json Telegram::call(const std::string& method, const json& params, long timeout_s) const {
    TgResult r = request(method, params, timeout_s);
    if (!r.ok) throw std::runtime_error(method + " failed (" + std::to_string(r.code) + "): " + r.description);
    return r.result;
}

TgResult Telegram::send_one(long long chat_id, const std::string& md_chunk, long long reply_to) const {
    json params = {{"chat_id", chat_id},
                   {"text", markdown_to_html(md_chunk)},
                   {"parse_mode", "HTML"},
                   {"link_preview_options", {{"is_disabled", true}}}};
    if (reply_to) params["reply_parameters"] = {{"message_id", reply_to}, {"allow_sending_without_reply", true}};
    TgResult r = request_retry("sendMessage", params);
    if (!r.ok && r.code == 400) {  // HTML rejected: send it as plain text instead
        params["text"] = md_chunk;
        params.erase("parse_mode");
        r = request_retry("sendMessage", params);
    }
    if (!r.ok) log("sendMessage failed: " + r.description);
    return r;
}

long long Telegram::send_markdown(long long chat_id, const std::string& md, long long reply_to) const {
    long long first = 0;
    for (const auto& chunk : split_for_telegram(md)) {
        TgResult r = send_one(chat_id, chunk, first ? 0 : reply_to);
        if (r.ok && !first && r.result.is_object()) first = r.result.value("message_id", 0LL);
    }
    return first;
}

long long Telegram::send_plain(long long chat_id, const std::string& text, long long reply_to) const {
    long long first = 0;
    for (const auto& chunk : split_for_telegram(text)) {
        json params = {{"chat_id", chat_id}, {"text", chunk}, {"link_preview_options", {{"is_disabled", true}}}};
        if (reply_to && !first) params["reply_parameters"] = {{"message_id", reply_to}, {"allow_sending_without_reply", true}};
        TgResult r = request_retry("sendMessage", params);
        if (!r.ok) log("sendMessage failed: " + r.description);
        else if (!first && r.result.is_object()) first = r.result.value("message_id", 0LL);
    }
    return first;
}

bool Telegram::edit_markdown(long long chat_id, long long message_id, const std::string& md) const {
    json params = {{"chat_id", chat_id},
                   {"message_id", message_id},
                   {"text", markdown_to_html(md)},
                   {"parse_mode", "HTML"},
                   {"link_preview_options", {{"is_disabled", true}}}};
    TgResult r = request_retry("editMessageText", params);
    if (r.ok || r.description.find("not modified") != std::string::npos) return true;
    if (r.code == 400) {
        params["text"] = md;
        params.erase("parse_mode");
        r = request_retry("editMessageText", params);
        if (r.ok || r.description.find("not modified") != std::string::npos) return true;
    }
    log("editMessageText failed: " + r.description);
    return false;
}

TgResult Telegram::edit_plain(long long chat_id, long long message_id, const std::string& text) const {
    json params = {{"chat_id", chat_id}, {"message_id", message_id}, {"text", text},
                   {"link_preview_options", {{"is_disabled", true}}}};
    TgResult r = request("editMessageText", params, 15);
    if (!r.ok && r.description.find("not modified") != std::string::npos) r.ok = true;
    return r;
}

TgResult Telegram::send_draft(long long chat_id, long long draft_id, const std::string& text, bool stop_button) const {
    json params = {{"chat_id", chat_id}, {"draft_id", draft_id}, {"text", text}};
    if (stop_button) params["can_stop"] = true;
    return request("sendMessageDraft", params, 15);
}

void Telegram::chat_action(long long chat_id, const std::string& action) const {
    request("sendChatAction", {{"chat_id", chat_id}, {"action", action}}, 10);
}
