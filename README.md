# Selotape

Songcraft Audio. Tape saturation + varispeed audio effect (AU / VST3 / Standalone), built with JUCE.

## Build (macOS)

Requires Xcode command line tools and CMake (`brew install cmake`).

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j 8

The AU is copied to `~/Library/Audio/Plug-Ins/Components/` automatically.
Validate it with:

    auval -v aufx Selo Sngc

If your DAW doesn't see it, run `killall -9 AudioComponentRegistrar` and rescan.

The plugin IDs changed from the earlier TapeVari builds, so Selotape shows up as
a new plugin. To remove the old one:

    rm -rf ~/Library/Audio/Plug-Ins/Components/TapeVari.component
    rm -rf ~/Library/Audio/Plug-Ins/VST3/TapeVari.vst3

## Interface

- Large dial: Speed (varispeed, semitones). The small white dot on its ring
  shows the live transport speed, so you can watch it wind up and down with
  Motor Inertia and Tape Stop.
- Tape Stop button under the dial.
- Knobs: Drive, Warmth, Tone, Mix, Output / Wow, Flutter, Hiss, Inertia, Window.
- Top right: Oversampling (Off / 2x / 4x / 8x) and Auto Gain.
- Double-click any knob to reset it to its default.

## Notes

- Reports one fixed latency to the host (60 ms head delay plus the 8x
  oversampler's filter latency). Lower oversampling settings are padded to
  match, so changing the factor never changes reported latency.
- The dry path is delayed by the same amount.
- For varispeed work, use Mix at 100%.
