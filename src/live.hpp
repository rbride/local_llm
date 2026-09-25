// live.hpp - shows a reply while it's being generated.
//   Private chats: Telegram's native live drafts (sendMessageDraft).
//   Groups (drafts are private-only) or if drafts fail: a real message that gets edited.
#pragma once
#include <atomic>
#include <deque>
#include <string>
#include <vector>

#include "config.hpp"
#include "telegram.hpp"

class LiveMessage {
public:
    LiveMessage(const Telegram& tg, const Config& cfg, long long chat_id, bool is_group, long long reply_to);

    bool active() const { return mode_ != Mode::Off; }
    void set_phase(const std::string& phase);      // e.g. "Thinking..." (shown while there's no text yet)
    void add_status(const std::string& line);      // e.g. "Searching: ..." (tool activity)
    void set_content(const std::string& content);  // the answer so far
    void tick(bool force = false);                 // push an update if it's time
    void finish(const std::string& final_markdown);

    static std::atomic<bool> drafts_unavailable;   // flips on the first draft failure

private:
    enum class Mode { Off, Draft, Edit };
    const Telegram& tg_;
    const Config& cfg_;
    long long chat_id_, reply_to_;
    Mode mode_;
    long long draft_id_ = 0;
    long long message_id_ = 0;  // Edit mode
    std::string phase_, content_, last_sent_;
    std::vector<std::string> status_;
    long long last_push_ = 0, blocked_until_ = 0;
    std::deque<long long> recent_;  // push timestamps, for the draft rate limit

    std::string render() const;
    bool window_ok(long long now);
    void push(const std::string& text, long long now);
};
