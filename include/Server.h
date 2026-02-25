#pragma once
#include <string>
#include <unordered_map>
#include "Analysis.h"
#include "nlohmann/json.hpp"

namespace lsp {

using json = nlohmann::json;

// Per-document state
struct DocumentState {
    std::string    uri;
    std::string    text;
    AnalysisResult result;
};

// LSP dispatcher — owns all document state and handles JSON-RPC messages.
class Server {
public:
    // Run the main event loop (reads from stdin, writes to stdout).
    void run();

private:
    // ── Request / notification handlers ──────────────────────────────────────
    json handleInitialize(const json &id, const json &params);
    void handleInitialized(const json &params);
    json handleShutdown(const json &id);
    void handleExit();

    void handleDidOpen(const json &params);
    void handleDidChange(const json &params);
    void handleDidClose(const json &params);
    void handleDidSave(const json &params);

    json handleHover(const json &id, const json &params);
    json handleCompletion(const json &id, const json &params);
    json handleDefinition(const json &id, const json &params);
    json handleDocumentSymbols(const json &id, const json &params);

    // ── Helpers ───────────────────────────────────────────────────────────────
    void analyzeAndPublish(const std::string &uri, const std::string &text);
    void publishDiagnostics(const std::string &uri,
                            const std::vector<Diagnostic> &diags);

    // ── State ─────────────────────────────────────────────────────────────────
    std::unordered_map<std::string, DocumentState> docs_;
    bool shutdownRequested_ = false;
};

} // namespace lsp
