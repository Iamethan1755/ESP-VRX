#!/usr/bin/env python3
"""Host regressions for upload port detection and concurrent console replies."""
import contextlib
import io
from pathlib import Path
import queue
import runpy
import sys
import threading
import time
import types
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parent


class SerialException(Exception):
    pass


class SerialTimeoutException(SerialException):
    pass


class ReplyDuringWrite:
    def __init__(self):
        self.replies = queue.Queue()
        self.consumed = threading.Event()

    def open(self):
        pass

    def close(self):
        pass

    def read(self, size):
        try:
            reply = self.replies.get(timeout=0.01)
        except queue.Empty:
            return b""
        self.consumed.set()
        return reply

    def write(self, command):
        self.replies.put(b"SNAPSHOT healthy\n")
        if not self.consumed.wait(1):
            raise AssertionError("reader did not receive the immediate reply")
        # Let the reader try to match the reply before write() returns.
        time.sleep(0.03)
        return len(command)


class TimeoutWrite(ReplyDuringWrite):
    def write(self, command):
        raise SerialTimeoutException()


class UploadEnv:
    def __init__(self, port):
        self.port = port
        self.detected = False

    def subst(self, value):
        if value == "$PROJECT_DIR":
            return str(TOOLS.parent)
        if value == "$UPLOAD_PORT":
            return self.port
        raise AssertionError(value)

    def AutodetectUploadPort(self):
        self.detected = True
        if not self.port:
            self.port = "COM42"

    def AddPreAction(self, target, action):
        self.before = action

    def AddPostAction(self, target, action):
        pass


class FlashToolsTests(unittest.TestCase):
    def test_upload_detects_port_before_requesting_download(self):
        for port in ("", "COM7"):
            with self.subTest(port=port):
                env = UploadEnv(port)
                requested = []
                download = types.ModuleType("enter_download")
                download.enter_download = requested.append
                with patch.dict(sys.modules, {"enter_download": download}), patch.object(sys, "path", sys.path.copy()):
                    runpy.run_path(str(TOOLS / "pio_enter_download.py"), init_globals={
                        "Import": lambda name: None, "env": env,
                    })
                    env.before(None, None, env)
                self.assertEqual(requested, [port or "COM42"])

    def run_soak(self, serial_type):
        serial = types.ModuleType("serial")
        serial.Serial = serial_type
        serial.SerialException = SerialException
        serial.SerialTimeoutException = SerialTimeoutException
        output = io.StringIO()
        argv = ["console_soak.py", "MOCK", "--seconds", "0.01", "--reply-timeout", "0.05"]
        with patch.dict(sys.modules, {"serial": serial}), patch.object(sys, "argv", argv), contextlib.redirect_stdout(output):
            code = runpy.run_path(str(TOOLS / "console_soak.py"))["main"]()
        return code, output.getvalue()

    def test_reply_received_during_write_is_counted(self):
        code, output = self.run_soak(ReplyDuringWrite)
        self.assertEqual(code, 0, output)
        self.assertIn("commands=1 write_timeouts=0 replies=1 lost=0", output)

    def test_failed_write_is_not_left_pending(self):
        code, output = self.run_soak(TimeoutWrite)
        self.assertEqual(code, 1, output)
        self.assertIn("commands=1 write_timeouts=1 replies=0 lost=0", output)


if __name__ == "__main__":
    unittest.main()
