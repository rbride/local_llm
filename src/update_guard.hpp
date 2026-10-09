// update_guard.hpp - ignore Telegram updates that were generated before this process started.
#pragma once
#include <nlohmann/json.hpp>

#include <ctime>

inline bool update_is_stale(const nlohmann::json& update, std::time_t start_time) {
    if (!update.is_object()) return false;
    auto date_of = [](const nlohmann::json& v) -> long long {
        if (!v.is_object()) return 0;
        auto it = v.find("date");
        if (it == v.end() || !it->is_number_integer()) return 0;
        return it->get<long long>();
    };
    long long date = 0;
    for (const char* key : {"message", "edited_message", "channel_post", "edited_channel_post"}) {
        auto it = update.find(key);
        if (it != update.end()) {
            date = date_of(*it);
            if (date) break;
        }
    }
    return date > 0 && date < static_cast<long long>(start_time);
}
