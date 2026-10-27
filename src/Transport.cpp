#include "Transport.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <optional>
#include <cctype>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace lsp {

// ── readMessage ──────────────────────────────────────────────────────────────
// LSP framing: one or more "Key: Value\r\n" headers, then "\r\n", then body.
// The only required header is "Content-Length: N".
//
// stdin is read through a buffer of our own (not stdio's), so
// inputPending() can tell whether another message is already waiting —
// the server uses that to skip analyses a newer edit has superseded.

namespace {
std::string g_in;        // bytes read from stdin, not yet consumed
size_t      g_inPos = 0;
bool        g_eof   = false;

bool fill() {
    if (g_eof) return false;
    if (g_inPos > 0 && g_inPos == g_in.size()) { g_in.clear(); g_inPos = 0; }
    char buf[65536];
#ifdef _WIN32
    int n = _read(0, buf, sizeof buf);
#else
    ssize_t n;
    do { n = read(0, buf, sizeof buf); } while (n < 0 && errno == EINTR);
#endif
    if (n <= 0) { g_eof = true; return false; }
    g_in.append(buf, (size_t)n);
    return true;
}

bool readLine(std::string &line) {
    line.clear();
    while (true) {
        size_t nl = g_in.find('\n', g_inPos);
        if (nl != std::string::npos) {
            line.assign(g_in, g_inPos, nl - g_inPos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            g_inPos = nl + 1;
            return true;
        }
        if (!fill()) return false;
    }
}
} // namespace

bool inputPending() {
    if (g_inPos < g_in.size()) return true;
    if (g_eof) return false;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (GetFileType(h) == FILE_TYPE_PIPE)
        return PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0;
    return false;
#else
    struct pollfd pfd = {0, POLLIN, 0};
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP));
#endif
}

std::optional<json> readMessage() {
    long contentLength = -1;

    // Read headers until blank line
    std::string line;
    while (true) {
        if (!readLine(line)) return std::nullopt;
        if (line.empty()) {
            if (contentLength < 0) continue;   // stray blank line before headers
            break;                             // blank line separates headers from body
        }
        // "Content-Length: N" (header names are case-insensitive)
        static const char prefix[] = "content-length:";
        if (line.size() > sizeof(prefix) - 1) {
            std::string key = line.substr(0, sizeof(prefix) - 1);
            for (auto &c : key) c = (char)std::tolower((unsigned char)c);
            if (key == prefix) contentLength = std::strtol(line.c_str() + sizeof(prefix) - 1, nullptr, 10);
        }
    }

    if (contentLength <= 0) return json::object();
    while (g_in.size() - g_inPos < (size_t)contentLength)
        if (!fill()) return std::nullopt;
    std::string body = g_in.substr(g_inPos, (size_t)contentLength);
    g_inPos += (size_t)contentLength;

    // A message that isn't JSON is skipped (an object without "method"),
    // not taken as the end of input.
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    return j.is_discarded() ? json::object() : j;
}

// ── writeMessage ─────────────────────────────────────────────────────────────

void writeMessage(const json &msg) {
    // Invalid UTF-8 (a Latin-1 source quoted in a diagnostic, say) becomes
    // U+FFFD rather than an exception that would take the server down.
    std::string body = msg.dump(-1, ' ', false, json::error_handler_t::replace);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    fflush(stdout);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

json makeResult(const json &id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json makeError(const json &id, int code, const std::string &message) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", message}}}};
}

json makeNotification(const std::string &method, json params) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

} // namespace lsp
