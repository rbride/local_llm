#include "llm.hpp"

#include <map>
#include <random>
#include <stdexcept>

#include "http.hpp"
#include "util.hpp"

namespace {
// What the user should see while content is still streaming in.
std::string visible_part(const std::string& raw) {
    auto open = raw.find("\074think\076");
    if (open != std::string::npos && raw.find("\074/think\076", open) == std::string::npos) return trim(raw.substr(0, open));
    return strip_think(raw);
}

// True while raw currently ends inside an unclosed inline thinking section: those content
// deltas count toward the thinking budget (Task H), not the answer cap.
bool in_think_section(const std::string& raw) {
    auto open = raw.find("\074think\076");
    return open != std::string::npos && raw.find("\074/think\076", open) == std::string::npos;
}

std::string random_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* abc = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string s = "call_";
    for (int i = 0; i < 12; ++i) s += abc[rng() % 36];
    return s;
}
}  // namespace

LlmResult llm_chat(const Config& cfg, const json& messages, const json& tools, bool thinking, int max_tokens,
                   const Sampling& sampling, const StreamHooks& hooks) {
    json payload = {
        {"model", cfg.llm_model},
        {"messages", messages},
        {"temperature", sampling.temperature},
        {"max_tokens", max_tokens},
        {"stream", true},
        {"stream_options", {{"include_usage", true}}},
    };
    // Task EG2: send top_p / presence_penalty only when the matching mode-specific
    // setting is configured; otherwise leave them out so the server default applies.
    if (sampling.top_p) payload["top_p"] = *sampling.top_p;
    if (sampling.presence_penalty) payload["presence_penalty"] = *sampling.presence_penalty;
    if (tools.is_array() && !tools.empty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
    }
    // Qwen3-style chat templates skip the thinking step when this is false.
    if (!thinking) payload["chat_template_kwargs"] = {{"enable_thinking", false}};

    LlmResult res;
    std::string raw_content, line_buf, raw_body, stream_error;
    std::map<int, ToolCall> calls;
    long long deltas = 0;

    auto handle_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!starts_with(line, "data:")) return;
        std::string data = trim(line.substr(5));
        if (data.empty() || data == "[DONE]") return;
        json j = json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        if (j.contains("error")) {
            const auto& e = j["error"];
            stream_error = e.is_object() ? e.value("message", e.dump()) : e.dump();
            return;
        }
        if (j.contains("usage") && j["usage"].is_object())
            res.tokens = j["usage"].value("completion_tokens", res.tokens);
        if (j.contains("timings") && j["timings"].is_object())
            res.tokens_per_sec = j["timings"].value("predicted_per_second", res.tokens_per_sec);
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        const auto& ch = j["choices"][0];
        if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) res.finish_reason = ch["finish_reason"];
        if (!ch.contains("delta") || !ch["delta"].is_object()) return;
        const auto& d = ch["delta"];

        bool changed = false;
        if (d.contains("content") && d["content"].is_string()) {
            raw_content += d["content"].get<std::string>();
            changed = true;
            ++deltas;
            // One streamed delta is roughly one answer token (Task EG1). Only content
            // counts here; reasoning never eats into the answer cap. Content streaming
            // inside an inline thinking section counts as thinking instead (Task H).
            if (in_think_section(raw_content)) { if (hooks.reasoning_deltas) ++(*hooks.reasoning_deltas); }
            else if (hooks.answer_deltas) ++(*hooks.answer_deltas);
        }
        if (d.contains("reasoning_content") && d["reasoning_content"].is_string()) {
            res.reasoning += d["reasoning_content"].get<std::string>();
            changed = true;
            ++deltas;
            if (hooks.reasoning_deltas) ++(*hooks.reasoning_deltas);
        }
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            for (const auto& tc : d["tool_calls"]) {
                int idx = tc.value("index", 0);
                auto& c = calls[idx];
                if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty()) c.id = tc["id"];
                if (tc.contains("function") && tc["function"].is_object()) {
                    const auto& f = tc["function"];
                    if (f.contains("name") && f["name"].is_string()) c.name += f["name"].get<std::string>();
                    if (f.contains("arguments") && f["arguments"].is_string()) c.arguments += f["arguments"].get<std::string>();
                }
            }
            ++deltas;
        }
        if (changed && hooks.on_update) hooks.on_update(visible_part(raw_content), res.reasoning.size());
    };

    HttpOptions o;
    o.method = "POST";
    o.url = cfg.llm_url;
    o.body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    o.headers = {"Content-Type: application/json", "Accept: text/event-stream"};
    if (!cfg.llm_api_key.empty()) o.headers.push_back("Authorization: Bearer " + cfg.llm_api_key);
    o.timeout = 0;                    // generation can legitimately take a long time...
    o.idle_timeout = cfg.llm_timeout; // ...but not a long time with no output at all
    o.should_cancel = hooks.should_cancel;
    o.on_data = [&](const char* data, size_t len) {
        if (raw_body.size() < 65536) raw_body.append(data, std::min(len, 65536 - raw_body.size()));
        line_buf.append(data, len);
        size_t nl;
        while ((nl = line_buf.find('\n')) != std::string::npos) {
            handle_line(line_buf.substr(0, nl));
            line_buf.erase(0, nl + 1);
        }
        return !(hooks.should_cancel && hooks.should_cancel());
    };

    HttpResponse r = http_request(o);
    if (!line_buf.empty()) handle_line(line_buf);

    if (r.aborted && hooks.should_cancel && hooks.should_cancel()) res.cancelled = true;
    else if (!r.error.empty())
        throw std::runtime_error("Can't reach the model server at " + cfg.llm_url + " (" + r.error + "). Is llama-server running?");
    if (!res.cancelled && r.status != 200) {
        std::string msg = raw_body;
        json j = json::parse(raw_body, nullptr, false);
        if (!j.is_discarded() && j.contains("error"))
            msg = j["error"].is_object() ? j["error"].value("message", j["error"].dump()) : j["error"].dump();
        throw std::runtime_error("Model server error (HTTP " + std::to_string(r.status) + "): " + utf8_head(msg, 300));
    }
    if (!stream_error.empty()) throw std::runtime_error("Model server error: " + stream_error);

    // Thinking that came back inline and got cut off: keep it as reasoning.
    auto open = raw_content.find("\074think\076");
    if (open != std::string::npos && raw_content.find("\074/think\076", open) == std::string::npos && res.reasoning.empty())
        res.reasoning = raw_content.substr(open + 7);
    res.content = strip_think(raw_content);
    res.reasoning = trim(res.reasoning);
    if (res.tokens == 0) res.tokens = deltas;

    for (auto& [idx, c] : calls) {
        if (c.name.empty()) continue;
        if (c.id.empty()) c.id = random_id();
        if (trim(c.arguments).empty()) c.arguments = "{}";
        res.tool_calls.push_back(c);
    }
    return res;
}

std::string llm_model_name(const Config& cfg) {
    std::string url = cfg.llm_url;
    auto p = url.find("/chat/completions");
    if (p == std::string::npos) return "";
    url = url.substr(0, p) + "/models";
    HttpOptions o;
    o.url = url;
    o.timeout = 5;
    if (!cfg.llm_api_key.empty()) o.headers.push_back("Authorization: Bearer " + cfg.llm_api_key);
    HttpResponse r = http_request(o);
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.contains("data") || !j["data"].is_array() || j["data"].empty()) return "";
    std::string id = j["data"][0].value("id", "");
    auto slash = id.find_last_of("/\\");
    return slash == std::string::npos ? id : id.substr(slash + 1);
}
