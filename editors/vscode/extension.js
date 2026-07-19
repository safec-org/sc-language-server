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
    };

    client = new LanguageClient(
        'safec',
        'SafeC Language Server',
        serverOptions,
        clientOptions
    );

    client.start();
}

function deactivate() {
    if (!client) return;
    return client.stop();
}

module.exports = { activate, deactivate };
