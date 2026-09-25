#include "live.hpp"

#include <random>

#include "util.hpp"

std::atomic<bool> LiveMessage::drafts_unavailable{false};

LiveMessage::LiveMessage(const Telegram& tg, const Config& cfg, long long chat_id, bool is_group, long long reply_to)
    : tg_(tg), cfg_(cfg), chat_id_(chat_id), reply_to_(reply_to) {
    if (!cfg.streaming) mode_ = Mode::Off;
    else if (!is_group && !drafts_unavailable) mode_ = Mode::Draft;
    else mode_ = Mode::Edit;
    static std::mt19937 rng{std::random_device{}()};
    draft_id_ = 1 + static_cast<long long>(rng() % 2000000000u);
}

void LiveMessage::set_phase(const std::string& phase) { phase_ = phase; }
void LiveMessage::add_status(const std::string& line) { status_.push_back(line); }
void LiveMessage::set_content(const std::string& content) { content_ = content; }

std::string LiveMessage::render() const {
    std::string out;
    for (const auto& s : status_) out += s + "\n";
    if (content_.empty()) {
        if (!phase_.empty()) out += (out.empty() ? "" : "\n") + phase_;
    } else {
        if (!out.empty()) out += "\n";
        if (mode_ == Mode::Draft) {
            // Drafts show the latest part of the text, so the newest words stay visible.
            out += content_.size() > 3300 ? "\xE2\x80\xA6" + utf8_tail(content_, 3300) : content_;
        } else {
            out += content_.size() > 3500 ? utf8_head(content_, 3500) + " \xE2\x80\xA6" : content_;
        }
    }
    out = trim(out);
    return out.empty() ? "\xE2\x80\xA6" : out;
}

// Telegram allows 20 draft/typing calls per 5 s and 40 per 30 s, per chat. Stay under both.
bool LiveMessage::window_ok(long long now) {
    while (!recent_.empty() && now - recent_.front() > 30000) recent_.pop_front();
    int last5 = 0;
    for (long long t : recent_) if (now - t <= 5000) ++last5;
    return last5 < 18 && recent_.size() < 38;
}

void LiveMessage::tick(bool force) {
    if (mode_ == Mode::Off) return;
    long long now = now_ms();
    if (now < blocked_until_) return;
    std::string text = render();
    bool changed = text != last_sent_;
    long long interval = mode_ == Mode::Draft ? cfg_.draft_interval_ms : cfg_.edit_interval_ms;
    // Drafts expire after ~30 s, so refresh them even when nothing changed.
    bool keepalive = mode_ == Mode::Draft && now - last_push_ > 10000;
    if (!changed && !keepalive) return;
    if (!force && now - last_push_ < interval) return;
    if (mode_ == Mode::Draft && !window_ok(now)) return;
    push(text, now);
}

void LiveMessage::push(const std::string& text, long long now) {
    if (mode_ == Mode::Draft) {
        TgResult r = tg_.send_draft(chat_id_, draft_id_, text, cfg_.stop_button);
        if (r.ok) {
            last_sent_ = text;
            last_push_ = now;
            recent_.push_back(now);
        } else if (r.code == 429) {
            blocked_until_ = now + std::max(r.retry_after, 1) * 1000LL;
        } else {
            if (!drafts_unavailable.exchange(true))
                log("Live drafts unavailable (" + r.description + "); streaming by editing messages instead");
            mode_ = Mode::Edit;
        }
        return;
    }
    // Edit mode
    TgResult r;
    if (message_id_ == 0) {
        json params = {{"chat_id", chat_id_}, {"text", text}, {"link_preview_options", {{"is_disabled", true}}}};
        if (reply_to_) params["reply_parameters"] = {{"message_id", reply_to_}, {"allow_sending_without_reply", true}};
        r = tg_.request("sendMessage", params, 15);
        if (r.ok && r.result.is_object()) message_id_ = r.result.value("message_id", 0LL);
    } else {
        r = tg_.edit_plain(chat_id_, message_id_, text);
    }
    if (r.ok) {
        last_sent_ = text;
        last_push_ = now;
    } else if (r.code == 429) {
        blocked_until_ = now + std::max(r.retry_after, 1) * 1000LL;
    } else {
        log("Live update failed (" + r.description + "); will send the reply when it's done");
        mode_ = Mode::Off;
    }
}

void LiveMessage::finish(const std::string& final_markdown) {
    std::string text = trim(final_markdown).empty() ? "\xE2\x80\xA6" : final_markdown;
    if (message_id_) {
        auto chunks = split_for_telegram(text);
        if (chunks.empty()) chunks.push_back(text);
        if (!tg_.edit_markdown(chat_id_, message_id_, chunks[0])) tg_.send_markdown(chat_id_, chunks[0], reply_to_);
        for (size_t k = 1; k < chunks.size(); ++k) tg_.send_markdown(chat_id_, chunks[k]);
        return;
    }
    // A normal message replaces any live draft in the chat.
    tg_.send_markdown(chat_id_, text, reply_to_);
}
