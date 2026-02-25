#include "Server.h"
#include "Transport.h"
#include "Protocol.h"
#include <cstdio>

namespace lsp {

// ── Capability constants ──────────────────────────────────────────────────────
static constexpr int TEXT_SYNC_FULL = 1;

// ── run ──────────────────────────────────────────────────────────────────────
void Server::run() {
    while (true) {
        auto msg = readMessage();
        if (!msg) {
            // EOF or parse error — exit cleanly
            break;
        }

        // Every JSON-RPC message has a method
        if (!msg->contains("method")) continue;
        std::string method = (*msg)["method"].get<std::string>();

        // id may be absent (notifications)
        json id = msg->contains("id") ? (*msg)["id"] : json{};
        json params = msg->contains("params") ? (*msg)["params"] : json::object();

        // ── Requests (have id) ────────────────────────────────────────────────
        if (msg->contains("id")) {
            json response;
            if (method == "initialize") {
                response = handleInitialize(id, params);
            } else if (method == "shutdown") {
                response = handleShutdown(id);
            } else if (method == "textDocument/hover") {
                response = handleHover(id, params);
            } else if (method == "textDocument/completion") {
                response = handleCompletion(id, params);
            } else if (method == "textDocument/definition") {
                response = handleDefinition(id, params);
            } else if (method == "textDocument/documentSymbol") {
                response = handleDocumentSymbols(id, params);
            } else {
                // Unknown method — return null result (not an error)
                response = makeResult(id, json{});
            }
            writeMessage(response);
        } else {
            // ── Notifications (no id) ─────────────────────────────────────────
            if (method == "initialized") {
                handleInitialized(params);
            } else if (method == "exit") {
                handleExit();
                return;
            } else if (method == "textDocument/didOpen") {
                handleDidOpen(params);
            } else if (method == "textDocument/didChange") {
                handleDidChange(params);
            } else if (method == "textDocument/didClose") {
                handleDidClose(params);
            } else if (method == "textDocument/didSave") {
                handleDidSave(params);
            }
        }

        if (shutdownRequested_ && method == "exit") return;
    }
}

// ── initialize ────────────────────────────────────────────────────────────────
json Server::handleInitialize(const json &id, const json & /*params*/) {
    json caps = {
        {"textDocumentSync", TEXT_SYNC_FULL},
        {"hoverProvider", true},
        {"completionProvider", {
            {"triggerCharacters", {".", ":"}},
            {"resolveProvider", false}
        }},
        {"definitionProvider", true},
        {"documentSymbolProvider", true}
    };
    json result = {
        {"capabilities", caps},
        {"serverInfo", {
            {"name", "sc-lsp"},
            {"version", "0.1.0"}
        }}
    };
    return makeResult(id, result);
}

void Server::handleInitialized(const json & /*params*/) {
    // Nothing to do — client confirmed initialization
}

// ── shutdown / exit ───────────────────────────────────────────────────────────
json Server::handleShutdown(const json &id) {
    shutdownRequested_ = true;
    return makeResult(id, json{});
}

void Server::handleExit() {
    // Will cause run() to return
}

// ── Document lifecycle ────────────────────────────────────────────────────────
void Server::handleDidOpen(const json &params) {
    auto &td  = params.at("textDocument");
    std::string uri  = td.at("uri").get<std::string>();
    std::string text = td.at("text").get<std::string>();
    analyzeAndPublish(uri, text);
}

void Server::handleDidChange(const json &params) {
    auto &td  = params.at("textDocument");
    std::string uri = td.at("uri").get<std::string>();
    // Full sync: take the last contentChange's text
    auto &changes = params.at("contentChanges");
    if (changes.empty()) return;
    std::string text = changes.back().at("text").get<std::string>();
    analyzeAndPublish(uri, text);
}

void Server::handleDidClose(const json &params) {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();
    docs_.erase(uri);
    // Clear diagnostics for closed file
    publishDiagnostics(uri, {});
}

void Server::handleDidSave(const json &params) {
    // text may be included; if so re-analyze
    std::string uri = params.at("textDocument").at("uri").get<std::string>();
    if (params.contains("text")) {
        std::string text = params.at("text").get<std::string>();
        analyzeAndPublish(uri, text);
    } else if (docs_.count(uri)) {
        analyzeAndPublish(uri, docs_.at(uri).text);
    }
}

// ── Language features ─────────────────────────────────────────────────────────
json Server::handleHover(const json &id, const json &params) {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();
    auto pos = Position::fromJson(params.at("position"));

    auto it = docs_.find(uri);
    if (it == docs_.end()) return makeResult(id, json{});

    auto h = lsp::hover(it->second.result, it->second.text, pos.line, pos.character);
    if (!h) return makeResult(id, json{});
    return makeResult(id, h->toJson());
}

json Server::handleCompletion(const json &id, const json &params) {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();

    auto it = docs_.find(uri);
    if (it == docs_.end()) return makeResult(id, json::array());

    auto items = lsp::complete(it->second.result);
    json arr = json::array();
    for (auto &item : items) arr.push_back(item.toJson());
    return makeResult(id, arr);
}

json Server::handleDefinition(const json &id, const json &params) {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();
    auto pos = Position::fromJson(params.at("position"));

    auto it = docs_.find(uri);
    if (it == docs_.end()) return makeResult(id, json{});

    auto loc = lsp::definition(it->second.result, it->second.text,
                               uri, pos.line, pos.character);
    if (!loc) return makeResult(id, json{});
    return makeResult(id, loc->toJson());
}

json Server::handleDocumentSymbols(const json &id, const json &params) {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();

    auto it = docs_.find(uri);
    if (it == docs_.end()) return makeResult(id, json::array());

    auto syms = lsp::documentSymbols(it->second.result);
    json arr = json::array();
    for (auto &s : syms) arr.push_back(s.toJson());
    return makeResult(id, arr);
}

// ── Helpers ───────────────────────────────────────────────────────────────────
void Server::analyzeAndPublish(const std::string &uri, const std::string &text) {
    auto &doc  = docs_[uri];
    doc.uri    = uri;
    doc.text   = text;
    doc.result = lsp::analyze(uri, text);
    publishDiagnostics(uri, doc.result.diagnostics);
}

void Server::publishDiagnostics(const std::string &uri,
                                const std::vector<Diagnostic> &diags) {
    json arr = json::array();
    for (auto &d : diags) arr.push_back(d.toJson());
    writeMessage(makeNotification("textDocument/publishDiagnostics",
                                 {{"uri", uri}, {"diagnostics", arr}}));
}

} // namespace lsp
