# Changelog

What changed for someone using or building ESP-VRX. The version is in
`bridge/version.txt` and shows on the page (Settings > Details), in
`/status` and in the console's `s` line (`fw=`).

## 1.0.0 (2026-11-12)

The first numbered version: everything built so far, after a review of the
whole code base along [docs/logic-map.md](docs/logic-map.md).

What ESP-VRX does at this version:

- Streams a C5VRX receiver's picture over the S3's own Wi-Fi to a browser:
  NTSC in colour or gray, PAL in gray, 320×240 / 320×288, up to 30 fps.
- Band and Channel pickers, Auto (scan) and the receiver's menu from the
  phone, over the video bit 0 wire.
- Native full screen on iPhone, the browser's elsewhere.
- Live view over USB (`bridge_tool.py live`), optional SPI screen.

New in this version:

- **Picture settings are remembered**: position, levels, despeckle, denoise
  and colour come back after a power cycle. **Defaults** on the page (Settings >
  Tools) or `z` on the console resets them.
- **Firmware version** on the page, in `/status` and in `s`.
- **The bridge measures its own delay**: `pipe_ms` in `s` and under Settings
  > Details is the time from a frame's last decoded line to its JPEG being
  ready (on the board with the test pattern: 39 ms in colour, 29 ms in gray).
- **"No video arriving"**: a picture that stopped is dimmed under a note
  instead of looking live.
- **A page icon** for the browser tab and the home screen.
- `w` on the console also shows what the page server is doing (requests,
  the slowest one, and `BUSY` if one hangs).
- `tools/check_consistency.py` (run with the host tests) checks that the
  firmware, the page and the mock agree on status fields, commands and keys.
- `tools/mock_bridge.py` answers the page's buttons like the bridge and can
  simulate a frozen stream, a hung status request and a missing receiver.
- `tools/requirements.txt`, a GitHub Actions build, this changelog and the
  logic map.

Fixed:

- The page's badge could stay on `connecting` forever while the picture
  played: status requests had no timeout and piled up behind one that never
  answered. Now one request at a time, given up after 3 s (`offline`).
- A capture interrupt held off for a whole ring (3.3 ms, as during a flash
  write) lost that ring from the sample count, and the decoder took
  overwritten samples for fresh ones. The count now also uses the time.
- `overruns` rose during a receiver scan although no picture was lost.
  Falling behind while searching for sync is now counted as `catchups`;
  `overruns` only counts lost picture lines.
- The denoise switch could blend a stale picture into one frame.
- A frame that failed to encode was dropped without a word.
- The console's `C<k>` called a busy key line "key not accepted".
- `bridge_tool.py keys` hid the answers to `u`, `#NN` and `w`.
- The sample-rate reading jumped after a command in test-pattern mode.
- Browsers' icon requests logged a 404 each.

Changed:

- Settings on the page are grouped as Picture, Picture position, Receiver
  menu and Tools (Snapshot, Test pattern, Defaults); the Levels button shows
  its state.
- `BRIDGE_COLOUR` in menuconfig is now the default of a fresh board; the
  stored choice wins.
- The USB live page has a Colour button.
- The README is shorter: the internals moved to
  [docs/how-it-works.md](docs/how-it-works.md).

Removed:

- The code that erased a home network stored by an early version.

Before 1.0.0 there were no version numbers; see the git history.
