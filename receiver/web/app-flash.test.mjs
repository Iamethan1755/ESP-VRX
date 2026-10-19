import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { ReceiverConsole, MENU_COMMAND_KEYS } from './receiver-console.js';

// Exercise the actual click handler with a mock loader: never touch USB hardware.
const source = fs.readFileSync(process.env.APP_SOURCE_PATH || new URL('./app.js', import.meta.url), 'utf8').replace(/^import .*;\r?\n/gm, '');
function harness(sourceName, packageType = 'merged', confirmed = true) {
  const elements = new Map();
  const get = id => {
    if (!elements.has(id)) elements.set(id, {
      style: {}, handlers: {}, value: '', textContent: '', checked: false,
      classList: { toggle() {}, add() {}, remove() {} },
      addEventListener(name, fn) { this.handlers[name] = fn; },
      setAttribute() {}, appendChild() {}, replaceChildren() {}
    });
    return elements.get(id);
  };
  const html = fs.readFileSync(new URL('./index.html', import.meta.url), 'utf8');
  const shortcuts = [...html.matchAll(/<button[^>]*data-command="([^"]+)"([^>]*)>/g)].map((match, i) => {
    const element = get(`shortcut-${i}`);
    element.dataset = { command: match[1], hint: match[2].match(/data-hint="([^"]+)"/)?.[1] };
    return element;
  });
  const downloads = [], flashes = [], alerts = [], commands = [], lifecycle = [];
  const context = vm.createContext({
    console, URL, Date, TextDecoder, TextEncoder, ReceiverConsole, MENU_COMMAND_KEYS, setTimeout, clearTimeout,
    setInterval: () => 0, clearInterval() {},
    document: { baseURI: 'https://c5vrx.com/', getElementById: get, visibilityState: 'visible',
      querySelectorAll: selector => selector === '[data-command]' ? shortcuts : [], addEventListener() {}, createElement: () => get('option') },
    navigator: { serial: { addEventListener() {} } },
    window: { confirm: () => confirmed, matchMedia: () => ({ matches: true, addEventListener() {} }) },
    SerialTerminal: class {
      constructor(options) { this.state = 'disconnected'; this.options = options; }
      async connect() { lifecycle.push('terminal connect'); this.state = 'connected'; this.options.onState(this.state); }
      async disconnect() { lifecycle.push('terminal disconnect'); this.state = 'disconnected'; this.options.onState(this.state); }
      async send(text) { commands.push(text); return true; }
    },
    alert: message => alerts.push(message),
    fetch: async url => { downloads.push(url); return { ok: true, arrayBuffer: async () => new Uint8Array([1, 2, 3, 4]).buffer }; },
    mockLoader: { writeFlash: async options => flashes.push(options), after: async () => {} }
  });
  vm.runInContext(source, context);
  for (const id of ['selectRelease', 'selectAlphaBuild']) get(id).value = '0';
  for (const id of ['selectPackageType', 'selectAlphaPackageType']) get(id).value = packageType;
  vm.runInContext(`
    activeSource = ${JSON.stringify(sourceName)};
    isConnected = true; esploader = mockLoader;
    const rel = { tag_name: 'c5vrx5-pr-1', prerelease: true, assets: [
      { name: 'bootloader.bin', local_url: 'firmware/alpha/bootloader.bin', size: 4 },
      { name: 'partition-table.bin', local_url: 'firmware/alpha/partition-table.bin', size: 4 },
      { name: 'c5vrx5.bin', local_url: 'firmware/alpha/c5vrx5.bin', size: 4 },
      { name: 'c5vrx5_merged.bin', local_url: 'firmware/alpha/c5vrx5_merged.bin', size: 4 }
    ] };
    githubReleases = [rel]; githubAlphaBuilds = [rel];
  `, context);
  return { get, context, downloads, flashes, alerts, commands, lifecycle, shortcuts, click: () => get('btnFlash').handlers.click() };
}

for (const remote of ['github', 'alpha']) {
  test(`${remote} Full firmware downloads the merged binary at 0x0`, async () => {
    const h = harness(remote);
    await h.click();
    assert.deepEqual(h.downloads, ['https://c5vrx.com/firmware/alpha/c5vrx5_merged.bin']);
    assert.equal(h.flashes.length, 1);
    assert.equal(h.flashes[0].fileArray[0].address, 0);
    assert.ok(!h.alerts.some(x => x.startsWith('Flashing error:')));
  });
}
test('alpha App only downloads the application at 0x10000', async () => {
  const h = harness('alpha', 'app_only');
  await h.click();
  assert.deepEqual(h.downloads, ['https://c5vrx.com/firmware/alpha/c5vrx5.bin']);
  assert.equal(h.flashes[0].fileArray[0].address, 0x10000);
});
test('alpha Full firmware falls back to all three flash parts', async () => {
  const h = harness('alpha');
  vm.runInContext("githubAlphaBuilds[0].assets = githubAlphaBuilds[0].assets.filter(a => !a.name.includes('merged'))", h.context);
  await h.click();
  assert.equal(h.downloads.length, 3);
  assert.deepEqual(Array.from(h.flashes[0].fileArray, part => part.address), [0x2000, 0x8000, 0x10000]);
});
test('cancelled alpha confirmation never downloads or flashes', async () => {
  const h = harness('alpha', 'merged', false);
  await h.click();
  assert.equal(h.downloads.length, 0);
  assert.equal(h.flashes.length, 0);
});
test('only Local File requires a local binary', async () => {
  const h = harness('local');
  await h.click();
  assert.equal(h.downloads.length, 0);
  assert.equal(h.flashes.length, 0);
  assert.ok(h.alerts.includes('Flashing error: No local binary selected'));
});

test('terminal connection releases flasher before identifying firmware', async () => {
  const h = harness('alpha');
  vm.runInContext("transport = { disconnect: async () => { log('released'); } }", h.context);
  await h.get('btnTerminalConnect').handlers.click();
  assert.equal(vm.runInContext('isConnected', h.context), false);
  assert.deepEqual(h.commands, ['hT']);
  assert.equal(h.get('btnConnect').disabled, false);
});

test('menu buttons and typed keys send at once: no browser popup, no Enter', async () => {
  const h = harness('alpha', 'merged', false);   // a confirm() answer of "cancel" must not matter
  vm.runInContext("isConnected = false; serialConsole.state = 'connected'; receiverConsole.push('C5VRX-5 keys: T status\\n');", h.context);
  const calibrate = h.shortcuts.find(b => b.dataset.command === 'F');
  await calibrate.handlers.click();
  assert.deepEqual(h.commands, ['F']);
  h.get('commandInput').value = 'F';
  await h.get('commandForm').handlers.submit({ preventDefault() {} });
  assert.deepEqual(h.commands, ['F', 'F']);
  h.get('commandInput').value = 'Fg';
  await h.get('commandForm').handlers.submit({ preventDefault() {} });
  assert.deepEqual(h.commands, ['F', 'F']);
  vm.runInContext('syncDeviceControls()', h.context);
  assert.match(calibrate.title, /Live video stops/, 'the warning stays visible as a tooltip');
});

test('the STATUS beat updates the menu by itself and stays out of the log', async () => {
  const h = harness('alpha');
  vm.runInContext("isConnected = false; serialConsole.state = 'connected'", h.context);
  vm.runInContext("appendSerial('C5VRX5 version=v5.0.9 demod=U85Q\\nSTATUS channel=F4 freq=5800 afc_locked=1 standard=AUTO detected=PAL signal=71 menu=0 mode=WIDE kp_auto=2 output=1.5x live=1 busy=0\\nHB t_s=4\\nSTAT')", h.context);
  vm.runInContext("appendSerial('US channel=F4 freq=5800 signal=72 busy=0\\n'); flushLog()", h.context);
  const shown = h.get('output').textContent;
  assert.doesNotMatch(shown, /STATUS/);
  assert.match(shown, /HB t_s=4/);
  const view = vm.runInContext('receiverConsole.view()', h.context);
  assert.equal(view.channel, 'F4');
  assert.equal(view.signal, 72);
  assert.equal(view.output, '1.5x');
  assert.equal(view.profile, 'WIDE');
  await h.shortcuts.find(b => b.dataset.command === 'c').handlers.click();
  await new Promise(resolve => setTimeout(resolve, 800));
  assert.deepEqual(h.commands, ['c'], 'with the beat no extra status report is requested');
});

test('legacy identity leaves current shortcuts disabled and help usable', () => {
  const h = harness('alpha');
  vm.runInContext("isConnected = false; serialConsole.state = 'connected'; appendSerial('C5VRX-3 ready\\n');", h.context);
  assert.equal(h.shortcuts.find(b => b.dataset.command === 'K').disabled, true);
  assert.equal(h.shortcuts.find(b => b.dataset.command === 'h').disabled, false);
});


test('a menu command is followed by one status refresh; browsing sends nothing', async () => {
  const h = harness('alpha');
  vm.runInContext("isConnected = false; serialConsole.state = 'connected'; receiverConsole.push('C5VRX-5 keys: T status\\n');", h.context);
  await h.shortcuts.find(b => b.dataset.command === 'c').handlers.click();
  assert.deepEqual(h.commands, ['c']);
  await new Promise(resolve => setTimeout(resolve, 800));
  assert.deepEqual(h.commands, ['c', 'T']);
  await h.shortcuts.find(b => b.dataset.command === 'n').handlers.click();
  await new Promise(resolve => setTimeout(resolve, 800));
  assert.deepEqual(h.commands, ['c', 'T', 'n'], 'moving the goggle cursor changes no reported value');
});

test('Pages deployment fingerprints every module app.js imports', () => {
  const app = fs.readFileSync(new URL('./app.js', import.meta.url), 'utf8');
  const script = fs.readFileSync(new URL('../tools/prepare_pages_site.sh', import.meta.url), 'utf8');
  const listed = script.match(/for name in \(([^)]*)\):/)[1];
  for (const [, name] of app.matchAll(/^import .* from '\.\/([^']+)';/gm)) {
    assert.ok(listed.includes(`"${name}"`), `${name} would be served from a stale cache`);
  }
});

test('Local File refuses an offset that would overwrite the wrong sector', async () => {
  const h = harness('local');
  vm.runInContext('localFileBinary = new Uint8Array([1, 2, 3, 4]).buffer;', h.context);
  for (const offset of ['0x10', 'zz']) {
    h.get('inputFlashOffset').value = offset;
    await h.click();
  }
  assert.equal(h.flashes.length, 0);
  h.get('inputFlashOffset').value = '0x10000';
  await h.click();
  assert.equal(h.flashes[0].fileArray[0].address, 0x10000);
});

test('menu buttons are disabled while the receiver is busy, diagnostics stay usable', () => {
  const h = harness('alpha');
  vm.runInContext("isConnected = false; serialConsole.state = 'connected'; appendSerial('C5VRX-5 keys: T\\nHB t_s=1 stage=ready gain=40 menu=0 startup=1 busy=1\\n');", h.context);
  const key = k => h.shortcuts.find(b => b.dataset.command === k);
  assert.equal(key('c').disabled, true);
  assert.equal(key('D').disabled, false);
  assert.equal(key('T').disabled, false);
  vm.runInContext("appendSerial('HB t_s=6 stage=ready gain=40 menu=0 startup=0 busy=0\\n');", h.context);
  assert.equal(key('c').disabled, false);
});

test('controls stay locked until the post-flash disconnect completes', async () => {
  const h = harness('alpha');
  let release;
  vm.runInContext('transport = { disconnect: () => new Promise(r => { globalThis.releaseDisconnect = r; }) };', h.context);
  const done = h.click();
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(vm.runInContext('isFlashing', h.context), true);
  assert.equal(h.get('btnTerminalConnect').disabled, true);
  release = vm.runInContext('releaseDisconnect', h.context);
  release();
  await done;
  assert.equal(vm.runInContext('isFlashing || isConnected', h.context), false);
  assert.equal(h.get('btnTerminalConnect').disabled, false);
});
