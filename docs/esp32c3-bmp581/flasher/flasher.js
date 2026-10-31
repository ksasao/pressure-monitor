// pressure-monitor (esp32c3-bmp581) のファームウェア書き込みツール。
// esptool.js で、Web Serial 経由で書き込む。
//
// releases.json        リリースの一覧 (新しい順)。tools/release.mjs が更新する
// releases/<ver>/flash_config.json   そのリリースの、書き込むファイルとアドレス
import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.6.0/bundle.js";

const CHIP = "ESP32-C3";
const TOOL_VERSION = "2026-10-31b";    // この書き込みツール自体の版 (index.html の ?v= と合わせる)

const $ = (id) => document.getElementById(id);
const statusEl = $("status");
const logEl = $("log");
const barEl = $("bar");
const releaseEl = $("release");
const notesEl = $("notes");
const connectBtn = $("connectBtn");
const flashBtn = $("flashBtn");
const disconnectBtn = $("disconnectBtn");
const eraseAllEl = $("eraseAll");

let releases = [];
let transport = null;
let esploader = null;

function log(message) {
  const ts = new Date().toLocaleTimeString("ja-JP", { hour12: false });
  logEl.textContent += `\n[${ts}] ${message}`;
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(type, message) {
  statusEl.className = "status" + (type ? " " + type : "");
  statusEl.textContent = message;
}

function setButtons({ connect, flash, disconnect }) {
  connectBtn.disabled = !connect;
  flashBtn.disabled = !flash;
  disconnectBtn.disabled = !disconnect;
  eraseAllEl.disabled = !flash;
  releaseEl.disabled = releases.length === 0 || (!connect && !flash);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// 書き込みのあと、基板をリセットして、書き込んだファームウェアを起動する。
// ESP32-C3 の USB (USB-Serial-JTAG) では、RTS でリセット、DTR で BOOT (IO9) を操作する。
// esptool-js 0.6.0 の after("hard_reset") は、RTS を離すだけでリセットをかけないため、書き込み用の
// プログラムのまま止まってしまう。そこで、DTR を離した (BOOT を押していない) まま、RTS を 0.1 秒かける。
// Windows の usbser では、RTS は DTR を書いたときに送られるので、RTS のあとに DTR も書く
async function resetToApp(t) {
  await t.setDTR(false);
  await t.setRTS(true);     // リセット
  await t.setDTR(false);
  await sleep(100);
  await t.setRTS(false);    // 起動 (BOOT は押していないので、書き込んだファームウェアが動く)
  await t.setDTR(false);
  await sleep(100);
}

function selectedRelease() {
  return releases.find((r) => r.version === releaseEl.value);
}

function showNotes() {
  const r = selectedRelease();
  if (!r) { notesEl.textContent = ""; return; }
  const lines = [`バージョン ${r.version}（${r.date} リリース）`];
  if (r.notes) lines.push(r.notes);
  notesEl.textContent = lines.join("\n");
}

// ---------------------------------------------------------------- releases
async function loadReleases() {
  const res = await fetch("./releases.json", { cache: "no-store" });
  if (!res.ok) throw new Error(`releases.json の取得に失敗しました (${res.status})`);
  const json = await res.json();
  releases = Array.isArray(json.releases) ? json.releases : [];
  releaseEl.innerHTML = "";
  if (releases.length === 0) {
    releaseEl.innerHTML = "<option>リリースがありません</option>";
    return;
  }
  releases.forEach((r, i) => {
    const opt = document.createElement("option");
    opt.value = r.version;
    opt.textContent = r.version + (i === 0 ? "（最新）" : "");
    releaseEl.appendChild(opt);
  });
  // ?v=2026-10-12 で、バージョンを指定して開ける
  const want = new URLSearchParams(location.search).get("v");
  if (want && releases.some((r) => r.version === want)) releaseEl.value = want;
  showNotes();
}

async function sha256Hex(data) {
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// 選んだリリースのファイルを取得し、壊れていないか (SHA-256) を確かめる
async function fetchParts(r) {
  const base = new URL(r.path, location.href);
  const res = await fetch(new URL("flash_config.json", base), { cache: "no-store" });
  if (!res.ok) throw new Error(`flash_config.json の取得に失敗しました (${res.status})`);
  const config = await res.json();
  if (config.chipFamily !== CHIP) throw new Error(`このリリースは ${config.chipFamily} 用です`);

  const parts = [];
  for (const f of config.files) {
    const fr = await fetch(new URL(f.path, base), { cache: "no-store" });
    if (!fr.ok) throw new Error(`${f.path} の取得に失敗しました (${fr.status})`);
    const data = new Uint8Array(await fr.arrayBuffer());
    if (f.sha256) {
      const h = await sha256Hex(data);
      if (h !== f.sha256) throw new Error(`${f.path} が壊れています (SHA-256 が一致しません)`);
    }
    parts.push({ data, address: f.address });
    log(`読み込み: ${f.path} (${data.length} バイト、0x${f.address.toString(16)} に書き込む)`);
  }
  return { config, parts };
}

// ---------------------------------------------------------------- device
async function connect() {
  setButtons({});
  try {
    log("シリアルポートの選択画面を開きます。");
    const port = await navigator.serial.requestPort();
    transport = new Transport(port, true);
    esploader = new ESPLoader({
      transport,
      baudrate: 115200,
      terminal: { clean() {}, writeLine: (d) => log(d), write: (d) => log(d) },
    });
    const chip = await esploader.main("default_reset");
    log(`接続しました: ${chip}`);
    if (!String(chip).includes(CHIP)) {
      throw new Error(`${CHIP} ではありません (${chip})。この基板には書き込めません`);
    }
    setStatus("ok", `接続しました（${chip}）。「書き込み開始」を押してください。`);
    setButtons({ flash: true, disconnect: true });
  } catch (err) {
    log(`接続エラー: ${err?.message || err}`);
    setStatus("error", `接続できませんでした: ${err?.message || err}`);
    await disconnect(true);
  }
}

async function flash() {
  const r = selectedRelease();
  if (!esploader || !r) return;
  setButtons({});
  barEl.style.width = "0";
  try {
    log(`バージョン ${r.version} を書き込みます${eraseAllEl.checked ? "（全消去あり）" : ""}。`);
    const { config, parts } = await fetchParts(r);
    const total = parts.reduce((s, p) => s + p.data.length, 0);
    const done = parts.map(() => 0);

    await esploader.writeFlash({
      fileArray: parts,
      flashMode: config.flashMode || "keep",
      flashFreq: config.flashFreq || "keep",
      flashSize: config.flashSize || "keep",
      eraseAll: eraseAllEl.checked,
      compress: true,
      reportProgress: (index, written, size) => {
        done[index] = (written / size) * parts[index].data.length;
        const pct = Math.floor((done.reduce((a, b) => a + b, 0) / total) * 100);
        barEl.style.width = pct + "%";
        setStatus("", `書き込み中... ${pct}%`);
      },
    });
    barEl.style.width = "100%";
    log("書き込みが完了しました。基板をリセットして、起動します (RTS)。");
    await resetToApp(transport);
    try { await transport.disconnect(); } catch (_) {}
    transport = null;
    esploader = null;
    setStatus("ok", `バージョン ${r.version} を書き込みました。基板が再起動しない（LED が点かない）場合は、` +
                    `RESET ボタンを押すか、USB を抜き差ししてください。`);
  } catch (err) {
    log(`書き込みエラー: ${err?.message || err}`);
    setStatus("error", `書き込みに失敗しました: ${err?.message || err}`);
  } finally {
    setButtons({ connect: true });
  }
}

async function disconnect(silent = false) {
  try { if (transport) await transport.disconnect(); } catch (err) {
    if (!silent) log(`切断の警告: ${err?.message || err}`);
  }
  transport = null;
  esploader = null;
  setButtons({ connect: true });
  if (!silent) {
    log("切断しました。");
    setStatus("warn", "切断しました。書き込むときは、もう一度「接続」を押してください。");
  }
}

// ---------------------------------------------------------------- init
releaseEl.addEventListener("change", showNotes);
connectBtn.addEventListener("click", connect);
flashBtn.addEventListener("click", flash);
disconnectBtn.addEventListener("click", () => disconnect(false));
window.addEventListener("beforeunload", () => { if (transport) transport.disconnect().catch(() => {}); });

(async () => {
  log(`書き込みツール ${TOOL_VERSION}`);
  try {
    await loadReleases();
  } catch (err) {
    log(`リリースの一覧を読めません: ${err?.message || err}`);
  }
  if (!("serial" in navigator)) {
    setStatus("error", "このブラウザは Web Serial に対応していません。パソコンの Chrome または Edge で開いてください。");
    setButtons({});
    releaseEl.disabled = releases.length === 0;
  } else if (!window.isSecureContext) {
    setStatus("warn", "Web Serial は HTTPS（または localhost）でだけ使えます。");
    setButtons({});
  } else if (releases.length === 0) {
    setStatus("warn", "書き込めるリリースがありません。");
    setButtons({});
  } else {
    setStatus("ok", "準備ができました。バージョンを選んで「接続」を押してください。");
    setButtons({ connect: true });
  }
})();
