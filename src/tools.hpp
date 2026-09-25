// tools.hpp - the tools the model can call. All of them are read-only or sandboxed:
// nothing here can touch your files, run commands, or reach your local network.
#pragma once
#include <nlohmann/json.hpp>

#include <string>

#include "config.hpp"
#include "store.hpp"

using json = nlohmann::json;

// Parses DuckDuckGo's HTML results page (exposed for tests).
json parse_duckduckgo(const std::string& html, int n);

struct ToolContext {
    long long chat_id = 0;
    long long user_id = 0;
    std::string user_name;
};

class Tools {
public:
    Tools(const Config& cfg, Store& store) : cfg_(cfg), store_(store) {}

    json definitions() const;  // OpenAI-format tool list (enabled tools only)
    std::string run(const std::string& name, const std::string& arguments, const ToolContext& ctx) const;
    // Short human-readable line shown in the live message, e.g. "Searching: cheap flights".
    std::string status_line(const std::string& name, const std::string& arguments) const;

    // Exposed for tests.
    static std::string calculate(const std::string& expr);                 // throws on bad input
    static std::string roll(const std::string& notation);                  // throws on bad input
    static bool url_is_safe(const std::string& url, std::string& resolve_entry, std::string& error);

private:
    const Config& cfg_;
    Store& store_;

    std::string web_search(const json& a) const;
    std::string fetch_url(const json& a) const;
    std::string wikipedia(const json& a) const;
    std::string weather(const json& a) const;
    std::string random(const json& a) const;
    std::string datetime() const;
    std::string remember(const json& a, const ToolContext& ctx) const;
    std::string forget(const json& a, const ToolContext& ctx) const;
};
