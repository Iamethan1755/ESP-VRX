// The firmware console reads individual bytes. Shortcuts must not append Enter.
export class SerialTerminal {
  constructor({ serial, onData, onState, onError }) {
    this.serial = serial;
    this.onData = onData;
    this.onState = onState;
    this.onError = onError;
    this.state = 'disconnected';
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.readTask = null;
    this.closeTask = null;
    this.opened = false;
    this.ending = false;
  }

  setState(state) {
    this.state = state;
    this.onState(state);
  }

  async connect(baudRate) {
    if (this.state !== 'disconnected') return;
    this.ending = false;
    this.setState('connecting');
    try {
      const selectedPort = await this.serial.requestPort();
      if (this.ending) return;
      this.port = selectedPort;
      await selectedPort.open({ baudRate });
      // A USB disconnect can arrive while open() is still pending.
      if (this.ending) {
        try { await selectedPort.close(); } catch { /* Already unplugged. */ }
        return;
      }
      this.opened = true;
      this.writer = this.port.writable.getWriter();
      this.setState('connected');
      this.readTask = this.readLoop();
    } catch (error) {
      if (error.name !== 'NotFoundError') this.onError(error);
      await this.disconnect();
    }
  }

  async readLoop() {
    const decoder = new TextDecoder();
    try {
      // Framing/parity errors can replace the readable stream without closing
      // the port. Keep reading the replacement while the connection is live.
      while (!this.ending && this.port?.readable) {
        const readable = this.port.readable;
        this.reader = readable.getReader();
        try {
          while (!this.ending) {
            const { value, done } = await this.reader.read();
            if (done) return;
            if (value) this.onData(decoder.decode(value, { stream: true }));
          }
        } catch (error) {
          if (!this.ending) this.onError(error);
          if (!this.port?.readable || this.port.readable === readable) break;
        } finally {
          this.reader.releaseLock();
          this.reader = null;
        }
      }
    } finally {
      const tail = decoder.decode();
      if (tail) this.onData(tail);
      // Do not await disconnect here: it awaits this read task.
      if (!this.ending) void this.disconnect();
    }
  }

  async send(text) {
    if (this.state !== 'connected' || !this.writer) return false;
    try {
      await this.writer.write(new TextEncoder().encode(text));
      return true;
    } catch (error) {
      this.onError(error);
      await this.disconnect();
      return false;
    }
  }

  disconnect() {
    if (this.closeTask) return this.closeTask;
    if (this.state === 'disconnected') return Promise.resolve();
    this.ending = true;
    this.setState('disconnecting');
    this.closeTask = this.closePort();
    return this.closeTask;
  }

  async closePort() {
    try {
      try { await this.reader?.cancel(); } catch { /* USB may already be gone. */ }
      await this.readTask;
      try { await this.writer?.abort(); } catch { /* A reboot closes USB. */ }
      this.writer?.releaseLock();
      this.writer = null;
      if (this.opened) {
        try { await this.port.close(); } catch { /* Already disconnected. */ }
      }
    } finally {
      this.port = null;
      this.reader = null;
      this.readTask = null;
      this.writer = null;
      this.opened = false;
      this.closeTask = null;
      this.setState('disconnected');
    }
  }
}
