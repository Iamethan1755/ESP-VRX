import { ESPLoader, Transport } from './esptool.js';
import { SerialTerminal } from './serial-terminal.js';
import { ReceiverConsole, MENU_COMMAND_KEYS } from './receiver-console.js';

const $ = id => document.getElementById(id);

// ---- Flasher -------------------------------------------------------------
const btnConnect = $('btnConnect');
const btnConnectText = $('btnConnectText');
const btnFlash = $('btnFlash');
const unsupportedWarning = $('unsupportedWarning');

const tabGithub = $('tabGithub');
const tabAlpha = $('tabAlpha');
const tabLocal = $('tabLocal');
const paneAlpha = $('paneAlpha');
const paneGithub = $('paneGithub');
const paneLocal = $('paneLocal');

const selectAlphaBuild = $('selectAlphaBuild');
const selectAlphaPackageType = $('selectAlphaPackageType');
const alphaBuildDetails = $('alphaBuildDetails');

const selectRelease = $('selectRelease');
const selectPackageType = $('selectPackageType');
const releaseDetails = $('releaseDetails');

const dropzone = $('dropzone');
const inputLocalFile = $('inputLocalFile');
const localFileDetails = $('localFileDetails');
const inputFlashOffset = $('inputFlashOffset');

const selectBaud = $('selectBaud');
const chkEraseAll = $('chkEraseAll');
const statStatus = $('statStatus');
const progressContainer = $('progressContainer');
const progressBar = $('progressBar');
const progressStatusText = $('progressStatusText');
const progressPercentText = $('progressPercentText');
const consoleOutput = $('consoleOutput');

let activeSource = 'alpha'; // 'alpha' (C5VRX-5) | 'local' | 'github' (legacy builds)
let port = null;
let transport = null;
let esploader = null;
let isConnected = false;
let isConnecting = false;
let isFlashing = false;
let githubReleases = [];
let githubAlphaBuilds = [];
let localFileBinary = null;

// Shown only when neither the Pages index nor the GitHub API answers.
const FALLBACK_RELEASES = [
  {
    tag_name: 'v3.0.0-rc1',
    name: 'C5VRX-3 v3.0.0-rc1: Seamless 16K Phase5 Production Receiver',
    published_at: '2026-09-18T17:01:49Z',
    prerelease: true,
    assets: [
      { name: 'c5vrx3.bin', size: 1082064, browser_download_url: 'https://github.com/Twotoz/C5VRX/releases/download/v3.0.0-rc1/c5vrx3.bin' },
      { name: 'bootloader.bin', size: 23232, browser_download_url: 'https://github.com/Twotoz/C5VRX/releases/download/v3.0.0-rc1/bootloader.bin' },
      { name: 'partition-table.bin', size: 3072, browser_download_url: 'https://github.com/Twotoz/C5VRX/releases/download/v3.0.0-rc1/partition-table.bin' },
    ]
  }
];
const VERSION_TAG_PATTERN = /^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const ALPHA_TAG_PATTERN = /^c5vrx5-(?:pr-[0-9]+|v5\.[0-9]+\.[0-9]+)$/;

function log(msg, type = 'info') {
  if (type === 'error') $('flashLog').open = true;
  consoleOutput.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
  consoleOutput.scrollTop = consoleOutput.scrollHeight;
}

// esptool-js writes its own progress through this terminal.
const terminal = {
  clean() { consoleOutput.textContent = ''; },
  writeLine(data) { this.write(`${data}\n`); },
  write(data) {
    consoleOutput.textContent += data;
    consoleOutput.scrollTop = consoleOutput.scrollHeight;
  }
};

function checkSerialSupport() {
  if ('serial' in navigator) return true;
  unsupportedWarning.hidden = false;
  log('Web Serial is not available in this browser. Use Chrome or Edge on a desktop to connect.');
  return false;
}

function activateSource(source) {
  if (isFlashing) return;
  activeSource = source;
  for (const [button, pane, name] of [[tabGithub, paneGithub, 'github'], [tabAlpha, paneAlpha, 'alpha'], [tabLocal, paneLocal, 'local']]) {
    button.setAttribute('aria-pressed', String(source === name));
    button.classList.toggle('active', source === name);
    pane.classList.toggle('active', source === name);
  }
  updateFlashButtonState();
}

tabGithub.addEventListener('click', () => activateSource('github'));
tabAlpha.addEventListener('click', () => activateSource('alpha'));
tabLocal.addEventListener('click', () => activateSource('local'));
selectAlphaBuild.addEventListener('change', () => onAlphaBuildSelected());
selectRelease.addEventListener('change', () => onReleaseSelected(parseInt(selectRelease.value, 10)));

// Local file: the Browse button sits inside the drop zone and bubbles here.
dropzone.addEventListener('click', (event) => {
  // The file input click bubbles too; never open the picker recursively.
  if (!isFlashing && event.target !== inputLocalFile) inputLocalFile.click();
});
dropzone.addEventListener('dragover', (event) => {
  event.preventDefault();
  dropzone.classList.add('dragover');
});
dropzone.addEventListener('dragleave', () => dropzone.classList.remove('dragover'));
dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('dragover');
  if (!isFlashing && event.dataTransfer.files.length > 0) handleLocalFile(event.dataTransfer.files[0]);
});
inputLocalFile.addEventListener('change', (event) => {
  if (event.target.files.length > 0) handleLocalFile(event.target.files[0]);
  inputLocalFile.value = '';   // choosing the same file again still fires change
});

function handleLocalFile(file) {
  const name = file.name.toLowerCase();
  if (!name.endsWith('.bin')) {
    alert('Please select a compiled firmware binary (.bin file).');
    return;
  }
  $('localFileName').textContent = file.name;
  $('localFileSize').textContent = `${(file.size / 1024).toFixed(1)} KB (${file.size.toLocaleString()} bytes)`;
  inputFlashOffset.value = name.includes('merged') ? '0x0'
    : name.includes('bootloader') ? '0x2000'
      : name.includes('partition') ? '0x8000' : '0x10000';

  const reader = new FileReader();
  reader.onload = (event) => {
    localFileBinary = event.target.result;
    localFileDetails.hidden = false;
    log(`Loaded local file: ${file.name} (${file.size} bytes)`);
    updateFlashButtonState();
  };
  reader.onerror = () => log(`Could not read ${file.name}.`, 'error');
  reader.readAsArrayBuffer(file);
}

function parseFlashOffset(text) {
  const value = text.trim();
  if (!/^(?:0x)?[0-9a-f]+$/i.test(value)) throw new Error(`Invalid flash offset "${text}"; use hex such as 0x0 or 0x10000`);
  const offset = parseInt(value, 16);
  if (offset % 0x1000) throw new Error(`Flash offset ${value} is not on a 4 KB sector boundary`);
  return offset;
}

// Fetch the firmware index mirrored into the GitHub Pages artifact.
// Production flashing must stay same-origin: browsers cannot reliably fetch
// GitHub Release storage redirects due to CORS.
// quiet: background refresh. No placeholders or log lines, and the lists
// are only rebuilt (keeping the selected build) when a build was added or
// removed.
async function fetchReleases({ quiet = false } = {}) {
  if (!quiet) {
    selectRelease.innerHTML = '<option value="">Fetching firmware index…</option>';
    selectAlphaBuild.innerHTML = '<option value="">Fetching C5VRX-5 builds…</option>';
  }
  const say = quiet ? () => {} : log;

  let data = null;
  try {
    const mirrorUrl = new URL('firmware/releases.json', document.baseURI);
    mirrorUrl.searchParams.set('t', Date.now().toString());
    const mirrorRes = await fetch(mirrorUrl.href, { cache: 'no-store' });
    if (!mirrorRes.ok) throw new Error(`Pages firmware index HTTP ${mirrorRes.status}`);
    data = await mirrorRes.json();
    if (!Array.isArray(data)) throw new Error('Pages firmware index is not an array');
    say(`Loaded the firmware index (${data.length} build(s)).`);
  } catch (mirrorError) {
    if (quiet) return;   // a background refresh keeps what is shown
    // Metadata fallback only. Binary flashing still prefers same-origin assets;
    // this path mainly keeps local development usable for release browsing.
    say(`Pages firmware index unavailable (${mirrorError.message}); using GitHub release metadata.`);
    try {
      const res = await fetch('https://api.github.com/repos/Twotoz/C5VRX/releases?per_page=100', {
        headers: { Accept: 'application/vnd.github.v3+json' },
        cache: 'no-store'
      });
      if (!res.ok) throw new Error(`GitHub API HTTP ${res.status}`);
      data = await res.json();
      if (!Array.isArray(data)) throw new Error('GitHub releases response is not an array');
    } catch (apiError) {
      say(`Could not fetch release metadata (${apiError.message}). Showing the cached legacy release only.`);
      data = FALLBACK_RELEASES;
    }
  }

  const byDate = (a, b) => new Date(b.published_at || 0) - new Date(a.published_at || 0);
  let releases = data.filter(rel => VERSION_TAG_PATTERN.test(rel.tag_name || '')).sort(byDate);
  if (releases.length === 0) releases = FALLBACK_RELEASES;
  // C5VRX-5: versioned main builds first (newest first), then unmerged PR builds.
  const isPr = rel => Number(rel.tag_name.startsWith('c5vrx5-pr-'));
  const alpha = data.filter(rel => ALPHA_TAG_PATTERN.test(rel.tag_name || ''))
    .sort((a, b) => (isPr(a) - isPr(b)) || byDate(a, b));
  if (isFlashing) return;   // never swap the lists under a running flash
  const signature = list => list.map(rel => `${rel.tag_name}@${rel.published_at || ''}`).join(',');
  const releasesChanged = signature(releases) !== signature(githubReleases);
  const alphaChanged = signature(alpha) !== signature(githubAlphaBuilds);
  if (quiet && !releasesChanged && !alphaChanged) return;
  const keepRelease = githubReleases[Number(selectRelease.value)]?.tag_name;
  const keepAlpha = githubAlphaBuilds[Number(selectAlphaBuild.value)]?.tag_name;
  githubReleases = releases;
  githubAlphaBuilds = alpha;
  say(`Available: ${githubAlphaBuilds.length} C5VRX-5 build(s), ${githubReleases.length} legacy release(s).`);

  if (!quiet || releasesChanged) populateReleaseDropdown(quiet ? keepRelease : undefined);
  if (!quiet || alphaChanged) populateAlphaBuildDropdown(quiet ? keepAlpha : undefined);
  if (quiet) log(`Firmware list updated: ${githubAlphaBuilds.length} C5VRX-5 build(s).`);
  updateFlashButtonState();
}

// The firmware lists follow new and removed builds by themselves.
const RELEASE_REFRESH_MS = 60000;
setInterval(() => { if (document.visibilityState !== 'hidden') fetchReleases({ quiet: true }); }, RELEASE_REFRESH_MS);
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') fetchReleases({ quiet: true });
});

function populateAlphaBuildDropdown(keepTag) {
  selectAlphaBuild.replaceChildren();
  githubAlphaBuilds.forEach((rel, index) => {
    const opt = document.createElement('option');
    opt.value = index;
    opt.textContent = rel.tag_name.startsWith('c5vrx5-v')
      ? `C5VRX-5 ${rel.tag_name.slice('c5vrx5-'.length)}${index === 0 ? ' (Latest)' : ''}`
      : `C5VRX-5 — PR #${rel.tag_name.split('-').pop()} / unmerged`;
    selectAlphaBuild.appendChild(opt);
  });
  if (githubAlphaBuilds.length) {
    const kept = githubAlphaBuilds.findIndex(rel => rel.tag_name === keepTag);
    selectAlphaBuild.value = String(kept >= 0 ? kept : 0);
    onAlphaBuildSelected();
  } else {
    selectAlphaBuild.innerHTML = '<option value="">No C5VRX-5 builds published yet</option>';
    alphaBuildDetails.hidden = true;
  }
}

function showDetails(box, ids, rel) {
  box.hidden = false;
  const values = [
    rel.name || rel.tag_name,
    rel.published_at ? new Date(rel.published_at).toLocaleDateString() : 'N/A',
    rel.tag_name,
    (rel.assets || []).map(a => a.name).join(', ') || 'No binary assets attached'
  ];
  ids.forEach((id, i) => { $(id).textContent = values[i]; });
}

function onAlphaBuildSelected() {
  const rel = githubAlphaBuilds[Number(selectAlphaBuild.value)];
  if (!rel) return;
  showDetails(alphaBuildDetails, ['infoAlphaBuildName', 'infoAlphaBuildDate', 'infoAlphaBuildTag', 'infoAlphaBuildAssets'], rel);
  updateFlashButtonState();
}

function populateReleaseDropdown(keepTag) {
  selectRelease.replaceChildren();
  githubReleases.forEach((rel, index) => {
    const opt = document.createElement('option');
    opt.value = index;
    const tag = rel.tag_name || rel.name;
    opt.textContent = `${tag}${rel.prerelease ? ' [Pre-release]' : ''}${index === 0 ? ' (Latest)' : ''}`;
    selectRelease.appendChild(opt);
  });
  const kept = Math.max(0, githubReleases.findIndex(rel => (rel.tag_name || rel.name) === keepTag));
  selectRelease.value = String(kept);
  onReleaseSelected(kept);
}

function onReleaseSelected(index) {
  const rel = githubReleases[index];
  if (!rel) return;
  showDetails(releaseDetails, ['infoReleaseName', 'infoReleaseDate', 'infoReleaseTag', 'infoReleaseAssets'], rel);
  updateFlashButtonState();
}

function updateFlashButtonState() {
  if (!isConnected || isFlashing) {
    btnFlash.disabled = true;
  } else if (activeSource === 'github') {
    btnFlash.disabled = !githubReleases[parseInt(selectRelease.value, 10)];
  } else if (activeSource === 'alpha') {
    btnFlash.disabled = !githubAlphaBuilds[parseInt(selectAlphaBuild.value, 10)];
  } else {
    btnFlash.disabled = localFileBinary === null;
  }
}

function getSelectedRemoteBuild() {
  if (activeSource === 'github') {
    return { release: githubReleases[parseInt(selectRelease.value, 10)], packageType: selectPackageType.value };
  }
  if (activeSource === 'alpha') {
    return { release: githubAlphaBuilds[parseInt(selectAlphaBuild.value, 10)], packageType: selectAlphaPackageType.value };
  }
  return { release: null, packageType: null };
}

function findApplicationAsset(assets) {
  // GitHub returns release assets in upload order. Never use the first .bin:
  // bootloader.bin is commonly uploaded before the application image.
  return assets.find(asset => /^c5vrx(?:[345])?\.bin$/i.test(asset.name || '')) ||
    assets.find(asset => {
      const name = (asset.name || '').toLowerCase();
      return name.endsWith('.bin') && !name.includes('bootloader') && !name.includes('partition') && !name.includes('merged');
    });
}

btnConnect.addEventListener('click', async () => {
  if (isFlashing || isConnecting) return;
  if (isConnected) await disconnectDevice();
  else await connectDevice();
});

async function connectDevice() {
  if (isConnecting || isConnected || isFlashing || !['disconnected', 'connected'].includes(serialConsole.state) || !checkSerialSupport()) return;
  isConnecting = true;
  syncDeviceControls();

  try {
    await serialConsole.disconnect();
    log('Opening the serial port selector…');
    port = await navigator.serial.requestPort();
    statStatus.textContent = 'Connecting…';

    transport = new Transport(port);
    esploader = new ESPLoader({
      transport,
      baudrate: parseInt(selectBaud.value, 10) || 460800,
      terminal,
      romBaudrate: 115200,
      debugLogging: false
    });

    log('Handshaking with the ESP32-C5 ROM bootloader…');
    await esploader.main('usb_reset');

    isConnected = true;
    btnConnectText.textContent = 'Disconnect';
    const chipName = esploader.chip ? esploader.chip.CHIP_NAME : 'ESP32-C5';
    statStatus.textContent = `${chipName} connected · ready to flash`;
    log(`Connected to ${chipName}.`);
  } catch (err) {
    if (err?.name === 'NotFoundError') {
      log('No port selected.');
    } else {
      log(`Connection failed: ${err.message || err}`, 'error');
      alert(`Failed to connect: ${err.message || err}\n\nIf the port timed out, hold BOOT on the XIAO, tap RESET, release BOOT and try again.`);
    }
    await disconnectDevice();
  } finally {
    isConnecting = false;
    syncDeviceControls();
    updateFlashButtonState();
  }
}

async function disconnectDevice(message = 'No device connected') {
  const wasOpen = Boolean(transport);
  try {
    if (transport) await transport.disconnect();
  } catch {
    // The port may already be gone after a reset.
  }
  port = null;
  transport = null;
  esploader = null;
  isConnected = false;
  btnConnectText.textContent = 'Connect device';
  statStatus.textContent = message;
  syncDeviceControls();
  updateFlashButtonState();
  if (wasOpen) log('Device disconnected.');
}

async function collectRemoteImages(selected) {
  const rel = selected.release;
  if (!rel) throw new Error('No release selected');
  const assets = rel.assets || [];
  log(`Fetching binaries for ${rel.tag_name} (${selected.packageType})…`);

  if (selected.packageType !== 'merged') {
    const app = findApplicationAsset(assets);
    if (!app) throw new Error('Could not find application firmware binary in release assets');
    return [{ data: new Uint8Array(await fetchReleaseAsset(app)), address: 0x10000 }];
  }
  const mergedAsset = assets.find(a => a.name.includes('merged'));
  if (mergedAsset) return [{ data: new Uint8Array(await fetchReleaseAsset(mergedAsset)), address: 0x0 }];

  // Standard three-part layout.
  const bootloader = assets.find(a => a.name.includes('bootloader'));
  const ptable = assets.find(a => a.name.includes('partition'));
  const app = findApplicationAsset(assets);
  const missing = [!bootloader && 'bootloader', !ptable && 'partition table', !app && 'application firmware'].filter(Boolean);
  if (missing.length > 0) throw new Error(`Incomplete full firmware package: missing ${missing.join(', ')}`);
  const images = [];
  for (const [asset, address] of [[bootloader, 0x2000], [ptable, 0x8000], [app, 0x10000]]) {
    images.push({ data: new Uint8Array(await fetchReleaseAsset(asset)), address });
  }
  return images;
}

function setProgress(percent, text) {
  progressBar.style.width = `${percent}%`;
  progressPercentText.textContent = `${percent}%`;
  if (text) progressStatusText.textContent = text;
}

btnFlash.addEventListener('click', async () => {
  if (!isConnected || !esploader || isFlashing || serialConsole.state !== 'disconnected') return;
  const selected = getSelectedRemoteBuild();
  if (activeSource === 'alpha' && /^c5vrx5-pr-/.test(selected.release?.tag_name || '') &&
      !window.confirm('Flash an unmerged C5VRX-5 PR build? It may fail to boot or produce broken video.')) return;

  isFlashing = true;
  syncDeviceControls();
  updateFlashButtonState();
  $('flashLog').open = true;
  progressContainer.hidden = false;
  setProgress(0, 'Preparing firmware images…');

  try {
    let fileArray;
    if (activeSource === 'local') {
      if (!localFileBinary) throw new Error('No local binary selected');
      fileArray = [{ data: new Uint8Array(localFileBinary), address: parseFlashOffset(inputFlashOffset.value) }];
    } else {
      fileArray = await collectRemoteImages(selected);
    }

    const totalBytes = fileArray.reduce((acc, f) => acc + f.data.length, 0);
    log(`Writing ${fileArray.length} image(s), ${totalBytes.toLocaleString()} bytes.`);
    progressStatusText.textContent = 'Flashing…';

    await esploader.writeFlash({
      fileArray,
      flashSize: '8MB',
      flashMode: 'dio',
      flashFreq: '80m',
      eraseAll: chkEraseAll.checked,
      // ESP32-C5 native USB-Serial/JTAG can fail mid-write in the compressed path (status 201,0).
      compress: false,
      reportProgress: (fileIndex, written, total) => {
        setProgress(Math.floor((written / total) * 100),
          `Writing image ${fileIndex + 1}/${fileArray.length} (${(written / 1024).toFixed(0)} / ${(total / 1024).toFixed(0)} KB)`);
      }
    });

    setProgress(100, 'Done. Restarting the receiver…');
    log('Flash written and verified. Resetting into the new firmware…');
    try {
      await esploader.after('hard_reset');
    } catch {
      // The reset pulse can close the USB-CDC port.
    }
    progressStatusText.textContent = 'Done. Open Terminal to watch the receiver boot.';
    log('Flashing completed.');
    // The ROM loader is gone after the reset: release the port for the
    // terminal while the controls are still locked.
    await disconnectDevice('Flashed · the receiver is restarting');
  } catch (err) {
    log(`Flashing failed: ${err.message || err}`, 'error');
    alert(`Flashing error: ${err.message || err}`);
    progressStatusText.textContent = 'Flash failed';
  } finally {
    isFlashing = false;
    syncDeviceControls();
    updateFlashButtonState();
  }
});

async function fetchReleaseAsset(asset) {
  if (!asset) throw new Error('Missing firmware asset metadata');
  log(`Downloading ${asset.name}…`);

  if (asset.local_url) {
    const localRes = await fetch(new URL(asset.local_url, document.baseURI).href, { cache: 'no-store' });
    if (!localRes.ok) throw new Error(`Pages firmware mirror HTTP ${localRes.status} for ${asset.name}`);
    const buffer = await localRes.arrayBuffer();
    if (asset.size && buffer.byteLength !== asset.size) {
      throw new Error(`Firmware mirror size mismatch for ${asset.name}: expected ${asset.size}, got ${buffer.byteLength}`);
    }
    return buffer;
  }

  // Development fallback only. Production Pages manifests always attach a
  // local_url. GitHub's release storage redirect may still be blocked by CORS.
  if (asset.url) {
    try {
      const apiRes = await fetch(asset.url, {
        headers: { Accept: 'application/octet-stream', 'X-GitHub-Api-Version': '2022-11-28' },
        redirect: 'follow',
        cache: 'no-store'
      });
      if (!apiRes.ok) throw new Error(`GitHub asset API HTTP ${apiRes.status}`);
      return await apiRes.arrayBuffer();
    } catch (apiError) {
      log(`GitHub asset API failed (${apiError.message}); trying the browser download URL…`);
    }
  }
  if (asset.browser_download_url) {
    try {
      const directRes = await fetch(asset.browser_download_url, { redirect: 'follow', cache: 'no-store' });
      if (!directRes.ok) throw new Error(`GitHub download HTTP ${directRes.status}`);
      return await directRes.arrayBuffer();
    } catch (directError) {
      throw new Error('This build is not mirrored on GitHub Pages and GitHub blocked the cross-origin download. ' +
        `Refresh the flasher after the Pages sync completes. Details: ${directError.message}`);
    }
  }
  throw new Error(`No downloadable URL available for ${asset.name || 'firmware asset'}`);
}

$('btnClearConsole').addEventListener('click', () => {
  terminal.clean();
  log('Console cleared.');
});

// ---- Receiver terminal ---------------------------------------------------
// Firmware and terminal own separate transports and never open concurrently.
const serialOutput = $('output');
const commandInput = $('commandInput');
const btnTerminalConnect = $('btnTerminalConnect');
const terminalBaud = $('terminalBaud');
const terminalStatus = $('terminalStatus');
const shortcutButtons = [...document.querySelectorAll('[data-command]')];
const MAX_LOG_CHARS = 120000;
// Keys whose effect the menu should show: one status report shortly after.
// Scan and calibration report when they finish instead.
const REFRESH_AFTER = new Set(['b', 'c', 'v', 'o', 'e', 'x', 'g', 'K', 'k', 'j', 'P', 'A']);
const REFRESH_DELAY_MS = 700;
const receiverConsole = new ReceiverConsole();
let commandBusy = false;
let refreshTimer = null;
let logPending = '';
let logTimer = null;
let logLinePartial = '';   // unfinished line, held until STATUS lines can be told apart

function setText(element, text) {
  if (element.textContent !== text) element.textContent = text;
}

function receiverValues(v) {
  if (!v) return {};
  return {
    channel: v.channel,
    freq: v.freq && `${v.freq} MHz`,
    band: v.band,
    signal: v.signal === null ? '' : `${v.signal}/100`,
    lock: v.locked === null ? '' : v.locked ? 'VIDEO' : 'SEARCH',
    standard: v.standard,
    detected: v.detected,
    output: v.output,
    menu: v.menu === null ? '' : v.menu ? 'OPEN' : 'CLOSED · LIVE VIDEO',
    calibrate: v.busy ? 'RUNNING · VIDEO STOPPED' : v.stage || v.statusAt ? 'READY' : '',
    scan: v.scan,
    kpMode: v.kpMode,
    liveProfile: v.liveProfile === undefined ? '' : v.liveProfile ? 'ON' : 'OFF · REPORT ONLY',
    profile: v.profile
  };
}

function renderReceiver() {
  const connected = serialConsole.state === 'connected';
  const v = connected && receiverConsole.current ? receiverConsole.view() : null;
  setText($('terminalFirmware'), !connected
    ? 'Connect a C5VRX-5 receiver over USB to use its menu.'
    : v
      ? `C5VRX-5 ${receiverConsole.version} · ${v.statusAt ? `status from ${new Date(v.statusAt).toLocaleTimeString()}` : 'waiting for a status report'}`
      : 'Identifying the firmware… the menu needs C5VRX-5 firmware.');

  const notice = $('receiverNotice');
  notice.hidden = !v?.busy;
  if (v?.busy) {
    setText(notice, `${v.refused ? `Key ${v.refused} was ignored: ` : ''}the receiver is calibrating (or running a lane test), ` +
      'so it ignores menu commands until that finishes. The menu buttons come back by themselves.');
  }

  const values = receiverValues(v);
  for (const element of document.querySelectorAll('[data-value]')) {
    const empty = element.closest?.('.receiver-header') ? '' : '—';
    setText(element, values[element.dataset.value] || empty);
  }
  // Same bar count as the on-screen menu (menu.c).
  const signal = v?.signal ?? 0;
  const bars = signal <= 0 ? 0 : signal >= 100 ? 5 : Math.floor((signal + 19) / 20);
  for (const element of document.querySelectorAll('[data-bars]')) element.setAttribute('data-bars', String(bars));
  for (const element of document.querySelectorAll('[data-meter]')) element.style.width = `${signal}%`;
}

function flushLog() {
  clearTimeout(logTimer);
  logTimer = null;
  if (!logPending) return;
  serialOutput.textContent = (serialOutput.textContent + logPending).slice(-MAX_LOG_CHARS);
  logPending = '';
  if ($('autoScroll').checked) serialOutput.scrollTop = serialOutput.scrollHeight;
}

// The 2-s STATUS beat feeds the menu, not the log. A line that may still
// become a STATUS line is held until it does or cannot.
function visibleLog(text) {
  const parts = (logLinePartial + text).split('\n');
  const tail = parts.pop();
  const isStatus = line => line.replace(/\r$/, '').startsWith('STATUS ');
  let out = parts.filter(line => !isStatus(line)).map(line => `${line}\n`).join('');
  if (tail.length <= 4096 && (tail.startsWith('STATUS ') || 'STATUS '.startsWith(tail))) {
    logLinePartial = tail;
  } else {
    out += tail;
    logLinePartial = '';
  }
  return out;
}

function appendSerial(text) {
  // Batch the log text: FIELD_DIAG and IQ dumps arrive in many small reads.
  logPending = (logPending + visibleLog(text)).slice(-MAX_LOG_CHARS);
  if (!logTimer) logTimer = setTimeout(flushLog, 50);
  for (const event of receiverConsole.push(text)) {
    if (event === 'ready') scheduleRefresh(1500);
    else if ((event === 'scan-done' || event === 'calibration-done') && !receiverConsole.reportsByItself()) scheduleRefresh(400);
  }
  renderReceiver();
  syncDeviceControls();
}

function scheduleRefresh(delay = REFRESH_DELAY_MS) {
  clearTimeout(refreshTimer);
  refreshTimer = setTimeout(async () => {
    refreshTimer = null;
    if (serialConsole.state !== 'connected') return;
    if (commandBusy) { scheduleRefresh(200); return; }
    commandBusy = true;
    try { await serialConsole.send('T'); } finally { commandBusy = false; syncDeviceControls(); }
  }, delay);
}

const serialConsole = new SerialTerminal({
  serial: navigator.serial,
  onData: appendSerial,
  onState: (state) => {
    const labels = { connected: 'Connected', connecting: 'Connecting…', disconnecting: 'Disconnecting…', disconnected: 'No device connected' };
    terminalStatus.textContent = labels[state];
    terminalStatus.setAttribute('data-state', state);
    if (state === 'connecting') {
      flushLog();
      logLinePartial = '';
      serialOutput.textContent = '';
      receiverConsole.reset();
    }
    if (state !== 'connected') {
      clearTimeout(refreshTimer);
      refreshTimer = null;
    }
    // onState can run during connect, after construction has completed.
    renderReceiver();
    syncDeviceControls();
  },
  onError: (error) => appendSerial(`\n[Web terminal] ${error.message || error}\n`)
});

// Older firmware has no STATUS beat: ask for a report every 10 s while the
// terminal is idle, so the menu never needs a refresh button.
const LEGACY_POLL_MS = 10000;
setInterval(() => {
  if (serialConsole.state !== 'connected' || !receiverConsole.current || receiverConsole.reportsByItself()) return;
  if (!refreshTimer && Date.now() - receiverConsole.statusAt >= LEGACY_POLL_MS) scheduleRefresh(0);
}, 1000);

function syncDeviceControls() {
  const supported = 'serial' in navigator;
  for (const control of [tabGithub, tabAlpha, tabLocal, selectAlphaBuild, selectAlphaPackageType, selectRelease, selectPackageType, selectBaud, chkEraseAll, inputLocalFile, inputFlashOffset, $('btnBrowseFile')]) {
    control.disabled = isFlashing;
  }
  const terminalConnected = serialConsole.state === 'connected';
  const terminalIdle = serialConsole.state === 'disconnected';
  const portBusy = !supported || isConnecting || isFlashing || (!terminalIdle && !terminalConnected);
  btnConnect.disabled = portBusy;
  btnConnect.title = terminalConnected ? 'Releases the terminal and connects the flasher' : '';
  btnTerminalConnect.disabled = portBusy;
  btnTerminalConnect.textContent = terminalConnected ? 'Disconnect' : serialConsole.state === 'connecting' ? 'Connecting…' : 'Connect receiver';
  btnTerminalConnect.title = isConnected ? 'Releases the flasher and connects the terminal' : '';
  terminalBaud.disabled = !terminalIdle;
  commandInput.disabled = !terminalConnected || commandBusy;
  $('btnSendCommand').disabled = !terminalConnected || commandBusy;
  const receiverBusy = receiverConsole.receiverBusy();
  for (const button of shortcutButtons) {
    const key = button.dataset.command;
    button.disabled = !terminalConnected || commandBusy || !receiverConsole.supports(key) || (receiverBusy && MENU_COMMAND_KEYS.has(key));
    button.title = receiverBusy && MENU_COMMAND_KEYS.has(key) ? 'The receiver ignores this until its calibration finishes'
      : button.dataset.hint || '';
  }
}

btnTerminalConnect.addEventListener('click', async () => {
  if (isConnecting || isFlashing) return;
  if (serialConsole.state === 'connected') {
    await serialConsole.disconnect();
  } else if (serialConsole.state === 'disconnected' && checkSerialSupport()) {
    // Reserve the UI while the previous flasher transport releases the port.
    isConnecting = true;
    syncDeviceControls();
    try {
      if (isConnected) await disconnectDevice();
      await serialConsole.connect(Number(terminalBaud.value));
      if (serialConsole.state === 'connected') await serialConsole.send('hT');
    } finally {
      isConnecting = false;
      syncDeviceControls();
    }
  }
});

async function sendTerminalCommand(command) {
  if (commandBusy || serialConsole.state !== 'connected') return false;
  const button = shortcutButtons.find(b => b.dataset.command === command);
  if (command.length !== 1 || (!button && command !== '?') || !receiverConsole.supports(command)) {
    appendSerial('\n[Web terminal] Unsupported key. Use h for help; the menu needs C5VRX-5 firmware.\n');
    return false;
  }
  commandBusy = true;
  syncDeviceControls();
  let sent = false;
  try {
    sent = await serialConsole.send(command);
  } finally {
    commandBusy = false;
    syncDeviceControls();
  }
  if (sent && command === 's') receiverConsole.startScan();
  // Firmware with the STATUS beat shows the effect within 2 s by itself.
  if (sent && REFRESH_AFTER.has(command) && !receiverConsole.reportsByItself()) scheduleRefresh();
  if (sent) renderReceiver();
  return sent;
}

for (const button of shortcutButtons) {
  button.addEventListener('click', () => sendTerminalCommand(button.dataset.command));
}

$('commandForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const command = commandInput.value;
  if (!command) return;
  if (await sendTerminalCommand(command)) commandInput.value = '';
});
$('clear').addEventListener('click', () => {
  logPending = '';
  serialOutput.textContent = '';
});
$('downloadLog').addEventListener('click', () => {
  flushLog();
  const url = URL.createObjectURL(new Blob([serialOutput.textContent], { type: 'text/plain;charset=utf-8' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = `c5vrx-${new Date().toISOString().replace(/[:.]/g, '-')}.log`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

// Receiver menu pages: browsing never sends anything.
const menuTabs = [...document.querySelectorAll('[data-menu]')];
function selectMenu(tab, focus = false) {
  for (const item of menuTabs) {
    const active = item === tab;
    item.setAttribute('aria-selected', String(active));
    item.tabIndex = active ? 0 : -1;
    $(`pane-${item.dataset.menu}`).hidden = !active;
  }
  if (focus) tab.focus();
}
const MENU_KEYS = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1 };
for (const tab of menuTabs) {
  tab.addEventListener('click', () => selectMenu(tab));
  tab.addEventListener('keydown', (event) => {
    const index = menuTabs.indexOf(tab);
    const target = event.key in MENU_KEYS ? (index + MENU_KEYS[event.key] + menuTabs.length) % menuTabs.length
      : event.key === 'Home' ? 0 : event.key === 'End' ? menuTabs.length - 1 : -1;
    if (target < 0) return;
    event.preventDefault();
    selectMenu(menuTabs[target], true);
  });
}

// Firmware / Terminal views; #terminal links straight to the terminal.
function showView(terminalView) {
  $('firmwareView').hidden = terminalView;
  $('terminalView').hidden = !terminalView;
  for (const [id, active] of [['firmwareTab', !terminalView], ['terminalTab', terminalView]]) {
    $(id).classList.toggle('active', active);
    $(id).setAttribute('aria-pressed', String(active));
  }
  if (globalThis.history?.replaceState) history.replaceState(null, '', terminalView ? '#terminal' : location.pathname + location.search);
}
$('firmwareTab').addEventListener('click', () => showView(false));
$('terminalTab').addEventListener('click', () => showView(true));

navigator.serial?.addEventListener('disconnect', async (event) => {
  if (event.target === serialConsole.port) await serialConsole.disconnect();
  if (event.target === port && !isFlashing && !isConnecting) await disconnectDevice();
});

// Keep docs visible on desktop and collapsible on smaller screens.
const docsBreakpoint = window.matchMedia('(min-width:801px)');
const docsNav = $('docsNav');
docsNav.open = docsBreakpoint.matches;
docsBreakpoint.addEventListener('change', (event) => { docsNav.open = event.matches; });

document.addEventListener('DOMContentLoaded', () => {
  log('C5VRX web flasher ready.');
  checkSerialSupport();
  if (globalThis.location?.hash === '#terminal') showView(true);
  renderReceiver();
  syncDeviceControls();
  fetchReleases();
});
