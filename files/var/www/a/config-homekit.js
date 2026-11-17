// SPDX-License-Identifier: GPL-3.0-or-later
// HomeKit (rhkd) page: pairing status, setup code + QR, pairing actions.
(function () {
  const ENDPOINT = "/x/json-homekit.cgi";
  const el = (id) => document.getElementById(id);
  let last = null;

  function yesNo(v, yes, no) {
    return v ? `<span class="text-success">${yes}</span>` : `<span class="text-body-secondary">${no}</span>`;
  }

  function render(d) {
    last = d;
    el("hk-running").innerHTML = d.running
      ? `<span class="text-success">Running</span> <small class="text-body-secondary">(pid ${d.pid})</small>`
      : '<span class="text-danger">Not running</span>';
    el("hk-name").textContent = d.name || "—";
    // Each person in the Home gets their own pairing (a "controller" in HAP terms).
    const n = d.controllers || 0;
    el("hk-paired").innerHTML = d.paired
      ? `<span class="text-success">Yes</span>, ${n} ${n === 1 ? "person" : "people"} with access` +
        '<small class="d-block text-body-secondary">You and anyone you share your Home with</small>'
      : '<span class="text-body-secondary">No — ready to add in the Home app</span>';
    el("hk-discoverable").innerHTML = yesNo(d.discoverable, "Yes (unpaired)", "No");
    // Diagnostics: rhkd version (= firmware revision in Home), configuration number (c#, rises when iOS must
    // re-read the camera's services) and setup ID.
    const details = [];
    if (d.version) details.push("rhkd " + d.version);
    if (d.config_number != null) details.push("config #" + d.config_number);
    if (d.setup_id) details.push("setup ID " + d.setup_id);
    el("hk-details").textContent = details.join(" · ");
    el("hk-hint").classList.toggle("d-none", !!d.paired && !d.discoverable);
    if (!el("hk-shown").classList.contains("d-none")) renderCode();
    renderCodecs(d.codecs || {});
  }

  function renderCodecs(c) {
    const ok = (t) => `<span class="text-success"><i class="bi bi-check-circle"></i> ${t}</span>`;
    const bad = (t) => `<span class="text-warning"><i class="bi bi-exclamation-triangle"></i> ${t}</span>`;
    const name = (v) => (v || "?").toUpperCase().replace("H26", "H.26");
    const streams = `main ${name(c.main)}, sub ${name(c.sub)}`;
    if (c.video_ok) {
      const via = c.main === "h264" ? "" : " — live view uses the sub stream";
      el("hk-video").innerHTML = ok(streams + via);
    } else {
      el("hk-video").innerHTML = bad(streams + " — no H.264 stream, live view will not start");
    }
    el("hk-audio").innerHTML = c.audio_ok
      ? ok("Opus")
      : bad(`${name(c.audio)} — HomeKit audio is off (needs Opus)`);
    const fixes = [];
    if (!c.audio_ok) fixes.push("switch audio to <b>Opus</b> (RTSP clients get Opus too)");
    if (!c.video_ok) fixes.push("switch the <b>sub</b> stream to <b>H.264</b> (main is left as it is)");
    el("hk-fix-box").classList.toggle("d-none", fixes.length === 0);
    el("hk-fix-text").innerHTML = fixes.length
      ? "This will " + fixes.join(" and ") + ", save it to raptor.conf, and restart the RTSP server, so open RTSP viewers and recorders reconnect once."
      : "";
  }

  function renderCode() {
    const d = last || {};
    el("hk-code").textContent = d.setup_code || "no setup code";
    const box = el("hk-qr");
    box.innerHTML = "";
    if (d.setup_payload && typeof qrcode === "function") {
      // The X-HM:// payload is uppercase alphanumeric, so QR alphanumeric mode keeps it at version 2.
      const qr = qrcode(0, "Q");
      qr.addData(d.setup_payload, "Alphanumeric");
      qr.make();
      box.innerHTML = qr.createSvgTag({ cellSize: 6, margin: 0, scalable: true });
    } else {
      box.textContent = "No QR payload";
    }
  }

  async function load(quiet) {
    if (!quiet) showBusy("Loading HomeKit status...");
    try {
      const r = await fetch(ENDPOINT, { headers: { Accept: "application/json" }, cache: "no-store" });
      const d = await r.json();
      if (!r.ok || d.error) throw new Error((d.error && d.error.message) || "HTTP " + r.status);
      render(d);
    } catch (e) {
      showAlert("danger", "Failed to load HomeKit status: " + e.message);
    } finally {
      if (!quiet) hideBusy();
    }
  }

  async function action(name, busyText, doneText) {
    showBusy(busyText);
    try {
      const r = await fetch(ENDPOINT, {
        method: "POST",
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({ action: name }),
      });
      const d = await r.json();
      if (!r.ok || d.error) throw new Error((d.error && d.error.message) || "HTTP " + r.status);
      render(d);
      showAlert("success", doneText, 5000);
    } catch (e) {
      showAlert("danger", "Failed: " + e.message);
    } finally {
      hideBusy();
    }
  }

  el("hk-show").addEventListener("click", () => {
    el("hk-hidden").classList.add("d-none");
    el("hk-shown").classList.remove("d-none");
    renderCode();
  });
  el("hk-hide").addEventListener("click", () => {
    el("hk-shown").classList.add("d-none");
    el("hk-hidden").classList.remove("d-none");
    el("hk-qr").innerHTML = "";
    el("hk-code").textContent = "";
  });
  el("hk-reload").addEventListener("click", () => load(false));
  el("hk-restart").addEventListener("click", () =>
    action("restart", "Restarting HomeKit service...", "HomeKit service restarted."));
  el("hk-fix").addEventListener("click", () => {
    if (!confirm("Change the camera's codecs for HomeKit?\n\nRTSP viewers and recorders reconnect once."))
      return;
    action("fixcodecs", "Changing codecs...", "Codecs updated for HomeKit.");
  });
  el("hk-unpair").addEventListener("click", () => {
    if (!confirm("Unpair this camera from all Apple devices and home hubs?\n\nRemove it from the Home app as well, then add it again with the same setup code."))
      return;
    action("unpair", "Unpairing...", "Unpaired. The camera can now be added in the Home app.");
  });
  el("hk-newcode").addEventListener("click", () => {
    if (!confirm("Generate a new setup code?\n\nThis unpairs the camera and the old code stops working."))
      return;
    action("newcode", "Generating a new setup code...", "New setup code created. The camera is unpaired.");
  });

  load(false);
})();
