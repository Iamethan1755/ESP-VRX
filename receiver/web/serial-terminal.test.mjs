import test from 'node:test';
import assert from 'node:assert/strict';
import { SerialTerminal } from './serial-terminal.js';

const tick = () => new Promise(resolve => setImmediate(resolve));
function fixture() {
  const writes = [], states = [], errors = [], data = [];
  let input;
  const port = {
    closed: false,
    readable: new ReadableStream({ start(controller) { input = controller; } }),
    writable: new WritableStream({ write(bytes) { writes.push([...bytes]); } }),
    async open(options) { this.options = options; },
    async close() {
      assert.equal(this.readable.locked, false);
      assert.equal(this.writable.locked, false);
      this.closed = true;
    },
    setSignals() { throw new Error('Terminal must not reset the receiver'); }
  };
  const terminal = new SerialTerminal({
    serial: { async requestPort() { return port; } },
    onData: text => data.push(text),
    onState: state => states.push(state),
    onError: error => errors.push(error)
  });
  return { terminal, port, writes, states, errors, data, input };
}

test('raw command bytes, UTF-8 across reads, baud, and clean disconnect', async () => {
  const f = fixture();
  await f.terminal.connect(115200);
  assert.equal(f.port.options.baudRate, 115200);
  assert.equal(await f.terminal.send('c'), true);
  await f.terminal.send('\r');
  await f.terminal.send('p\r\n');
  assert.deepEqual(f.writes, [[99], [13], [112, 13, 10]]);
  const bytes = new TextEncoder().encode('RF → ready');
  f.input.enqueue(bytes.slice(0, 4));
  f.input.enqueue(bytes.slice(4));
  await tick();
  assert.equal(f.data.join(''), 'RF → ready');
  await Promise.all([f.terminal.disconnect(), f.terminal.disconnect()]);
  assert.equal(f.terminal.state, 'disconnected');
  assert.equal(f.port.closed, true);
  assert.equal(await f.terminal.send('c'), false);
  assert.deepEqual(f.states, ['connecting', 'connected', 'disconnecting', 'disconnected']);
  assert.equal(f.errors.length, 0);
});

test('port-picker cancellation releases UI without a fake connection', async () => {
  const states = [], errors = [];
  const terminal = new SerialTerminal({
    serial: { async requestPort() { throw new DOMException('Cancelled', 'NotFoundError'); } },
    onData() {}, onState: s => states.push(s), onError: e => errors.push(e)
  });
  await terminal.connect(115200);
  assert.equal(terminal.state, 'disconnected');
  assert.equal(terminal.port, null);
  assert.equal(errors.length, 0);
  assert.deepEqual(states, ['connecting', 'disconnecting', 'disconnected']);
});

test('open failure cleans up and a fresh connection can be attempted', async () => {
  const f = fixture();
  const open = f.port.open;
  f.port.open = async () => { throw new Error('Port busy'); };
  await f.terminal.connect(115200);
  assert.equal(f.terminal.state, 'disconnected');
  assert.equal(f.errors[0].message, 'Port busy');
  f.port.open = open;
  await f.terminal.connect(460800);
  assert.equal(f.terminal.state, 'connected');
  await f.terminal.disconnect();
});

test('reader EOF releases stream locks and closes the connection', async () => {
  const f = fixture();
  await f.terminal.connect(115200);
  f.input.close();
  await tick();
  assert.equal(f.terminal.state, 'disconnected');
  assert.equal(f.port.closed, true);
});

test('USB read failure cannot loop forever on the same errored stream', async () => {
  const f = fixture();
  await f.terminal.connect(115200);
  f.input.error(new Error('USB unplugged'));
  await tick();
  assert.equal(f.terminal.state, 'disconnected');
  assert.equal(f.errors.length, 1);
  assert.equal(f.port.closed, true);
});

test('write failure disconnects and returns false', async () => {
  const f = fixture();
  f.port.writable = new WritableStream({ write() { throw new Error('USB write failed'); } });
  await f.terminal.connect(115200);
  assert.equal(await f.terminal.send('p'), false);
  assert.equal(f.terminal.state, 'disconnected');
  assert.equal(f.errors[0].message, 'USB write failed');
});

test('a second connect while port selection is pending cannot steal the port', async () => {
  const f = fixture();
  let resolve;
  let requests = 0;
  f.terminal.serial.requestPort = () => { requests++; return new Promise(r => { resolve = r; }); };
  const pending = f.terminal.connect(115200);
  await f.terminal.connect(460800);
  assert.equal(requests, 1);
  resolve(f.port);
  await pending;
  assert.equal(f.port.options.baudRate, 115200);
  await f.terminal.disconnect();
});

test('a recoverable read error resumes from a replacement USB stream', async () => {
  const f = fixture();
  await f.terminal.connect(115200);
  let nextInput;
  f.port.readable = new ReadableStream({ start(controller) { nextInput = controller; } });
  f.input.error(new Error('Framing error'));
  await tick();
  nextInput.enqueue(new TextEncoder().encode('recovered'));
  await tick();
  assert.equal(f.data.join(''), 'recovered');
  assert.equal(f.terminal.state, 'connected');
  await f.terminal.disconnect();
});

test('a reconnect after unplug uses a fresh port and releases its locks', async () => {
  const f = fixture();
  await f.terminal.connect(115200);
  f.input.error(new Error('Unplugged'));
  await tick();
  const fresh = fixture();
  f.terminal.serial.requestPort = async () => fresh.port;
  await f.terminal.connect(115200);
  assert.equal(f.terminal.port, fresh.port);
  assert.equal(await f.terminal.send('d'), true);
  assert.deepEqual(fresh.writes, [[100]]);
  await f.terminal.disconnect();
  assert.equal(fresh.port.closed, true);
});

test('disconnect during port opening cannot leave an open USB port behind', async () => {
  const f = fixture();
  let finishOpen;
  f.port.open = () => new Promise(resolve => { finishOpen = resolve; });
  const opening = f.terminal.connect(115200);
  await tick();
  await f.terminal.disconnect();
  finishOpen();
  await opening;
  assert.equal(f.terminal.state, 'disconnected');
  assert.equal(f.port.closed, true);
  assert.equal(f.terminal.writer, null);
});
