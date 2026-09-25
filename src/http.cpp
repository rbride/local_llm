#include "http.hpp"

#include <curl/curl.h>

namespace {
struct Ctx {
    const HttpOptions* opts;
    HttpResponse* resp;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<Ctx*>(userdata);
    size_t n = size * nmemb;
    if (ctx->opts->on_data) {
        if (!ctx->opts->on_data(ptr, n)) { ctx->resp->aborted = true; return 0; }
        return n;
    }
    ctx->resp->body.append(ptr, n);
    if (ctx->opts->max_bytes && ctx->resp->body.size() > ctx->opts->max_bytes) {
        ctx->resp->body.resize(ctx->opts->max_bytes);
        ctx->resp->truncated = true;
        return 0;  // stop reading; we keep what we have
    }
    return n;
}

int progress_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->opts->should_cancel && ctx->opts->should_cancel()) { ctx->resp->aborted = true; return 1; }
    return 0;
}
}  // namespace

HttpResponse http_request(const HttpOptions& o) {
    HttpResponse r;
    CURL* c = curl_easy_init();
    if (!c) { r.error = "curl_easy_init failed"; return r; }
    Ctx ctx{&o, &r};

    curl_slist* headers = nullptr;
    for (const auto& h : o.headers) headers = curl_slist_append(headers, h.c_str());
    curl_slist* resolve = nullptr;
    if (!o.resolve.empty()) resolve = curl_slist_append(nullptr, o.resolve.c_str());

    curl_easy_setopt(c, CURLOPT_URL, o.url.c_str());
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (resolve) curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
    if (o.method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, o.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(o.body.size()));
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, o.connect_timeout);
    if (o.timeout > 0) curl_easy_setopt(c, CURLOPT_TIMEOUT, o.timeout);
    if (o.idle_timeout > 0) {
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, o.idle_timeout);
    }
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, o.follow_redirects ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if (!o.user_agent.empty()) curl_easy_setopt(c, CURLOPT_USERAGENT, o.user_agent.c_str());
    if (o.compressed) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (o.should_cancel) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    }

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK && !r.truncated && !r.aborted) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    char* ct = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) r.content_type = ct;
    char* redir = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &redir) == CURLE_OK && redir) r.redirect_url = redir;

    curl_slist_free_all(headers);
    curl_slist_free_all(resolve);
    curl_easy_cleanup(c);
    return r;
}
