// Passive UI state only. USB/status never controls the realtime video path.
// Parses the C5VRX-5 console (main/console.c, control.c, transport.c,
// profile.c) into the values the web receiver menu shows.

export const BAND_NAMES = {
  R: 'RACEBAND R', A: 'BOSCAM A', B: 'BOSCAM B', E: 'BOSCAM E', F: 'FATSHARK F', L: 'LOWBAND L'
};
export const KP_MODES = ['OFF', 'AUTO kp', 'AUTO kp+LP'];
// Keys the firmware routes through control.c command(): ignored while a
// calibration/lane-test status screen owns the picture.
export const MENU_COMMAND_KEYS = new Set(['o', 'e', 'v', 'n', 'x', 'c', 'b', 's', 'K', 'k', 'j', 'P', 'g', 'A']);

const FIELDS = /(?:^|\s)(\w+)=([^\s]+)/g;
const fields = (line, target = {}) => {
  for (const match of line.matchAll(FIELDS)) target[match[1]] = match[2];
  return target;
};

export class ReceiverConsole {
  constructor() { this.reset(); }

  reset() {
    this.pending = '';
    this.version = '';
    this.current = false;
    this.directMenu = false;
    this.control = {};
    this.output = {};
    this.heartbeat = {};
    this.profile = {};
    this.scan = { active: false, candidates: 0, result: '', channel: '' };
    this.statusAt = 0;
    this.beatAt = 0;     // last STATUS beat: the firmware reports by itself
    this.busy = false;
    this.busyReported = false;
    this.refused = '';
  }

  // Returns the events in this chunk that make a fresh status report useful:
  // 'ready' (boot line), 'status' (CONTROL received), 'scan-done',
  // 'calibration-done' and 'changed' (a command acknowledged a change).
  push(chunk, now = Date.now()) {
    const events = new Set();
    const lines = (this.pending + chunk).split('\n');
    this.pending = lines.pop().slice(-4096);
    for (const raw of lines) this.line(raw.replace(/\r/g, ''), now, events);
    return [...events];
  }

  line(line, now, events) {
    const version = line.match(/^C5VRX5 (?:version=(\S+)|(\S+) ready:)/);
    if (version) {
      this.current = true;
      this.version = version[1] || version[2];
      if (version[2]) {
        // A reboot resets OUTPUT to 1.0x and closes the menu; old values lie.
        this.control = {};
        this.output = {};
        this.scan = { active: false, candidates: 0, result: '', channel: '' };
        this.refused = '';
        events.add('ready');
      }
    }
    if (line.startsWith('C5VRX-5 keys:')) this.current = true;
    if (/^\s+v video standard AUTO\/NTSC\/PAL, e close menu/.test(line)) this.directMenu = true;
    if (!this.current) return;

    if (line.startsWith('STATUS ')) {
      // Status beat (control.c control_print_beat, every 2 s).
      const beat = fields(line);
      if (beat.output) this.output.level = beat.output;
      if (beat.live !== undefined) this.profile.live = beat.live === '1';
      delete beat.output;
      delete beat.live;
      Object.assign(this.control, beat);
      if (beat.busy !== undefined) this.setBusy(beat.busy === '1', events);
      this.statusAt = this.beatAt = now;
      events.add('status');
    } else if (line.startsWith('CONTROL ')) {
      fields(line, this.control);
      if (this.control.busy !== undefined) this.setBusy(this.control.busy === '1', events);
      this.statusAt = now;
      events.add('status');
    } else if (line.startsWith('OUTPUT level=')) {
      fields(line, this.output);
    } else if (line.startsWith('PROFILE monitor ')) {
      this.profile.live = fields(line).mode === 'live_switch';
    } else if (line.startsWith('PROFILE active=')) {
      this.profile.active = fields(line).active;
    } else if (line.startsWith('HB ')) {
      const hb = fields(line);
      const wasStartup = this.heartbeat.startup === '1';
      this.heartbeat = hb;
      if (hb.menu !== undefined && this.control.channel) this.control.menu = hb.menu;
      if (wasStartup && hb.startup === '0') events.add('calibration-done');
      if (hb.busy !== undefined) this.setBusy(hb.busy === '1', events);
    } else {
      this.acknowledgement(line, events);
    }
  }

  setBusy(busy, events) {
    if (this.busy && !busy) events.add('calibration-done');
    this.busy = busy;
    this.busyReported = true;
    if (!busy) this.refused = '';
  }

  // Older firmware has no busy field: its startup flag is the best hint.
  receiverBusy() {
    return this.busy || (!this.busyReported && this.heartbeat.startup === '1');
  }

  acknowledgement(line, events) {
    let match;
    if ((match = line.match(/^COMMAND (\S) refused=busy/))) {
      this.busy = true;
      this.refused = match[1];
      events.add('refused');
    } else if ((match = line.match(/^\[CHANNEL\] (\S+) (\d+) MHz/))) {
      Object.assign(this.control, { channel: match[1], freq: match[2], afc_locked: '0', signal: undefined, detected: undefined });
      events.add('changed');
    } else if ((match = line.match(/^OUTPUT manual (\S+) result=applied/))) {
      this.output.level = match[1];
      events.add('changed');
    } else if ((match = line.match(/^KP ladder (OFF|AUTO kp \+ LP|AUTO kp)/))) {
      this.control.kp_auto = match[1] === 'OFF' ? '0' : match[1] === 'AUTO kp' ? '1' : '2';
      events.add('changed');
    } else if ((match = line.match(/^PROFILE live switch (ON|OFF)/))) {
      this.profile.live = match[1] === 'ON';
    } else if (line.startsWith('[MENU] closed')) {
      this.control.menu = '0';
    } else if ((match = line.match(/^\[VIDEO\] locked (\S+) (\d+) MHz (PAL|NTSC)/))) {
      Object.assign(this.control, { channel: match[1], freq: match[2], detected: match[3], afc_locked: '1' });
    } else if (line.startsWith('SCAN candidate ')) {
      if (!this.scan.active) this.scan = { active: true, candidates: 0, result: '', channel: '' };
      this.scan.candidates += 1;
    } else if ((match = line.match(/^SCAN result=(\S+) channel=(\S+)/))) {
      this.scan = { active: false, candidates: this.scan.candidates, result: match[1], channel: match[2] };
      events.add('scan-done');
    } else if (line.startsWith('SCAN refused')) {
      this.scan = { active: false, candidates: 0, result: 'refused', channel: '' };
    }
  }

  // Firmware with the STATUS beat keeps the menu current by itself.
  reportsByItself(now = Date.now()) {
    return this.beatAt !== 0 && now - this.beatAt < 6000;
  }

  // The web menu sent s: SCAN candidate lines only name channels with video.
  startScan() {
    if (this.current) this.scan = { active: true, candidates: 0, result: '', channel: '' };
  }

  // The menu shows what the firmware last reported, nothing inferred.
  view() {
    const c = this.control;
    const band = c.channel ? c.channel.replace(/[0-9]+$/, '') : '';
    const signal = Number.parseInt(c.signal, 10);
    const kp = Number.parseInt(c.kp_auto, 10);
    const scan = this.scan;
    return {
      channel: c.channel || '',
      freq: c.freq || '',
      band: band ? BAND_NAMES[band] || band : '',
      signal: Number.isFinite(signal) ? Math.max(0, Math.min(100, signal)) : null,
      locked: c.afc_locked === undefined ? null : c.afc_locked === '1',
      standard: c.standard || '',
      detected: c.detected === undefined ? '' : c.detected === 'none' ? 'NO VIDEO' : c.detected,
      output: this.output.level || '',
      menu: c.menu === undefined ? null : c.menu === '1',
      calibrating: this.heartbeat.startup === '1',
      busy: this.receiverBusy(),
      refused: this.refused,
      stage: this.heartbeat.stage || '',
      gain: this.heartbeat.gain || '',
      kpMode: Number.isInteger(kp) ? KP_MODES[kp] || '' : '',
      profile: c.mode || this.profile.active || '',
      liveProfile: this.profile.live,
      scan: scan.active ? `SCANNING · ${scan.candidates} WITH VIDEO`
        : scan.result === 'found' ? `FOUND ${scan.channel}`
          : scan.result === 'none_kept' ? `NO VIDEO FOUND · KEPT ${scan.channel}`
            : scan.result === 'refused' ? 'SCAN REFUSED' : 'READY',
      statusAt: this.statusAt
    };
  }

  supports(key) {
    return ['T', 'h', '?'].includes(key) || (this.current && (!['v', 'e'].includes(key) || this.directMenu));
  }
}
