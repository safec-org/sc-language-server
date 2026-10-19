'use strict';

const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
    const config = vscode.workspace.getConfiguration('safec');
    const serverPath = config.get('server.path') || 'sc-lsp';
    const serverArgs = config.get('server.args') || [];

    const serverOptions = {
        command: serverPath,
        args: serverArgs,
        transport: TransportKind.stdio,
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'safec' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{sc,scx}'),
        },
        traceOutputChannel: vscode.window.createOutputChannel('SafeC LSP Trace'),
        initializationOptions: analysisSettings(config),
    };

    client = new LanguageClient(
        'safec',
        'SafeC Language Server',
        serverOptions,
        clientOptions
    );

    client.start();

    // initializationOptions are sent once; restart the server when they change.
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((e) => {
        if (e.affectsConfiguration('safec') && client) {
            client.stop().then(() => activate(context));
        }
    }));
}

// Settings sc-lsp reads from initialize's initializationOptions (see the
// server's ProjectConfig.h). Unset values are left out so the server's own
// defaults (safeguard's rules) apply.
function analysisSettings(config) {
    const opts = {
        includePaths: config.get('includePaths') || [],
        defines: config.get('defines') || {},
        features: config.get('features') || [],
        noDefaultFeatures: config.get('noDefaultFeatures') || false,
        provides: config.get('provides') || [],
    };
    const target = config.get('target');
    if (target) opts.target = target;
    const compat = config.get('compatPreprocessor');
    if (typeof compat === 'boolean') opts.compatPreprocessor = compat;
    return opts;
}

function deactivate() {
    if (!client) return;
    return client.stop();
}

module.exports = { activate, deactivate };
