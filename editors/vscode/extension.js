// Starts ace-lsp and forwards Ace documents, outline, definition and hover requests
// to it over LSP. Dependency-free: the JSON-RPC framing is the few lines below.
const vscode = require("vscode");
const { spawn } = require("child_process");

let server = null;
let buffer = Buffer.alloc(0);
let nextId = 1;
const pending = new Map();

function send(message) {
  if (!server) return;
  const body = Buffer.from(JSON.stringify({ jsonrpc: "2.0", ...message }), "utf8");
  server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  server.stdin.write(body);
}

function request(method, params) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    send({ id, method, params });
  });
}

function notify(method, params) {
  send({ method, params });
}

function receive(chunk) {
  buffer = Buffer.concat([buffer, chunk]);
  for (;;) {
    const end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return;
    const match = /Content-Length: *(\d+)/i.exec(buffer.subarray(0, end).toString("ascii"));
    const length = match ? Number(match[1]) : 0;
    if (buffer.length < end + 4 + length) return;
    const message = JSON.parse(buffer.subarray(end + 4, end + 4 + length).toString("utf8"));
    buffer = buffer.subarray(end + 4 + length);
    const waiter = pending.get(message.id);
    if (!waiter) continue;
    pending.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message));
    else waiter.resolve(message.result);
  }
}

const isAce = (doc) => doc.languageId === "ace";

function opened(doc) {
  if (!isAce(doc)) return;
  notify("textDocument/didOpen", {
    textDocument: { uri: doc.uri.toString(), languageId: "ace", version: doc.version, text: doc.getText() },
  });
}

function range(r) {
  return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function symbol(s) {
  const result = new vscode.DocumentSymbol(s.name, "", s.kind - 1, range(s.range), range(s.selectionRange));
  result.children = (s.children || []).map(symbol);
  return result;
}

async function activate(context) {
  // The server ships inside the extension, as rust-analyzer's does; the
  // setting overrides it, e.g. with a freshly built lsp/ace-lsp.
  const path = vscode.workspace.getConfiguration("ace").get("lsp.path") || context.asAbsolutePath("bin/ace-lsp");
  const log = vscode.window.createOutputChannel("Ace Language Server");
  context.subscriptions.push(log);
  server = spawn(path, [], { stdio: ["pipe", "pipe", "pipe"] });
  server.on("error", (err) => {
    log.appendLine(`cannot start ${path}: ${err.message}`);
    server = null;
    for (const waiter of pending.values()) waiter.reject(err);
    pending.clear();
  });
  server.on("exit", (code) => {
    log.appendLine(`${path} exited with ${code}`);
    server = null;
    for (const waiter of pending.values()) waiter.reject(new Error("ace-lsp exited"));
    pending.clear();
  });
  server.stdout.on("data", receive);
  server.stderr.on("data", (data) => log.append(data.toString()));

  try {
    await request("initialize", { processId: process.pid, rootUri: null, capabilities: {} });
  } catch {
    return; // highlighting still works; the reason is in the output channel
  }
  notify("initialized", {});
  vscode.workspace.textDocuments.forEach(opened);

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(opened),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (!isAce(e.document)) return;
      notify("textDocument/didChange", {
        textDocument: { uri: e.document.uri.toString(), version: e.document.version },
        contentChanges: [{ text: e.document.getText() }],
      });
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (isAce(doc)) notify("textDocument/didClose", { textDocument: { uri: doc.uri.toString() } });
    }),
    vscode.languages.registerDefinitionProvider("ace", {
      async provideDefinition(doc, pos) {
        const result = await request("textDocument/definition", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: pos.line, character: pos.character },
        });
        return (result || []).map((link) => ({
          originSelectionRange: link.originSelectionRange && range(link.originSelectionRange),
          targetUri: vscode.Uri.parse(link.targetUri),
          targetRange: range(link.targetRange),
          targetSelectionRange: range(link.targetSelectionRange),
        }));
      },
    }),
    vscode.languages.registerHoverProvider("ace", {
      async provideHover(doc, pos) {
        const result = await request("textDocument/hover", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: pos.line, character: pos.character },
        });
        return result ? new vscode.Hover(new vscode.MarkdownString(result.contents.value)) : null;
      },
    }),
    vscode.languages.registerDocumentSymbolProvider("ace", {
      async provideDocumentSymbols(doc) {
        const result = await request("textDocument/documentSymbol", { textDocument: { uri: doc.uri.toString() } });
        return (result || []).map(symbol);
      },
    }),
  );
}

async function deactivate() {
  if (!server) return;
  await request("shutdown", null).catch(() => {});
  notify("exit", null);
}

module.exports = { activate, deactivate };
