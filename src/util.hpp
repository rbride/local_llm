// util.hpp - small string/time helpers shared by everything
#pragma once
#include <ctime>
#include <string>
#include <vector>

std::string trim(const std::string& s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
void log(const std::string& msg);  // thread-safe, timestamped
long long now_ms();                // monotonic milliseconds
std::string format_time(std::time_t t, bool utc, const char* fmt);

// Remove <think>...</think> blocks (and a dangling "...</think>" prefix).
std::string strip_think(std::string s);

// UTF-8 aware cutting so we never split a multi-byte character.
std::string utf8_head(const std::string& s, size_t max_bytes);
std::string utf8_tail(const std::string& s, size_t max_bytes);
// Split text into Telegram-sized chunks, preferring paragraph/line breaks.
std::vector<std::string> split_for_telegram(const std::string& text, size_t limit = 3500);

// Remove chat-template control tokens (<|im_start|>, <|eot_id|>, <tool_call>, ...) from
// untrusted text so nobody can fake conversation turns or tool calls.
std::string sanitize_untrusted(std::string s);

std::string url_encode(const std::string& s);
std::string url_decode(const std::string& s);
std::string html_escape(const std::string& s);            // & < > "
std::string html_decode_entities(const std::string& s);
std::string strip_tags(const std::string& html);           // tags removed, entities decoded, spaces collapsed
std::string html_to_text(const std::string& html, std::string* title_out = nullptr);
std::string collapse_spaces(const std::string& s);          // runs of whitespace -> single space
