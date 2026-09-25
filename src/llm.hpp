// llm.hpp - streaming client for OpenAI-compatible chat servers (llama-server, LM Studio, ...)
#pragma once
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

#include "config.hpp"

using json = nlohmann::json;

struct ToolCall {
    std::string id, name, arguments;  // arguments is a JSON string
};

struct LlmResult {
    std::string content;        // visible answer, <think> removed
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
};

// One chat completion. Throws std::runtime_error on server/network errors.
LlmResult llm_chat(const Config& cfg, const json& messages, const json& tools, bool thinking, int max_tokens,
                   const StreamHooks& hooks);

// Asks the server which model it has loaded (GET /v1/models). Empty on failure.
std::string llm_model_name(const Config& cfg);
