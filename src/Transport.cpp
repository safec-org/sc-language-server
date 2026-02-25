#include "Transport.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <optional>

namespace lsp {

// ── readMessage ──────────────────────────────────────────────────────────────
// LSP framing: one or more "Key: Value\r\n" headers, then "\r\n", then body.
// The only required header is "Content-Length: N".

std::optional<json> readMessage() {
    int contentLength = -1;

    // Read headers until blank line
    while (true) {
        std::string line;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (c == '\n') break;
            if (c != '\r') line += static_cast<char>(c);
        }
        if (feof(stdin) || ferror(stdin)) return std::nullopt;

        if (line.empty()) break; // blank line separates headers from body

        // Parse "Content-Length: N"
        const char *prefix = "Content-Length: ";
        if (line.rfind(prefix, 0) == 0) {
            contentLength = std::stoi(line.substr(strlen(prefix)));
        }
    }

    if (contentLength <= 0) return std::nullopt;

    std::string body(static_cast<size_t>(contentLength), '\0');
    if (fread(body.data(), 1, static_cast<size_t>(contentLength), stdin)
            != static_cast<size_t>(contentLength)) {
        return std::nullopt;
    }

    try {
        return json::parse(body);
    } catch (...) {
        return std::nullopt;
    }
}

// ── writeMessage ─────────────────────────────────────────────────────────────

void writeMessage(const json &msg) {
    std::string body = msg.dump();
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
