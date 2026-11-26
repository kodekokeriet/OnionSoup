MiiPC - test build for Linux PC (x86_64)
========================================

Start:     ./start.sh   (or double-click start.sh and choose "Run")
Requires:  nothing extra. Works on Linux Mint 19 and newer (also LMDE),
           Ubuntu 18.04+, Debian 10+, Fedora, Arch - any 64-bit distro with
           X11 and PulseAudio, PipeWire or ALSA. SDL2 is built in.
Window:    opens at 2x size and can be resized; the picture scales with it.
Mini v4 screen layout:  MIIPC_RES=752x560 ./start.sh

Press F1 in the app for help. The help screen also opens on first start,
and the bottom line always shows the keys for the view you are in.

Keyboard = Miyoo buttons
  Arrow keys ....... D-pad
  A / B / X / Y .... A / B / X / Y
  Q / W ............ L1 / R1
  Space (or 1) ..... L2
  Shift (or 2) ..... R2 = SHIFT (hold it)
  Tab .............. SELECT
  Enter / Esc ...... START / MENU
  F1 ............... help

Everywhere
  L1 / R1 .......... previous / next view: PLAY SEQ MIX SAMPLE SONG
  L2 ............... play / stop
  SHIFT + L2 ....... record (one bar count-in, can be turned off)
  SHIFT + L1 / R1 .. undo / redo (12 steps)
  SELECT ........... settings: tempo, tap tempo, swing, sequence, bars,
                     quantize, click, count-in, snap chops to zero crossings
  START or MENU .... project menu: save, open, new, copy/clear sequence,
                     resample sequence to a pad, export sequence as WAV, quit

Views
  PLAY    Four pads on A/B/X/Y, Left/Right switches bank (A-D = 16 pads).
          Hold Down + pad = note repeat at the quantize grid.
          Up = KEYS mode: the current pad played chromatically over
          16 semitones (Left/Right picks the range).
          SHIFT + pad erases that pad from the sequence.
  SEQ     Grid editor for the current sequence. Events sit at their exact
          tick, so unquantized playing shows up as played.
          A add/delete, X velocity, B hear, Y grid size.
          SHIFT + Left/Right jumps a bar, SHIFT + Up/Down zooms,
          SHIFT + X mutes the pad row.
  MIX     16 channel strips + FX/master. SHIFT + Left/Right picks a channel.
          Channel: level, pan, filter (LP/HP/BP), cutoff, resonance, drive,
          bitcrush, delay send, reverb send. X mute, Y solo, A hear,
          SHIFT + B resets the channel effects.
          FX: tempo-synced ping-pong delay, reverb, compressor, master level.
  SAMPLE  Waveform for the selected pad. Y loads a WAV, A plays the region,
          B plays the whole file. Start/end (hold X for fine steps), pitch,
          attack, decay, choke group.
          CHOP: "tap" (A plays, X drops a slice mark while it plays, B removes
          the last mark), equal parts or transients. SHIFT + A lays the
          slices out on the following pads. SHIFT + Left/Right picks the pad,
          SHIFT + Y / B copies / pastes pad settings.
  SONG    A list of sequences, each repeated 1-16 times. Y adds a step,
          B deletes it, Left/Right picks the sequence, SHIFT + Up/Down the
          repeats, A turns song mode on/off.

Resampled pads are saved in samples/resampled/, exports in exports/.
The header shows CPU load and battery. The project is also autosaved
every 2 minutes when something changed.

Your own sounds: put .wav files (PCM 8/16/24-bit) anywhere and load them
with Y in the SAMPLE view. Projects are saved in the projects/ folder and
the last session is restored automatically on start.
