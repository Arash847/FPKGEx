/* fPKGEx web UI */
"use strict";

const $ = (id) => document.getElementById(id);

function fmtBytes(n) {
  if (!n && n !== 0) return "-";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = n, i = 0;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return (i === 0 ? v : v.toFixed(v < 10 ? 2 : 1)) + " " + units[i];
}

async function api(path, opts) {
  const res = await fetch(path, opts || {});
  const text = await res.text();
  let data = null;
  try { data = JSON.parse(text); } catch (_) { /* non-JSON error page */ }
  if (!res.ok || (data && data.ok === false)) {
    const msg = (data && data.error) || ("HTTP " + res.status);
    throw new Error(msg);
  }
  return data;
}

function setMsg(text, kind) {
  const el = $("msg");
  el.textContent = text || "";
  el.className = "msg" + (kind ? " " + kind : "");
}

/* ---------------------------------------------------------------- status */

async function refreshStatus() {
  try {
    const s = await api("/api/status");
    $("version").textContent = "v" + s.version + " · " + s.ip + ":" + s.port;
    $("conn").textContent = "online";
    $("conn").className = "status online";
    $("foot").textContent = "fPKGEx " + s.version + " · pid " + s.pid +
      " · up " + s.uptime + "s · all work runs on the console";
    if (s.packageOpen) loadPackage(s.packagePath || null);
  } catch (e) {
    $("version").textContent = "offline";
    $("conn").textContent = "offline";
    $("conn").className = "status";
  }
}

/* --------------------------------------------------------------- volumes */

async function loadVolumes() {
  const sel = $("volumes");
  sel.innerHTML = "";
  try {
    const v = await api("/api/volumes");
    if (!v.volumes.length) {
      sel.innerHTML = '<option value="">no writable storage found</option>';
      return;
    }
    for (const vol of v.volumes) {
      const o = document.createElement("option");
      o.value = vol.path;
      o.textContent = vol.path + "  (" + fmtBytes(vol.available) + " free)";
      sel.appendChild(o);
    }
    const current = $("curpath").value;
    const match = v.volumes.find((x) => current.startsWith(x.path));
    if (match) sel.value = match.path;
    renderVolumeHint(v.volumes);
  } catch (e) {
    sel.innerHTML = '<option value="">(volumes unavailable)</option>';
  }
}

function renderVolumeHint(volumes) {
  const pick = $("volumes").value;
  const vol = volumes.find((v) => v.path === pick);
  $("volhint").textContent = vol
    ? "Free space on " + vol.path + ": " + fmtBytes(vol.available) + " of " + fmtBytes(vol.total)
    : "";
  if (vol && $("dest").value.indexOf("/mnt/") === 0) {
    const base = $("dest").value.split("/").slice(0, 3).join("/");
    if (volumes.some((v) => v.path === base)) return;
  }
  if (vol) $("dest").placeholder = vol.path + "/Extracted";
}

/* ---------------------------------------------------------------- browse */

async function browse(path) {
  if (!path) return;
  $("curpath").value = path;
  const ul = $("entries");
  ul.innerHTML = '<li class="empty">loading…</li>';
  try {
    const data = await api("/api/browse?path=" + encodeURIComponent(path));
    ul.innerHTML = "";
    if (!data.entries.length) {
      ul.innerHTML = '<li class="empty">folder is empty</li>';
      return;
    }
    for (const e of data.entries) {
      const li = document.createElement("li");
      const name = document.createElement("span");
      name.textContent = (e.dir ? "📁 " : e.package ? "📦 " : "📄 ") + e.name;
      const sz = document.createElement("span");
      sz.className = "sz";
      sz.textContent = e.dir ? "" : fmtBytes(e.size);
      li.appendChild(name);
      li.appendChild(sz);
      if (e.package) li.classList.add("pkg");
      li.onclick = () => {
        if (e.dir) { browse(join(path, e.name)); return; }
        $("manual").value = join(path, e.name);
        openPackage(join(path, e.name));
      };
      ul.appendChild(li);
    }
  } catch (e) {
    ul.innerHTML = "";
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = e.message;
    ul.appendChild(li);
  }
}

function join(a, b) {
  return a.endsWith("/") ? a + b : a + "/" + b;
}

function goUp() {
  const p = $("curpath").value.replace(/\/+$/, "");
  const parent = p.slice(0, p.lastIndexOf("/")) || "/";
  browse(parent);
}

/* ----------------------------------------------------------------- open */

async function openPackage(path) {
  setMsg("Reading package…");
  try {
    const info = await api("/api/open", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: path }),
    });
    showPackage(info);
    setMsg("Package identified.", "ok");
  } catch (e) {
    setMsg(e.message, "err");
  }
}

function showPackage(p) {
  $("picker").classList.add("hidden");
  $("info").classList.remove("hidden");
  $("extract").classList.remove("hidden");
  $("files").classList.remove("hidden");

  $("titleName").textContent = p.titleName || "(unnamed)";
  $("version2").textContent = p.version || "-";
  $("titleId").textContent = p.titleId || "-";
  $("contentId").textContent = p.contentId || "-";
  $("drm").textContent = p.drm || "-";
  $("fileSize").textContent = fmtBytes(p.fileSize);
  $("fileCount").textContent = p.fileCount + " files";
  $("extractBytes").textContent = fmtBytes(p.extractBytes);

  setArt("pic0", p, "pic0.png");
  setArt("icon0", p, "icon0.png");

  if (p.hasOpaqueVolume) {
    const w = $("opaque");
    w.classList.remove("hidden");
    w.innerHTML =
      "<strong>Embedded application volume not included.</strong> This package " +
      "carries " + fmtBytes(p.opaqueBytes) + " of game data in a builder-specific " +
      "container that no standard tool — including Sony's own publishing tools — " +
      "can read. The " + fmtBytes(p.extractBytes) + " listed below are everything " +
      "the package exposes: <span class='mono'>sce_sys</span> and " +
      "<span class='mono'>sce_suppl</span>.";
  } else {
    $("opaque").classList.add("hidden");
  }

  const ul = $("filelist");
  ul.innerHTML = "";
  for (const f of p.files) {
    const li = document.createElement("li");
    const a = document.createElement("span");
    a.className = "mono";
    a.textContent = f.path;
    const b = document.createElement("span");
    b.className = "sz";
    b.textContent = fmtBytes(f.size);
    li.appendChild(a);
    li.appendChild(b);
    ul.appendChild(li);
  }

  if (!$("dest").value) {
    const vol = $("volumes").value;
    $("dest").placeholder = (vol || "/mnt/usb0") + "/Extracted";
  }
}

function setArt(id, pkg, name) {
  const img = $(id);
  const fb = $(id + "fallback");
  const present = (pkg.art || []).some((a) => a.name === name);
  if (!present) {
    img.classList.add("hidden");
    fb.classList.remove("hidden");
    return;
  }
  img.classList.remove("hidden");
  fb.classList.add("hidden");
  img.src = "/api/art/" + name + "?t=" + Date.now();
  img.onerror = () => { img.classList.add("hidden"); fb.classList.remove("hidden"); };
}

/* --------------------------------------------------------------- extract */

async function startExtract() {
  const path = $("manual").value.trim();
  const dest = $("dest").value.trim();
  if (!path) { setMsg("Choose a package first.", "err"); return; }
  if (!dest) { setMsg("Set an extraction location.", "err"); return; }

  $("go").disabled = true;
  $("progress").classList.remove("hidden");
  $("cancel").classList.remove("hidden");
  setMsg("Starting…");
  try {
    await api("/api/extract", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: path, dest: dest }),
    });
    pollJob();
  } catch (e) {
    setMsg(e.message, "err");
    $("go").disabled = false;
    $("cancel").classList.add("hidden");
  }
}

async function pollJob() {
  let job;
  try {
    job = await api("/api/job");
  } catch (e) {
    setTimeout(pollJob, 1200);
    return;
  }

  $("fill").style.width = job.percent + "%";
  $("pstate").textContent = job.state;
  $("pfile").textContent = job.currentFile || "";
  $("pbytes").textContent = fmtBytes(job.doneBytes) + " / " + fmtBytes(job.totalBytes);

  if (job.state === "running") {
    setMsg("Extracting…");
    setTimeout(pollJob, 700);
    return;
  }

  $("go").disabled = false;
  $("cancel").classList.add("hidden");
  if (job.state === "done") setMsg(job.message, "ok");
  else if (job.state === "cancelled") setMsg("Cancelled. " + job.message, "err");
  else setMsg(job.message, "err");
}

/* ----------------------------------------------------------------- boot */

$("browse").onclick = () => browse($("volumes").value || "/mnt/usb0");
$("refresh").onclick = () => browse($("curpath").value);
$("up").onclick = goUp;
$("volumes").onchange = () => {
  const v = $("volumes").value;
  if (v) browse(v);
  loadVolumes();
};
$("open").onclick = () => openPackage($("manual").value.trim());
$("manual").onkeydown = (e) => { if (e.key === "Enter") openPackage($("manual").value.trim()); };
$("dest").onkeydown = (e) => { if (e.key === "Enter") startExtract(); };
$("go").onclick = startExtract;
$("cancel").onclick = () => api("/api/cancel").catch(() => {});

refreshStatus().then(() => {
  loadVolumes().then(() => {
    const v = $("volumes").value || "/mnt/usb0";
    browse(v);
  });
});
setInterval(refreshStatus, 5000);
