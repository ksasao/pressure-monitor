#!/usr/bin/env node
// docs/ を、手元の Web サーバで公開する (GitHub Pages に上げる前の確認用)。
// Web Serial は HTTPS か localhost でしか使えないので、localhost で開きます。
//
//   node tools/serve-docs.mjs            http://localhost:8000/ で開く
//   node tools/serve-docs.mjs 8080       ポートを指定する
//
// 書き込みツール: http://localhost:8000/esp32c3-bmp581/flasher/
// (Markdown のページは変換しないので、そのまま表示されます)

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const DOCS = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../docs");
const PORT = Number(process.argv[2]) || 8000;
const TYPES = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8", ".json": "application/json; charset=utf-8",
  ".md": "text/plain; charset=utf-8", ".bin": "application/octet-stream",
  ".png": "image/png", ".gif": "image/gif", ".webp": "image/webp", ".svg": "image/svg+xml",
};

http.createServer((req, res) => {
  let p = decodeURIComponent(new URL(req.url, "http://localhost").pathname);
  let file = path.join(DOCS, p);
  if (!file.startsWith(DOCS)) { res.writeHead(403).end(); return; }
  if (fs.existsSync(file) && fs.statSync(file).isDirectory()) file = path.join(file, "index.html");
  if (!fs.existsSync(file)) { res.writeHead(404).end("not found"); return; }
  res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream", "Cache-Control": "no-store" });
  fs.createReadStream(file).pipe(res);
}).listen(PORT, "127.0.0.1", () => {
  console.log(`docs を公開しています: http://localhost:${PORT}/`);
  console.log(`書き込みツール:       http://localhost:${PORT}/esp32c3-bmp581/flasher/`);
  console.log("止めるときは Ctrl+C");
});
