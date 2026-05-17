// Tachyon dashboard — minimal WebSocket bridge.
//
// Spawns the C++ tachyon_stream binary, reads its NDJSON stdout line by line,
// and broadcasts each line to all connected WebSocket clients. Also serves the
// static dashboard from ./public on the same HTTP port (path /).
//
// Run:  node dashboard/server.js
// Then: open http://localhost:8080 in a browser.

const http      = require('http');
const path      = require('path');
const fs        = require('fs');
const readline  = require('readline');
const { spawn } = require('child_process');
const WebSocket = require('ws');

const PORT          = process.env.PORT          || 8080;
const STREAM_BIN    = process.env.TACHYON_STREAM
                      || path.join(__dirname, '..', 'build', 'bin', 'Release',
                                   'tachyon_stream.exe');
const DELAY_MS      = process.env.DELAY_MS      || '50';   // ~20 events/sec
const N_EVENTS      = process.env.N_EVENTS      || '0';    // 0 = infinite
const DEPTH         = process.env.DEPTH         || '8';
const PUBLIC_DIR    = path.join(__dirname, 'public');

// ---- HTTP: static file server for the dashboard UI -------------------------
const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js'  : 'application/javascript; charset=utf-8',
    '.css' : 'text/css; charset=utf-8',
};

const httpServer = http.createServer((req, res) => {
    let rel = req.url === '/' ? '/index.html' : req.url;
    // strip query string; defend against path traversal
    rel = rel.split('?')[0].replace(/\\/g, '/');
    const file = path.normalize(path.join(PUBLIC_DIR, rel));
    if (!file.startsWith(PUBLIC_DIR)) {
        res.writeHead(403); return res.end('forbidden');
    }
    fs.readFile(file, (err, data) => {
        if (err) {
            res.writeHead(404); return res.end('not found');
        }
        const ext = path.extname(file).toLowerCase();
        res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
        res.end(data);
    });
});

// ---- WebSocket: broadcast every NDJSON line as a frame ---------------------
const wss = new WebSocket.Server({ server: httpServer });
wss.on('connection', (ws) => {
    console.log('[ws] client connected — total:', wss.clients.size);
    ws.on('close', () => {
        console.log('[ws] client disconnected — total:', wss.clients.size);
    });
});

function broadcast(line) {
    for (const client of wss.clients) {
        if (client.readyState === WebSocket.OPEN) {
            client.send(line);
        }
    }
}

// ---- Engine: spawn the C++ streamer and pipe stdout -----------------------
if (!fs.existsSync(STREAM_BIN)) {
    console.error('[engine] binary not found:', STREAM_BIN);
    console.error('         build it first: cmake --build build --config Release --target tachyon_stream');
    process.exit(1);
}

console.log('[engine] spawning:', STREAM_BIN, N_EVENTS, DEPTH, DELAY_MS);
const child = spawn(STREAM_BIN, [N_EVENTS, DEPTH, DELAY_MS]);

const rl = readline.createInterface({ input: child.stdout });
rl.on('line', broadcast);

child.stderr.on('data', d => process.stderr.write('[engine] ' + d));
child.on('exit', (code) => {
    console.log('[engine] exited with code', code);
    process.exit(code ?? 1);
});

// ---- Launch ----------------------------------------------------------------
httpServer.listen(PORT, () => {
    console.log(`[http] serving dashboard on http://localhost:${PORT}`);
    console.log(`[ws]   WebSocket on ws://localhost:${PORT}`);
});

process.on('SIGINT', () => {
    console.log('\n[shutdown] terminating engine and server');
    child.kill();
    httpServer.close(() => process.exit(0));
});
