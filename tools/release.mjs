#!/usr/bin/env node
// ファームウェアのリリース (ブラウザの書き込みツールで配布する) を作るスクリプト。
// 手順は RELEASING.md を参照してください。
//
//   node tools/release.mjs --notes "変更点"         今日の日付でリリースを作る
//   node tools/release.mjs --version 2026-10-20 ...  バージョンを指定する
//   node tools/release.mjs --skip-build ...          Arduino IDE で書き出したバイナリを使う
//   node tools/release.mjs --commit --push ...       作ったあと、コミットしてプッシュする
//   node tools/release.mjs --list                    リリースの一覧を表示する
//   node tools/release.mjs --remove 2026-10-20       リリースを一覧と docs から削除する
//
// 行うこと
//   1. version.h の FIRMWARE_VERSION を、リリースのバージョン (リリース日) にする
//   2. arduino-cli (Arduino IDE に同梱) でビルドする
//   3. バイナリが、そのバージョンを含むことを確かめる
//   4. docs/<variant>/flasher/releases/<version>/ に、バイナリと flash_config.json を置く
//   5. docs/<variant>/flasher/releases.json の先頭に、リリースを加える
// Node.js 18 以降の標準機能だけで動きます (npm install は不要です)。

import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import readline from "node:readline/promises";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

// 組み合わせ (firmware/ のフォルダ) ごとの設定
const VARIANTS = {
  "esp32c3-bmp581": {
    name: "pressure-monitor (esp32c3-bmp581)",
    sketchDir: "firmware/esp32c3-bmp581",
    sketch: "esp32c3-bmp581.ino",
    // Arduino IDE の設定と同じ: ESP32C3 Dev Module / USB CDC On Boot: Enabled /
    // Partition Scheme: Huge APP。ほかは既定のまま
    fqbn: "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app",
    chipFamily: "ESP32-C3",
    flasherDir: "docs/esp32c3-bmp581/flasher",
    // 書き込むファイル。address はビルドの flash_args から読む (読めないときは、この値)
    parts: [
      { out: "bootloader.bin",  src: (s) => `${s}.bootloader.bin`, address: 0x0 },
      { out: "partitions.bin",  src: (s) => `${s}.partitions.bin`, address: 0x8000 },
      { out: "boot_app0.bin",   src: () => "boot_app0.bin",        address: 0xe000 },
      { out: "application.bin", src: (s) => `${s}.bin`,            address: 0x10000 },
    ],
  },
};

// ---------------------------------------------------------------- utils
function die(msg) {
  console.error(`\nエラー: ${msg}`);
  process.exit(1);
}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { cwd: ROOT, encoding: "utf8", ...opts });
  if (r.error) throw r.error;
  return r;
}

function git(...args) {
  const r = run("git", args);
  if (r.status !== 0) die(`git ${args.join(" ")} に失敗しました\n${r.stderr}`);
  return r.stdout.trim();
}

function parseArgs(argv) {
  const a = { variant: "esp32c3-bmp581" };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const next = () => { if (i + 1 >= argv.length) die(`${k} に値がありません`); return argv[++i]; };
    switch (k) {
      case "--variant":     a.variant = next(); break;
      case "--version":     a.version = next(); break;
      case "--notes":       a.notes = next(); break;
      case "--notes-file":  a.notes = fs.readFileSync(next(), "utf8").trim(); break;
      case "--skip-build":  a.skipBuild = true; break;
      case "--force":       a.force = true; break;
      case "--allow-dirty": a.allowDirty = true; break;
      case "--commit":      a.commit = true; break;
      case "--push":        a.push = true; a.commit = true; break;
      case "--list":        a.list = true; break;
      case "--remove":      a.remove = next(); break;
      case "-h": case "--help": a.help = true; break;
      default: die(`不明なオプション: ${k}`);
    }
  }
  return a;
}

function today() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function sha256(buf) {
  return crypto.createHash("sha256").update(buf).digest("hex");
}

function readJson(file, fallback) {
  try { return JSON.parse(fs.readFileSync(file, "utf8")); } catch { return fallback; }
}

function writeJson(file, obj) {
  fs.writeFileSync(file, JSON.stringify(obj, null, 2) + "\n");
}

// arduino-cli を探す: 環境変数 ARDUINO_CLI -> PATH -> Arduino IDE に同梱のもの
function findArduinoCli() {
  if (process.env.ARDUINO_CLI && fs.existsSync(process.env.ARDUINO_CLI)) return process.env.ARDUINO_CLI;
  const which = run(process.platform === "win32" ? "where" : "which", ["arduino-cli"]);
  if (which.status === 0 && which.stdout.trim()) return which.stdout.trim().split(/\r?\n/)[0];
  const candidates = [
    path.join(process.env.LOCALAPPDATA || "", "Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"),
    "C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe",
    "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli",
    path.join(os.homedir(), "Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"),
  ];
  return candidates.find((p) => p && fs.existsSync(p)) || null;
}

// flash_args (ビルドが出力する、書き込むファイルとアドレスの一覧) を読む
function readFlashArgs(buildDir) {
  const file = path.join(buildDir, "flash_args");
  if (!fs.existsSync(file)) return null;
  const map = {};
  for (const line of fs.readFileSync(file, "utf8").split(/\r?\n/)) {
    const m = line.trim().match(/^(0x[0-9a-fA-F]+)\s+(\S+)$/);
    if (m) map[m[2]] = parseInt(m[1], 16);
  }
  return map;
}

// ---------------------------------------------------------------- commands
function list(v) {
  const rel = readJson(path.join(ROOT, v.flasherDir, "releases.json"), { releases: [] });
  if (!rel.releases.length) { console.log("リリースはありません。"); return; }
  for (const r of rel.releases) console.log(`${r.version}  ${r.commit || ""}  ${(r.notes || "").split("\n")[0]}`);
}

function remove(v, version) {
  const relFile = path.join(ROOT, v.flasherDir, "releases.json");
  const rel = readJson(relFile, { releases: [] });
  const before = rel.releases.length;
  rel.releases = rel.releases.filter((r) => r.version !== version);
  if (rel.releases.length === before) die(`リリース ${version} は一覧にありません`);
  writeJson(relFile, rel);
  fs.rmSync(path.join(ROOT, v.flasherDir, "releases", version), { recursive: true, force: true });
  console.log(`リリース ${version} を削除しました。コミットしてプッシュすると、公開ページからも消えます。`);
}

async function release(v, a) {
  const relDir = path.join(ROOT, v.flasherDir, "releases");
  const relFile = path.join(ROOT, v.flasherDir, "releases.json");
  const rel = readJson(relFile, { name: v.name, chipFamily: v.chipFamily, releases: [] });

  // ---- バージョン
  let version = a.version;
  if (!version) {
    version = today();
    for (let n = 2; rel.releases.some((r) => r.version === version); n++) version = `${today()}.${n}`;
  }
  if (!/^\d{4}-\d{2}-\d{2}(\.\d+)?$/.test(version)) die(`バージョンの形式が違います: ${version} (例: 2026-10-20、2026-10-20.2)`);
  if (rel.releases.some((r) => r.version === version) && !a.force) {
    die(`リリース ${version} はすでにあります。作り直すときは --force を付けてください`);
  }

  // ---- 作業ツリーの確認 (ソースの変更をコミットしてからリリースする)
  const versionH = path.join(v.sketchDir, "version.h");
  const dirty = git("status", "--porcelain", "--", v.sketchDir)
    .split("\n").filter((l) => l && !l.endsWith("version.h"));
  if (dirty.length && !a.allowDirty) {
    die(`${v.sketchDir} に、コミットしていない変更があります。\n${dirty.join("\n")}\n` +
        "コミットしてからリリースしてください (確認せずに進めるときは --allow-dirty)");
  }
  const commit = git("rev-parse", "--short", "HEAD") + (dirty.length ? "-dirty" : "");

  // ---- リリースノート
  let notes = a.notes;
  if (notes === undefined && process.stdin.isTTY) {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    notes = (await rl.question("リリースノート (変更点。空欄でも可): ")).trim();
    rl.close();
  }
  notes = notes || "";

  console.log(`\n== リリース ${version} (${v.name}, commit ${commit})`);

  // ---- 1. version.h
  const vhPath = path.join(ROOT, versionH);
  const vh = fs.readFileSync(vhPath, "utf8");
  const vhNew = vh.replace(/#define FIRMWARE_VERSION ".*"/, `#define FIRMWARE_VERSION "${version}"`);
  if (vhNew === vh && !vh.includes(`"${version}"`)) die("version.h に FIRMWARE_VERSION が見つかりません");
  fs.writeFileSync(vhPath, vhNew);
  console.log(`version.h: FIRMWARE_VERSION = "${version}"`);

  // ---- 2. ビルド
  let buildDir;
  if (a.skipBuild) {
    // Arduino IDE の「スケッチ > コンパイルしたバイナリをエクスポート」の出力を使う
    const exportRoot = path.join(ROOT, v.sketchDir, "build");
    const dirs = fs.existsSync(exportRoot) ? fs.readdirSync(exportRoot).map((d) => path.join(exportRoot, d))
      .filter((d) => fs.existsSync(path.join(d, `${v.sketch}.bin`))) : [];
    if (!dirs.length) die(`${exportRoot} に、書き出したバイナリがありません。Arduino IDE で「コンパイルしたバイナリをエクスポート」を実行してください`);
    dirs.sort((x, y) => fs.statSync(path.join(y, `${v.sketch}.bin`)).mtimeMs - fs.statSync(path.join(x, `${v.sketch}.bin`)).mtimeMs);
    buildDir = dirs[0];
    console.log(`ビルドを省略し、Arduino IDE の出力を使います: ${buildDir}`);
  } else {
    const cli = findArduinoCli();
    if (!cli) die("arduino-cli が見つかりません。Arduino IDE 2.x を入れるか、環境変数 ARDUINO_CLI にパスを設定してください\n" +
                  "(または、Arduino IDE でバイナリを書き出して --skip-build を付けてください)");
    buildDir = path.join(os.tmpdir(), "pressure-monitor-build", a.variant);
    fs.mkdirSync(buildDir, { recursive: true });
    console.log(`ビルドします (初回は 10 分ほどかかります): ${cli}`);
    const r = run(cli, ["compile", "--fqbn", v.fqbn, "--build-path", buildDir, path.join(ROOT, v.sketchDir)],
                  { stdio: "inherit" });
    if (r.status !== 0) die("ビルドに失敗しました");
  }

  // ---- 3. バイナリを集めて、バージョンを確かめる
  const flashArgs = readFlashArgs(buildDir);
  const files = v.parts.map((p) => {
    const srcName = p.src(v.sketch);
    let src = path.join(buildDir, srcName);
    if (!fs.existsSync(src) && srcName === "boot_app0.bin") {
      // IDE の書き出しに無いときは、ボードパッケージのものを使う
      const pkg = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), ".arduino15"), "Arduino15/packages/esp32/hardware/esp32");
      const ver = fs.existsSync(pkg) ? fs.readdirSync(pkg).sort().pop() : null;
      if (ver) src = path.join(pkg, ver, "tools/partitions/boot_app0.bin");
    }
    if (!fs.existsSync(src)) die(`${srcName} が見つかりません (${buildDir})`);
    const address = flashArgs && flashArgs[srcName] !== undefined ? flashArgs[srcName] : p.address;
    return { ...p, srcName, srcPath: src, address, data: fs.readFileSync(src) };
  });
  const app = files.find((f) => f.out === "application.bin");
  if (!app.data.includes(Buffer.from(version))) {
    die(`application.bin に、バージョン "${version}" が含まれていません。` +
        (a.skipBuild ? "version.h を書き換えたあとに、Arduino IDE でもう一度書き出してください" : "ビルドを確認してください"));
  }
  console.log(`バイナリにバージョン "${version}" が含まれていることを確認しました`);

  // ---- 4. docs に置く
  const outDir = path.join(relDir, version);
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });
  for (const f of files) fs.writeFileSync(path.join(outDir, f.out), f.data);
  writeJson(path.join(outDir, "flash_config.json"), {
    name: v.name,
    version,
    commit,
    chipFamily: v.chipFamily,
    fqbn: v.fqbn,
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    files: files.map((f) => ({ path: f.out, address: f.address, size: f.data.length, sha256: sha256(f.data), source: f.srcName })),
  });

  // ---- 5. 一覧 (新しい順)
  const entry = { version, date: version.slice(0, 10), commit, notes, path: `releases/${version}/` };
  rel.name = v.name;
  rel.chipFamily = v.chipFamily;
  rel.releases = [entry, ...rel.releases.filter((r) => r.version !== version)]
    .sort((x, y) => (x.version < y.version ? 1 : -1));
  writeJson(relFile, rel);

  const relPath = path.relative(ROOT, outDir).replace(/\\/g, "/");
  console.log(`\n${relPath}/ に置きました:`);
  for (const f of files) console.log(`  0x${f.address.toString(16).padStart(5, "0")}  ${f.out}  (${f.data.length} バイト)`);

  // ---- コミットとプッシュ (指定したときだけ)
  const toAdd = [v.flasherDir, versionH];
  if (a.commit) {
    git("add", ...toAdd);
    git("commit", "-m", `ファームウェア ${version} をリリース (${a.variant})`);
    console.log("\nコミットしました。");
    if (a.push) { git("push"); console.log("プッシュしました。数分後に GitHub Pages に反映されます。"); }
  }
  console.log("\n次の手順:");
  if (!a.commit) {
    console.log("  1. 手元で書き込みツールを確かめる:  node tools/serve-docs.mjs");
    console.log(`  2. コミットしてプッシュする:  git add ${toAdd.join(" ")} && git commit -m "ファームウェア ${version} をリリース" && git push`);
  } else if (!a.push) {
    console.log("  git push で公開します。");
  }
  console.log(`  公開ページ: https://ksasao.github.io/pressure-monitor/${v.flasherDir.replace(/^docs\//, "")}/`);
}

// ---------------------------------------------------------------- main
const args = parseArgs(process.argv.slice(2));
if (args.help) {
  console.log(fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").slice(1, 20).join("\n"));
  process.exit(0);
}
const variant = VARIANTS[args.variant];
if (!variant) die(`不明な組み合わせ: ${args.variant} (${Object.keys(VARIANTS).join(", ")})`);
if (args.list) list(variant);
else if (args.remove) remove(variant, args.remove);
else await release(variant, args);
