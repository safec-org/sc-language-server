#pragma once
#include <string>
#include <optional>
#include "nlohmann/json.hpp"

namespace lsp {

using json = nlohmann::json;

// JSON-RPC 2.0 framing over stdin/stdout (Content-Length header).
// readMessage() blocks until a complete message arrives or EOF.
// writeMessage() writes Content-Length + CRLF + body to stdout.

std::optional<json> readMessage();
void                writeMessage(const json &msg);

// Convenience: build a result reply.
json makeResult(const json &id, json result);
// Convenience: build an error reply.
json makeError(const json &id, int code, const std::string &message);
// Convenience: build a notification.
json makeNotification(const std::string &method, json params);

} // namespace lsp
