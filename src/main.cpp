#include "Server.h"
#include <cstdio>
#include <cstring>

// sc-lsp: SafeC Language Server Protocol server.
// Communicates over stdin/stdout using JSON-RPC 2.0 with Content-Length framing.
//
// Usage:  sc-lsp [--log-file <path>]
//   --log-file   Write debug log to a file instead of discarding it.

int main(int argc, char *argv[]) {
    // Optional debug log file
    FILE *logFile = nullptr;
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--log-file") == 0) {
            logFile = fopen(argv[i + 1], "w");
        }
    }
    (void)logFile; // reserved for future debug logging

    // stdin/stdout must be in binary mode on Windows; harmless on POSIX.
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    lsp::Server server;
    server.run();
    return 0;
}
