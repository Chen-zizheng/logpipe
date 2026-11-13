// 纯前端轮询：2 秒拿一次 /api/stats，5 秒拿一次 /api/tail。
const REFRESH_STATS_MS = 2000;
const REFRESH_TAIL_MS = 5000;

const el = (id) => document.getElementById(id);
let paused = false;

function fmtBytes(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KiB";
  if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + " MiB";
  return (n / (1024 * 1024 * 1024)).toFixed(2) + " GiB";
}

function fmtNum(n) {
  return typeof n === "number" ? n.toLocaleString("en-US") : String(n);
}

async function refreshStats() {
  try {
    const res = await fetch("/api/stats", { cache: "no-store" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    const data = await res.json();
    const m = data.metrics;

    el("m-records").textContent = fmtNum(m.records);
    el("m-written").textContent = fmtBytes(m.written_bytes);
    el("m-dropped").textContent = fmtNum(m.dropped);
    el("m-rotations").textContent = fmtNum(data.rotations);
    el("m-conns").textContent = fmtNum(m.connections_accepted - m.connections_closed);
    el("m-queue").textContent = fmtNum(data.queue_depth) + " / " + fmtNum(data.queue_capacity);
    el("sink").textContent = "sink: " + data.sink;

    const ratio = data.queue_capacity ? Math.min(1, data.queue_depth / data.queue_capacity) : 0;
    const bar = el("queue-bar");
    bar.style.width = (ratio * 100).toFixed(1) + "%";
    bar.classList.toggle("warn", ratio > 0.7);

    const status = el("status");
    const up = Math.floor(data.uptime_ms / 1000);
    status.textContent = "在线 · 端口 " + data.port + " · 已运行 " + up + "s";
    status.className = "status ok";
  } catch (err) {
    const status = el("status");
    status.textContent = "离线 (" + err.message + ")";
    status.className = "status bad";
  }
}

async function refreshTail() {
  if (paused) return;
  try {
    const res = await fetch("/api/tail?n=50", { cache: "no-store" });
    if (!res.ok) return;
    const data = await res.json();
    const box = el("tail");
    box.textContent = data.lines.length ? data.lines.join("\n") : "暂无数据";
    box.scrollTop = box.scrollHeight;
  } catch (_) {
    /* 看板刷新失败不打扰用户 */
  }
}

el("pause").addEventListener("click", () => {
  paused = !paused;
  el("pause").textContent = paused ? "继续" : "暂停";
});

refreshStats();
refreshTail();
setInterval(refreshStats, REFRESH_STATS_MS);
setInterval(refreshTail, REFRESH_TAIL_MS);
