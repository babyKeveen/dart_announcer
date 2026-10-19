#pragma once
#include <Arduino.h>
#include <Update.h>
#include "esp_ota_ops.h"

inline String buildOtaHtml() {
    uint32_t sketchSize = ESP.getSketchSize();
    uint32_t freeSpace = ESP.getFreeSketchSpace();
    uint32_t flashSize = ESP.getFlashChipSize();
    const esp_partition_t* runningPart = esp_ota_get_running_partition();
    const esp_partition_t* nextPart = esp_ota_get_next_update_partition(NULL);

    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DART Display - Firmware OTA Update</title>
<style>
  :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --accent: #22c55e; --accent-hover: #16a34a; --error: #ef4444; }
  body { background: var(--bg); color: var(--text); font-family: -apple-system, system-ui, sans-serif; padding: 24px 16px; margin: 0; display: flex; justify-content: center; }
  .card { background: var(--card); border-radius: 12px; padding: 24px; max-width: 480px; width: 100%; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
  h1 { font-size: 20px; margin-top: 0; display: flex; align-items: center; gap: 8px; }
  p { color: #94a3b8; font-size: 14px; line-height: 1.5; margin-bottom: 16px; }
  .info-box { background: #0f172a; border-radius: 8px; padding: 12px 14px; font-size: 13px; color: #cbd5e1; margin-bottom: 20px; line-height: 1.6; border: 1px solid #334155; }
  .info-box b { color: #38bdf8; }
  .drop-zone { border: 2px dashed #475569; border-radius: 8px; padding: 24px 16px; text-align: center; cursor: pointer; transition: all 0.2s; margin-bottom: 16px; background: rgba(15,23,42,0.5); }
  .drop-zone.dragover { border-color: var(--accent); background: rgba(34,197,94,0.1); }
  .drop-zone input[type=file] { display: none; }
  .drop-zone span { display: block; font-size: 13px; color: #94a3b8; margin-top: 6px; }
  .file-name { font-weight: bold; color: #38bdf8; margin-top: 10px; word-break: break-all; font-size: 14px; }
  button { width: 100%; padding: 14px; border: none; border-radius: 6px; background: var(--accent); color: #000; font-size: 16px; font-weight: bold; cursor: pointer; transition: opacity 0.2s; }
  button:hover:not(:disabled) { background: var(--accent-hover); }
  button:disabled { opacity: 0.4; cursor: not-allowed; }
  .progress-wrap { display: none; margin-top: 16px; }
  .progress-bar-bg { background: #334155; border-radius: 6px; height: 16px; overflow: hidden; }
  .progress-bar { background: var(--accent); height: 100%; width: 0%; transition: width 0.15s ease; }
  .progress-text { font-size: 13px; color: #94a3b8; margin-top: 6px; text-align: center; font-variant-numeric: tabular-nums; }
  .status-msg { margin-top: 16px; font-size: 14px; text-align: center; font-weight: bold; line-height: 1.5; }
  .status-success { color: var(--accent); }
  .status-error { color: var(--error); }
  .nav-links { margin-top: 24px; padding-top: 16px; border-top: 1px solid #334155; text-align: center; font-size: 13px; display: flex; justify-content: center; gap: 16px; }
  .nav-links a { color: #38bdf8; text-decoration: none; font-weight: 600; }
  .nav-links a:hover { text-decoration: underline; }
  .footer { text-align: center; font-size: 11px; color: #64748b; margin-top: 16px; }
</style>
</head>
<body>
<div class="card">
  <h1>⬆️ Firmware OTA Update</h1>
  <p>Wirelessly update your DART Frame with a new compiled binary (<code>.bin</code>) over Wi-Fi without using a USB cable.</p>

  <div class="info-box">
    <div>Target Chip: <b>Adafruit Feather ESP32-S3 (4MB)</b></div>
    <div>Current Sketch: <b>)rawliteral";
    html += String(sketchSize / 1024) + " KB</b></div>\n";
    html += "    <div>Free App Slot: <b>" + String(freeSpace / 1024) + " KB</b></div>\n";
    html += "    <div>Total Flash: <b>" + String(flashSize / (1024 * 1024)) + " MB</b></div>\n";
    html += "    <div>Active Partition: <b>" + String(runningPart ? runningPart->label : "Unknown") + "</b></div>\n";
    if (nextPart) {
        html += "    <div>OTA Target Slot: <b>" + String(nextPart->label) + " (" + String(nextPart->size / 1024) + " KB)</b></div>\n";
    } else {
        html += "    <div>OTA Target Slot: <b style=\"color:#ef4444;\">None (Single Partition)</b></div>\n";
    }
    html += R"rawliteral(  </div>
)rawliteral";

    if (!nextPart) {
        html += R"rawliteral(
  <div style="background:rgba(239,68,68,0.18); border:1px solid #ef4444; border-radius:8px; padding:16px; margin-bottom:20px; font-size:13px; color:#fca5a5; line-height:1.6;">
    <div style="font-weight:bold; font-size:15px; margin-bottom:6px; color:#f87171;">⚠️ Partition Table Has No OTA Target</div>
    Your board is currently running a single-partition <i>&ldquo;No OTA&rdquo;</i> layout (e.g. TinyUF2 No OTA). In this mode, the ESP32 does not have a secondary flash slot to write updates to.<br><br>
    <b>To enable wireless OTA updates:</b><br>
    1. In <b>Arduino IDE</b>, open <b>Tools ➔ Partition Scheme</b>.<br>
    2. Change from <i>TinyUF2 No OTA</i> to <b>&ldquo;Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)&rdquo;</b> (or <b>&ldquo;Default 4MB with spiffs&rdquo;</b>).<br>
    3. Plug in the USB cable and click <b>Upload</b> once.<br><br>
    <i>This re-partitions the flash chip with dual OTA slots (ota_0 &amp; ota_1), enabling wireless browser updates forever!</i>
  </div>
)rawliteral";
    }

    html += R"rawliteral(
  <div class="drop-zone" id="dropZone" onclick="document.getElementById('firmwareFile').click()">
    <svg width="36" height="36" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24" style="color:#94a3b8; margin:0 auto; display:block;"><path stroke-linecap="round" stroke-linejoin="round" d="M7 16a4 4 0 01-.88-7.903A5 5 0 1115.9 6L16 6a5 5 0 011 9.9M15 13l-3-3m0 0l-3 3m3-3v12"/></svg>
    <div id="dropText" style="font-weight:600; margin-top:8px;">Click to browse or drop .bin file here</div>
    <span>(e.g., esp32_dart_display.ino.bin)</span>
    <div class="file-name" id="fileName"></div>
    <input type="file" id="firmwareFile" accept=".bin" onchange="onFileSelected(this.files)">
  </div>

  <button type="button" id="uploadBtn" disabled onclick="startUpload()">⚡ Flash Firmware Now</button>

  <div class="progress-wrap" id="progressWrap">
    <div class="progress-bar-bg">
      <div class="progress-bar" id="progressBar"></div>
    </div>
    <div class="progress-text" id="progressText">0%</div>
  </div>

  <div id="statusMsg" class="status-msg"></div>

  <div class="nav-links">
    <a href="/">← Live Preview</a>
    <a href="/config">⚙️ Settings</a>
  </div>
  <div class="footer">DART Kitchen Display &bull; Over-The-Air Update &bull; Weather data by <a href="https://open-meteo.com/" target="_blank" style="color:#94a3b8; text-decoration:underline;">Open-Meteo</a></div>
</div>

<script>
let selectedFile = null;
const dropZone = document.getElementById('dropZone');
const fileInput = document.getElementById('firmwareFile');
const fileName = document.getElementById('fileName');
const uploadBtn = document.getElementById('uploadBtn');
const progressWrap = document.getElementById('progressWrap');
const progressBar = document.getElementById('progressBar');
const progressText = document.getElementById('progressText');
const statusMsg = document.getElementById('statusMsg');

['dragenter', 'dragover'].forEach((name) => {
  dropZone.addEventListener(name, (e) => { e.preventDefault(); dropZone.classList.add('dragover'); }, false);
});
['dragleave', 'drop'].forEach((name) => {
  dropZone.addEventListener(name, (e) => { e.preventDefault(); dropZone.classList.remove('dragover'); }, false);
});
dropZone.addEventListener('drop', (e) => {
  if (e.dataTransfer.files.length) {
    onFileSelected(e.dataTransfer.files);
  }
});

const onFileSelected = (files) => {
  if (files.length === 0) return;
  selectedFile = files[0];
  if (!selectedFile.name.endsWith('.bin')) {
    statusMsg.className = 'status-msg status-error';
    statusMsg.innerText = '⚠️ Please select a valid .bin firmware file!';
    uploadBtn.disabled = true;
    fileName.innerText = '';
    return;
  }
  statusMsg.innerText = '';
  fileName.innerText = '📄 ' + selectedFile.name + ' (' + Math.round(selectedFile.size / 1024) + ' KB)';
  uploadBtn.disabled = false;
};

const startUpload = () => {
  if (!selectedFile) return;
  uploadBtn.disabled = true;
  dropZone.style.pointerEvents = 'none';
  progressWrap.style.display = 'block';
  statusMsg.className = 'status-msg';
  statusMsg.innerText = 'Flashing firmware to ESP32... Do not unplug power!';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update', true);

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      progressBar.style.width = pct + '%';
      progressText.innerText = pct + '% (' + Math.round(e.loaded / 1024) + ' / ' + Math.round(e.total / 1024) + ' KB)';
    }
  };

  xhr.onload = () => {
    if (xhr.status === 200) {
      progressBar.style.width = '100%';
      progressText.innerText = '100% - Firmware Verified!';
      statusMsg.className = 'status-msg status-success';
      let countdown = 10;
      statusMsg.innerText = '✅ Update Succeeded! Rebooting ESP32... Reconnecting in ' + countdown + 's';
      const timer = setInterval(() => {
        countdown--;
        if (countdown <= 0) {
          clearInterval(timer);
          window.location.href = '/';
        } else {
          statusMsg.innerText = '✅ Update Succeeded! Rebooting ESP32... Reconnecting in ' + countdown + 's';
        }
      }, 1000);
    } else {
      statusMsg.className = 'status-msg status-error';
      statusMsg.innerText = '❌ Update Failed: ' + (xhr.responseText || 'HTTP ' + xhr.status);
      uploadBtn.disabled = false;
      dropZone.style.pointerEvents = 'auto';
    }
  };

  xhr.onerror = () => {
    statusMsg.className = 'status-msg status-error';
    statusMsg.innerText = '❌ Upload connection interrupted or timed out.';
    uploadBtn.disabled = false;
    dropZone.style.pointerEvents = 'auto';
  };

  const formData = new FormData();
  formData.append('firmware', selectedFile, selectedFile.name);
  xhr.send(formData);
};
</script>
</body>
</html>
)rawliteral";
    return html;
}
