// llm.hpp - streaming client for OpenAI-compatible chat servers (llama-server, LM Studio, ...)
#pragma once
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

using json = nlohmann::json;

// Reply-length math (Task EG1). The answer cap (MAX_TOKENS / ChatSettings.max_tokens)
// limits the ANSWER only; thinking has its own separate budget. Pure helpers so they
// can be unit-tested offline.

// What max_tokens to send the server. llama-server counts thinking and the answer
// together in that field, so this is a total safety ceiling, not the answer cap:
// thinking off -> answer cap + fallback_think_tokens, because some models slip into
// reasoning anyway and the bot wraps that up at FALLBACK_THINK_TOKENS (Task H);
// thinking on with a budget -> answer cap + budget; thinking on with no budget
// (unlimited) -> max_total, so nothing can run forever.
inline int server_ceiling(int answer_cap, bool thinking, int think_budget, int max_total, int fallback_think_tokens) {
    if (!thinking) return answer_cap + fallback_think_tokens;
    if (think_budget > 0) return answer_cap + think_budget;
    return max_total;
}

// Task H: has the thinking so far gone over its budget? `budget` is the effective cap:
// the per-chat think_budget when thinking is on, or FALLBACK_THINK_TOKENS when thinking
// is off but reasoning arrives anyway. 0 = no cap (never trips). Trips only when strictly
// OVER the budget, so a model that uses exactly its budget still gets to answer.
inline bool think_budget_exceeded(long long reasoning_tokens, int budget) {
    return budget > 0 && reasoning_tokens > budget;
}

// System-prompt nudge so the model usually stays under the answer cap on its own; the
// hard cutoff while streaming is just the backstop. N ~ answer_cap * 0.7 words. Returns
// empty for large caps, where there's enough room that the nudge isn't worth the tokens.
inline std::string answer_length_hint(int answer_cap) {
    if (answer_cap >= 2000) return "";
    return "Keep your reply under about " + std::to_string(answer_cap * 7 / 10) + " words.";
}

// Sampling settings for one request (Task EG2). nullopt = "not set, don't send the field".
struct Sampling {
    double temperature = 0.7;
    std::optional<double> top_p;
    std::optional<double> presence_penalty;
};

// Pick the sampling values for a request from the mode actually used. Temperature
// precedence: the resolved per-chat /temp override (-1 = unset; the caller folds the
// chat -> global layers first, Task C; the owner's global override stays part of that
// resolved value), then TEMPERATURE_THINKING / TEMPERATURE_NO_THINKING, then TEMPERATURE.
// top_p and presence_penalty are mode-specific only; unset means don't send them.
// The retry path just calls this with thinking = false. Pure, so it is unit-testable.
inline Sampling pick_sampling(const Config& cfg, bool thinking, double temp_override) {
    Sampling s;
    const std::optional<double>& mode_temp = thinking ? cfg.temperature_thinking : cfg.temperature_no_thinking;
    s.temperature = temp_override != -1.0 ? temp_override : mode_temp.value_or(cfg.temperature);
    s.top_p = thinking ? cfg.top_p_thinking : cfg.top_p_no_thinking;
    s.presence_penalty = thinking ? cfg.presence_penalty_thinking : cfg.presence_penalty_no_thinking;
    return s;
}

struct ToolCall {
    std::string id, name, arguments;  // arguments is a JSON string
};

struct LlmResult {
    std::string content;        // visible answer, \074think\076 removed
    std::string reasoning;      // thinking text, if any
    std::string finish_reason;  // "stop", "length", "tool_calls", ...
    std::vector<ToolCall> tool_calls;
    long long tokens = 0;       // generated tokens
    double tokens_per_sec = 0;
    bool cancelled = false;
};

struct StreamHooks {
    // Called as text arrives: current visible content and total reasoning bytes so far.
    std::function<void(const std::string& content, size_t reasoning_bytes)> on_update;
    std::function<bool()> should_cancel;
    // Optional: counts answer (content) deltas as they stream, so the caller can enforce a
    // bot-side answer cap (Task EG1). llama-server sends roughly one token per streamed
    // delta, so this is a good-enough running answer length. Reasoning deltas don't count.
    std::atomic<long long>* answer_deltas = nullptr;
    // Optional: counts reasoning deltas the same way (one delta ~ one token): both the
    // server's reasoning channel and inline thinking sections in the content, so the
    // caller can enforce a thinking budget (Task H). Answer deltas never count here.
    std::atomic<long long>* reasoning_deltas = nullptr;
};

// One chat completion. Throws std::runtime_error on server/network errors.
LlmResult llm_chat(const Config& cfg, const json& messages, const json& tools, bool thinking, int max_tokens,
                   const Sampling& sampling, const StreamHooks& hooks);

// Asks the server which model it has loaded (GET /v1/models). Empty on failure.
std::string llm_model_name(const Config& cfg);
