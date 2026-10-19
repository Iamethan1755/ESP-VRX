# PlatformIO extra_script (pre): before every upload, ask the running
# C5VRX-4 image to reboot into USB download mode (tools/enter_download.py),
# so esptool can flash with --before no-reset and no BOOT button.
#
#   extra_scripts = pre:tools/pio_enter_download.py
#   upload_flags = --before=no-reset
Import("env")  # noqa: F821  (SCons)

import os
import sys

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))  # noqa: F821
from enter_download import enter_download  # noqa: E402


def _before_upload(source, target, env):
    # This pre-action runs before PlatformIO's own port detection.
    env.AutodetectUploadPort()
    port = env.subst("$UPLOAD_PORT")
    if not port:
        raise RuntimeError("enter_download: no upload port detected")
    enter_download(port)


def _after_upload(source, target, env):
    # esptool's RTS hard reset returns the C5 to the ROM loader after a
    # download boot; a watchdog reset starts the new image. The port vanishes
    # during that reset, which esptool reports as an error: expected here.
    port = env.subst("$UPLOAD_PORT")
    if not port:
        return
    import subprocess
    cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--port", port,
           "--before", "no-reset", "--after", "watchdog-reset", "chip-id"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    out = result.stdout + result.stderr
    print("enter_download: watchdog reset sent, new image starting"
          if "Hard resetting with a watchdog" in out else
          "enter_download: watchdog reset not confirmed; press RESET if the board stays silent")


env.AddPreAction("upload", _before_upload)  # noqa: F821
env.AddPostAction("upload", _after_upload)  # noqa: F821
