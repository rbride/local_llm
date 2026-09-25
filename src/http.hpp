// http.hpp - thin libcurl wrapper
#pragma once
#include <functional>
#include <string>
#include <vector>

struct HttpOptions {
    std::string method = "GET";          // GET or POST
    std::string url;
    std::string body;
    std::vector<std::string> headers;    // "Name: value"
    long timeout = 30;                   // total seconds (0 = none)
    long connect_timeout = 10;
    long idle_timeout = 0;               // abort if no bytes arrive for this many seconds (0 = off)
    size_t max_bytes = 0;                // stop reading after this many bytes (0 = unlimited)
    bool follow_redirects = false;
    bool compressed = false;             // accept gzip/deflate
    std::string resolve;                 // CURLOPT_RESOLVE entry "host:port:ip" to pin DNS
    std::string user_agent = "tgbot/2.0";
    // Streaming: called for each chunk. Return false to abort the transfer.
    std::function<bool(const char* data, size_t len)> on_data;
    // Polled a few times a second; return true to abort.
    std::function<bool()> should_cancel;
};

struct HttpResponse {
    long status = 0;
    std::string body;          // empty when on_data is used
    std::string error;         // curl error, if any
    std::string content_type;
    std::string redirect_url;  // Location target when follow_redirects is off
    bool truncated = false;    // hit max_bytes
    bool aborted = false;      // on_data returned false or should_cancel fired
};

HttpResponse http_request(const HttpOptions& opts);
