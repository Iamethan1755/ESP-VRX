import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { ReceiverConsole } from './receiver-console.js';

test('firmware identity and status survive arbitrary USB chunk boundaries', () => {
  const state = new ReceiverConsole();
  const report = 'C5VRX5 version=v5.0.123 demod=U85Q\r\nCONTROL channel=R1 freq=5658 standard=AUTO detected=PAL signal=71 menu=0\r\nOUTPUT level=1.5x live=1\r\n';
  for (const ch of report) state.push(ch);
  assert.equal(state.version, 'v5.0.123');
  assert.equal(state.control.channel, 'R1');
  assert.equal(state.control.signal, '71');
  assert.equal(state.output.level, '1.5x');
  assert.equal(state.supports('c'), true);
  assert.equal(state.supports('v'), false);
  state.push('  v video standard AUTO/NTSC/PAL, e close menu (web menu direct controls),\n');
  assert.equal(state.supports('v'), true);
  assert.equal(state.supports('e'), true);
});

test('unknown/legacy firmware cannot enable current receiver actions', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX-3 ready\nCONTROL channel=R1 freq=5658\n');
  assert.equal(state.supports('K'), false);
  assert.equal(state.supports('F'), false);
  assert.equal(state.supports('h'), true);
  assert.equal(state.supports('T'), true);
  assert.deepEqual(state.control, {});
  state.push('C5VRX-5 keys: T status\n');
  assert.equal(state.supports('K'), true);
});

test('reconnect discards firmware capabilities and previous settings', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX5 v5.0.123 ready: R1 5658 MHz\n  v video standard AUTO/NTSC/PAL, e close menu\nOUTPUT level=2.5x\n');
  state.reset();
  assert.equal(state.supports('v'), false);
  assert.equal(state.current, false);
  assert.deepEqual(state.output, {});
  state.push('x'.repeat(20000));
  assert.equal(state.pending.length, 4096);
});

test('every web shortcut is an actual current console byte, with its warning as a hint', () => {
  const html = fs.readFileSync(new URL('./index.html', import.meta.url), 'utf8');
  const consoleSource = fs.readFileSync(new URL('../main/console.c', import.meta.url), 'utf8');
  const accepted = new Set([...consoleSource.matchAll(/(?:case|c ==) '([^'])'/g)].map(m => m[1]));
  const buttons = [...html.matchAll(/<button[^>]*data-command="([^"]+)"[^>]*>/g)];
  assert.ok(buttons.length > 15);
  for (const [markup, key] of buttons) {
    assert.equal(key.length, 1);
    assert.ok(accepted.has(key), `Firmware does not accept ${key}`);
    if (['c', 'b', 's', 'o', 'x', 'v', 'K', 'k', 'j', 'P', 'g', 'A', 'F', 'L', '`'].includes(key))
      assert.match(markup, /data-hint=/, `${key} requires its action warning`);
    assert.doesNotMatch(markup, /data-confirm=/, 'menu commands never open a browser popup');
  }
});

test('command acknowledgements update the menu before the next status report', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX5 version=v5.0.9 demod=U85Q\nCONTROL channel=R1 freq=5658 afc_locked=1 standard=AUTO detected=PAL signal=63 menu=0 mode=DEFAULT kp_auto=2\nOUTPUT level=1.0x live=1\n');
  assert.deepEqual(state.push('[CHANNEL] A1 5865 MHz\n'), ['changed']);
  let view = state.view();
  assert.equal(view.channel, 'A1');
  assert.equal(view.band, 'BOSCAM A');
  assert.equal(view.signal, null, 'a retune invalidates the old signal');
  state.push('OUTPUT manual 2.0x result=applied wait_ms=0\nOUTPUT manual 2.5x result=refused wait_ms=4000\n');
  assert.equal(state.view().output, '2.0x', 'a refused change keeps the applied level');
  state.push('KP ladder AUTO kp (brief restarts; bench test)\nPROFILE live switch OFF (saved): report only\n');
  view = state.view();
  assert.equal(view.kpMode, 'AUTO kp');
  assert.equal(view.liveProfile, false);
  state.push('KP ladder AUTO kp + LP (program changes briefly restart video; bench test)\n');
  assert.equal(state.view().kpMode, 'AUTO kp+LP');
});

test('heartbeat tracks the screen menu and reports the end of a calibration', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX-5 keys: T status\nCONTROL channel=R1 freq=5658 menu=0\n');
  assert.deepEqual(state.push('HB t_s=5 stage=live gain=40 menu=1 startup=1\n'), []);
  assert.equal(state.view().menu, true);
  assert.equal(state.view().calibrating, true);
  assert.deepEqual(state.push('HB t_s=10 stage=live gain=40 menu=0 startup=0\n'), ['calibration-done']);
  assert.equal(state.view().menu, false);
  state.push('[MENU] closed after 30 s idle\n');
  assert.equal(state.view().menu, false);
});

test('scan progress and result come from the scan lines', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX-5 keys: T status\n');
  state.startScan();
  assert.match(state.view().scan, /^SCANNING · 0/);
  state.push('SCAN candidate R3 5732 MHz video=80 offset_khz=200 quality=50 PAL\n');
  assert.match(state.view().scan, /^SCANNING · 1/);
  assert.deepEqual(state.push('SCAN result=found channel=R3\n'), ['scan-done']);
  assert.equal(state.view().scan, 'FOUND R3');
  state.push('SCAN result=none_kept channel=A1\n');
  assert.equal(state.view().scan, 'NO VIDEO FOUND · KEPT A1');
});

test('a reboot discards values the firmware resets', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX5 version=v5.0.9\nCONTROL channel=R1 freq=5658 menu=1\nOUTPUT level=2.5x\n');
  assert.deepEqual(state.push('C5VRX5 v5.0.9 ready: R1 5658 MHz, U85Q\n'), ['ready']);
  assert.equal(state.view().output, '');
  assert.equal(state.view().menu, null);
});

test('a busy receiver is reported, its refusals shown and its end refreshes', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX-5 keys: T status\nCONTROL channel=R1 freq=5658 menu=0 busy=0\n');
  assert.equal(state.receiverBusy(), false);
  assert.deepEqual(state.push('COMMAND c refused=busy (a calibration owns the picture)\n'), ['refused']);
  assert.equal(state.view().busy, true);
  assert.equal(state.view().refused, 'c');
  assert.deepEqual(state.push('HB t_s=5 stage=ready gain=40 menu=0 startup=0 busy=0\n'), ['calibration-done']);
  assert.equal(state.view().busy, false);
  assert.equal(state.view().refused, '');
});

test('the busy field wins over the startup flag; old firmware falls back to it', () => {
  const current = new ReceiverConsole();
  current.push('C5VRX-5 keys: T\nHB t_s=5 stage=ready gain=40 menu=0 startup=1 busy=0\n');
  assert.equal(current.receiverBusy(), false);
  const older = new ReceiverConsole();
  older.push('C5VRX-5 keys: T\nHB t_s=5 stage=ready gain=40 menu=0 startup=1\n');
  assert.equal(older.receiverBusy(), true);
});

test('busy-gated keys are exactly the keys the console routes to command()', async () => {
  const { MENU_COMMAND_KEYS } = await import('./receiver-console.js');
  const consoleSource = fs.readFileSync(new URL('../main/console.c', import.meta.url), 'utf8');
  const routed = consoleSource.match(/case 'o':[^\n]*/)[0];
  const keys = new Set([...routed.matchAll(/case '([^'])'/g)].map(m => m[1]));
  assert.deepEqual([...MENU_COMMAND_KEYS].sort(), [...keys].sort());
});

test('STATUS beat fills the menu and marks firmware that reports by itself', () => {
  const state = new ReceiverConsole();
  state.push('C5VRX5 version=v5.0.9 demod=U85Q\n', 1000);
  assert.equal(state.reportsByItself(1000), false);
  const events = state.push('STATUS channel=R1 freq=5658 afc_locked=0 standard=PAL detected=none signal=12 menu=1 mode=DEFAULT kp_auto=1 output=2.0x live=0 busy=1\n', 2000);
  assert.ok(events.includes('status'));
  const view = state.view();
  assert.equal(view.channel, 'R1');
  assert.equal(view.locked, false);
  assert.equal(view.output, '2.0x');
  assert.equal(view.liveProfile, false);
  assert.equal(view.kpMode, 'AUTO kp');
  assert.equal(view.busy, true);
  assert.equal(state.reportsByItself(5000), true);
  assert.equal(state.reportsByItself(9000), false, 'a silent receiver falls back to status requests');
});
