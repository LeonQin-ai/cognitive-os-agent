// mock_mcp_server.js — standard MCP (Model Context Protocol) echo server.
// Implements the standard JSON-RPC 2.0 handshake:
//   initialize -> notifications/initialized -> tools/list -> tools/call
// Transports:
//   default: HTTP on 127.0.0.1:9321 (POST /mcp)  — node mock_mcp_server.js
//   --stdio: newline-delimited JSON on stdin/stdout — node mock_mcp_server.js --stdio
//   --port N: override the HTTP port
// Tools: echo (returns "echo: <args.text>"), shutdown (exits the server).
'use strict';
const USE_STDIO = process.argv.includes('--stdio');
const portArg = process.argv.indexOf('--port');
const PORT = portArg > -1 ? parseInt(process.argv[portArg + 1], 10) || 9321 : 9321;

const TOOLS = [
  {
    name: 'echo',
    description: 'Echo back the given text',
    inputSchema: {
      type: 'object',
      properties: { text: { type: 'string', description: 'text to echo' } },
      required: ['text'],
    },
  },
  {
    name: 'shutdown',
    description: 'Stop this mock server (test helper)',
    inputSchema: { type: 'object', properties: {} },
  },
];

function handle(method, params) {
  if (method === 'initialize') {
    return {
      protocolVersion: (params && params.protocolVersion) || '2024-11-05',
      capabilities: { tools: {} },
      serverInfo: { name: 'mock-mcp', version: '1.0' },
    };
  }
  if (method === 'tools/list') return { tools: TOOLS };
  if (method === 'tools/call') {
    const name = params && params.name;
    const args = (params && params.arguments) || {};
    if (name === 'echo') {
      const text = typeof args.text === 'string' ? args.text : JSON.stringify(args);
      return {
        content: [{ type: 'text', text: 'echo: ' + text }],
        isError: false,
      };
    }
    if (name === 'shutdown') {
      return { content: [{ type: 'text', text: 'bye' }], isError: false, _shutdown: true };
    }
    return { content: [{ type: 'text', text: 'unknown tool: ' + name }], isError: true };
  }
  return null; // unknown method -> JSON-RPC error
}

function processMessage(msg) {
  if (!msg || typeof msg !== 'object') return null;
  const isNotification = msg.id == null;
  if (msg.method === 'notifications/initialized') return null; // notification: no reply
  if (typeof msg.method !== 'string') return null;
  const result = handle(msg.method, msg.params);
  if (isNotification) return null;
  if (result === null) {
    return { jsonrpc: '2.0', id: msg.id, error: { code: -32601, message: 'method not found: ' + msg.method } };
  }
  return { jsonrpc: '2.0', id: msg.id, result };
}

if (USE_STDIO) {
  let buf = '';
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', (chunk) => {
    buf += chunk;
    let idx;
    while ((idx = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, idx).trim();
      buf = buf.slice(idx + 1);
      if (!line) continue;
      let msg;
      try { msg = JSON.parse(line); } catch (e) { continue; }
      const reply = processMessage(msg);
      if (reply) {
        const shutdownTool = reply.result && reply.result._shutdown;
        process.stdout.write(JSON.stringify(reply) + '\n');
        if (shutdownTool) process.exit(0);
      }
    }
  });
  process.stdin.resume();
} else {
  const http = require('http');
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let msg = {};
      try { msg = JSON.parse(body); } catch (e) { /* ignore */ }
      const reply = processMessage(msg);
      const shutdownTool = reply && reply.result && reply.result._shutdown;
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify(reply || { jsonrpc: '2.0', id: msg.id ?? null, result: {} }));
      if (shutdownTool) setTimeout(() => process.exit(0), 100);
    });
  });
  server.listen(PORT, '127.0.0.1', () => {
    console.log('mock MCP server listening on http://127.0.0.1:' + PORT + '/mcp');
  });
}
