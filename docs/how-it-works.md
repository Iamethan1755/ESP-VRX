# How the bridge works: design notes and board findings

The reasons behind the design, with what the boards showed along the way.
For the structure (stages, state machines, what guards what) see the
[logic map](logic-map.md); for colour, the [colour study](colour-study.md).
File names are in `bridge/main/`.

- **Capture**: the S3 LCD_CAM camera receiver samples D0..D5 at 20 MHz
  (menuconfig: 10 MHz) with its own clock routed out and back through
  GPIO42; DE/HSYNC are tied high in the GPIO matrix, so every clock is a
  sample. GDMA writes a 64 KiB ring in internal SRAM (3.3 ms at 20 MHz); each
  2 KiB node ends with an EOF whose interrupt advances the write position and
  wakes the decoder. No CPU per sample. The node count also uses the time
  between interrupts, so an interrupt held off for a ring or more (a flash
  write) does not lose rings from the write position.
  Board findings: recording starts on a rising VSYNC edge (VSYNC is tied
  low during setup, then high), and that edge must come a few microseconds
  after `cam_start`, or a restart records nothing; every start therefore
  resets and reconfigures LCD_CAM and waits 20 us before the edge.
  The C5 holds each of its 20 MS/s samples for 50 ns (two 40 MHz clocks);
  the S3 samples asynchronously, so an occasional sample taken during a
  transition is wrong, and the despeckle filter removes it with the FM clicks.
- **Decoder** (core 1, `video.c`): slicer between sync tip and blanking,
  sync edges interpolated to 1/256 sample, a line PLL with flywheel
  (±2 us search when locked), broad-pulse vertical sync, NTSC/PAL from lines
  per field, levels from the sync tip and the back porch after the colour
  burst, and 320 pixels per line through either a median of three samples 2
  apart (*despeckle*, default) or a `[1 2 1]` low-pass with taps 0.1 us apart
  (−15 dB at the NTSC colour subcarrier at 10 and 20 MS/s).
  Sync detection works on the signal averaged over 0.4 us: on the C5 the
  sync tip is only ~6 codes below blanking with ~3 codes of noise per
  sample, so single samples chopped every sync and the line lock dropped
  ~50 times a second (board recording, replayed with `bridge/test/replay.py`).
  Vertical sync needs at least 5 of 7 probes (6..24 us into the line) below
  the slicer, and once locked a new field is only accepted within 8 lines of
  the expected 262/312. A missed vertical sync (a fade, a channel change) is
  replaced after the expected length without losing the line count
  (interlaced 262/263 kept), and the next real one is taken (lock dropped
  after 4 fields without one). If the decoder falls a ring behind while
  locked, it skips whole lines and keeps both locks (`overruns`); while
  searching it just searches further on (`catchups`). With the C5's small
  sync amplitude, two probes took dark picture for vertical sync and split
  frames; a measured low-run length was cut short by single FM clicks. One field
  becomes one frame: 320×240 NTSC, 320×288 PAL. Without sync it free-runs,
  like an analog monitor. Only fields that will be encoded are drawn (every
  other field at 30 fps), pixels go through a per-field level LUT (in the
  pixel loop, except on the histogram lines), and the
  project builds with `-O2`: at ESP-IDF's default `-Og` one drawn line took
  longer than a line of video lasts.
  **Colour** (NTSC, on by default, `m` on the console or the page's Colour
  button; [docs/colour-study.md](colour-study.md)): the colour burst of
  every other line is measured against the subcarrier at the absolute sample
  index and averaged into a phase and amplitude reference; a burst more than
  35° off is used for its own line only, and a second one off to the same
  side restarts the average (a clock slip: the S3 samples the C5's 20 MS/s
  with its own clock and takes a sample twice or drops one every few tens of
  ms, a 64° step). On the even rows the chroma is summed over 64 blocks of
  16 samples with the S3's SIMD instructions (checked against C at start,
  `colour=on(no-simd)` in `s` if not) and rotated by the reference into a
  Cb/Cr plane next to the unchanged luma rows: the decoder writes no more to
  PSRAM than for gray. Board, live: 62 µs per drawn line (gray 50), 61 % of
  core 1, no overruns. PAL and pictures without a burst stay gray, and so do
  noisy ones: the chroma shares the burst's band, so the bursts' phase noise
  around the reference (`bnoise` in `s`, ~9° on a good signal) fades the
  colour from 15° (`sat`) and turns it gray at 30° (`colour=noisy`, colour
  again below 25°) instead of showing colour confetti; a field without
  usable bursts (no line lock, such as the C5's snow without a transmitter)
  is gray too.
- **Stream** (core 0): the decoder hands frames over through three buffers
  (four with the screen, `mbox.c`), newest wins, so the encoder always
  starts on the latest picture and the decoder never waits; each frame
  carries the time it was finished, which gives `pipe_ms` (last decoded line
  to JPEG ready). Optional temporal denoise (a pixel within ±28 of the
  running picture is averaged into it, a bigger change is taken as motion),
  then grayscale JPEG (`esp_new_jpeg`, quality 60) at up to 30 fps, always the
  same field parity; colour frames are encoded 4:2:0 from YCbYCr built 16
  rows at a time in internal RAM (block encoder, no colour frame in PSRAM),
  denoised on the way, ~21 fps with denoise (the encoder's ms per frame:
  `enc_ms=` denoise + colour bands + JPEG in `s`; a noisy picture makes the
  JPEG itself slower); MJPEG on port 81 (up to 3 viewers: when all are taken
  the oldest makes room, and a viewer that reads nothing for 5 s is closed),
  page and status on port 80. The page closes its stream while it is in the
  background and reconnects after 5 s without data. 2.4 GHz only: 5 GHz Wi-Fi would land on the FPV band. The denoise
  runs here and not in the decoder: at 20 MHz the decoder's extra PSRAM
  traffic caused 47 overruns/s.
- **Screen** (optional, core 0, `display.c`): its own task takes the newest
  finished frame from the same mailbox as the encoder, so neither waits for
  the other, and the decoder never renders into a frame being read. Gray goes
  to RGB565 through a table (colour through YCbCr tables), 12 lines at a time into two 7.5 KB internal
  DMA buffers: one is converted while the other goes out over SPI (esp_lcd,
  DMA). The whole screen is one window per frame (the first strip starts
  it, the others continue it), and the frame goes back to the mailbox as
  soon as its last strip is converted. Converting should cost core 0 about
  2 ms a frame (estimated); the rest is the SPI DMA.
- **Host test** (`run_host_tests.py`): NTSC/PAL test patterns at 10 and 20 MHz
  with noise, FM clicks and C5-like weak, noisy levels, also with 5 bits
  (keys on the bit 0 wire), through the real decoder (lock, field length and
  count, no lock losses, no rolled field after 1-3 missed vertical syncs, the
  lock kept through a short stall), and the frame mailbox in random
  orders of hand-offs (the decoder never gets a frame a reader holds; with
  one reader exactly the frames of the original three-buffer mailbox).
  `bridge_tool.py rawlong` records ~6 real fields from the board and
  `replay.py` runs them through the decoder with lock/field events.
  With clicks: speckles smooth 64 / despeckle 0; noise (rms) smooth 11.5,
  despeckle 7.4, despeckle + denoise 4.3.

Expect a delay on the order of 100 ms (one field, JPEG, Wi-Fi, browser):
fine for watching and recording, use the analog output for flying. The
bridge measures its own share (`pipe_ms` in `s`, `send_ms` in `w`); the whole
chain from camera to eye has not been measured yet
([logic map, section 6](logic-map.md#6-time-budget-and-delay)).
