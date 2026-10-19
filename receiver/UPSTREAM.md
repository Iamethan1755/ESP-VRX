# About this folder

This is the receiver firmware of **[C5VRX](https://github.com/Twotoz/C5VRX)**
by Twotoz and the C5VRX contributors, release `c5vrx5-v5.0.109`
(`GPL-3.0-only`, see [LICENSE](LICENSE)). All credit for it goes to them.
The official source, releases and web flasher are at
<https://github.com/Twotoz/C5VRX> and <https://twotoz.github.io/C5VRX/>.

VRX Bridge keeps it here as released (the first commit touching this
folder), except:

- Three small commits for the bridge:
  1. `main/console.c`, `main/CMakeLists.txt`: a UART key reader for `<k>`
     frames (menu, channel, band, scan keys) from the ESP32-S3 bridge.
  2. `main/console.c`, `main/control.c`, `main/receiver.h`,
     `main/transport.c`: `<#NN>` frames that tune straight to a channel, and
     the key line on D4 (GPIO23), so the video goes out with 5 bits (bit 0
     off).
  3. `main/console.c`, `main/control.c`: a channel picked during a
     calibration is applied when it ends; a pause drops a half-received
     frame.
- The C5VRX logo and favicon images are left out: the C5VRX name and logos
  are not covered by the GPL ([assets/BRANDING.md](assets/BRANDING.md)). The
  READMEs that show them link to images that are not here.

This copy is not an official C5VRX release and is not affiliated with or
endorsed by C5VRX.
