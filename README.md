# A hood full of pixels

A Halloween wizard costume with an LED matrix face under the hood and a magic staff that
changes the scene when you stomp it on the floor: face, staff and a sound effect change
together.

Two Raspberry Pi Pico 2 W do the work. The one in the mask plays videos from a microSD card
on two flexible 128x64 HUB75 LED panels. The one in the staff drives 180 addressable LEDs, reads
a rotary encoder, two buttons and an accelerometer, streams sound effects to a Bluetooth
speaker and tells the mask over Bluetooth LE when to change the face.

The build is described at https://there.oughta.be/a/hood-full-of-pixels and an overview is
given in the following video:

[![YouTube video: A hood full of pixels](https://img.youtube.com/vi/yXilvMO-uTY/0.jpg)](https://youtu.be/yXilvMO-uTY)

If you have pull requests or bug reports, please do not hesitate to open an issue.

<a href="https://www.buymeacoffee.com/there.oughta.be" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-blue.png" alt="Buy Me A Coffee" height="47" width="174" ></a>

# Warnings

This is not a step-by-step guide. Please read the blog post first, in particular its
disclaimers: both parts run up to 3 A through self-assembled circuits, so solder properly, use
suitable wires and secure everything against strain. The firmware limits the current to stay
below what the power banks deliver, but its current model is calibrated to my panels and my
LED strip (see [Adapting the firmware](#adapting-the-firmware-to-your-build)). Use a USB-C power
meter.

# Note about AI use

This entire repository was generated using Anthropic's Claude Opus 5.5. The only exceptions are this paragraph and the 3D print models for the costume's staff by me and some of the media files and libraries from external sources. The blog post, video and of course all manual electronics work are still done by me. If you want to learn more about my thoughts on this topic, check out [this section](https://there.oughta.be/a/hood-full-of-pixels#software-and-ai) in the blog post.

# Contents

| Folder | Content |
|---|---|
| `display/` | Firmware of the mask's Pico: HUB75 driver (PIO + DMA), video player, BLE peripheral |
| `controller/` | Firmware of the staff's Pico: LED strip, encoder, buttons, LIS3DH stomp detection, A2DP audio, BLE central |
| `common/` | Code shared by both: link protocol, SD card setup |
| `lib/` | [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) by carlk3 (git submodule) |
| `tools/` | Python tools that generate the scenes, convert videos and sounds and draw the wiring diagrams |
| `media/` | Generated scenes, ready to be copied to the SD cards, plus preview videos |
| `sound original/`, `video original/` | Source material for some of the scenes |
| `3d print/` | Panel mounts for the mask, a case for the mask's electronics and all parts of the staff |
| `docs/` | Design notes, wiring diagrams and the breadboard bring-up procedure |

Documentation:

- [docs/design.md](docs/design.md): pins, behaviour, file formats, power limiting, sync protocol
- [docs/breadboard.md](docs/breadboard.md): wiring and step-by-step tests on the breadboard, serial commands
- [docs/wiring_display.svg](docs/wiring_display.svg), [docs/wiring_controller.svg](docs/wiring_controller.svg): wiring diagrams
- [docs/mask_geometry.md](docs/mask_geometry.md): how the bent and tilted panels map to the face

# Hardware

The shopping list is in the blog post. In short, per part:

- **Mask**: Raspberry Pi Pico 2 W, two 74HCT245 level shifters, two 2x8 box headers for the
  panels' ribbon cables, microSD breakout (3.3 V), 1N5819 diode, capacitors (2200 µF, 10 µF,
  2× 100 nF), 10 kΩ pull-up on OE, USB-C power breakout and a power bank.
- **Staff**: Raspberry Pi Pico 2 W, 74AHCT125 level shifter, WS2812(E) strip with 2 × 90 LEDs,
  Adafruit LIS3DH breakout, EC11 rotary encoder and two push buttons, microSD breakout, 1N5819
  diode, capacitors (1000 µF, 10 µF, 100 nF), 330 Ω and 10 kΩ resistors, USB-C power breakout
  and a power bank.
- A Bluetooth speaker with A2DP.

**Check your LED panels.** Panels that look identical can use different driver chips. The
firmware is written for HUB75E panels with 1/32 scan, FM6124C column drivers and SM5368 row
drivers. The FM6126A init sequence and plain binary row addressing (A to E) can be selected on
the display's serial console, but other chips will need changes to `display/hub75.c`.

Pin assignments are in [docs/design.md](docs/design.md). The PIO programs need groups of
consecutive GPIOs, so please do not reorder the pins to simplify your wiring.

# Building the firmware

You need the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x (I used
2.3.1, with its submodules) and an `arm-none-eabi-gcc` toolchain.

```sh
git clone --recursive https://github.com/Staacks/a-hood-full-of-pixels.git
cd a-hood-full-of-pixels
export PICO_SDK_PATH=~/pico/pico-sdk
cmake -S . -B build && cmake --build build -j8
```

This builds

- `build/display/display.uf2` for the mask,
- `build/controller/controller.uf2` for the staff,
- `build/controller/bt_test.uf2`, a Bluetooth test firmware for the staff's Pico that can be
  used to measure the latency of your speaker (see below).

To flash, hold the BOOTSEL button of the Pico while plugging it into USB and copy the uf2
file to the drive that appears.

Both firmwares have a serial console on USB (for example `picocom /dev/ttyACM0`) that prints
a help text on connect (`h` shows it again). Nothing on the console is needed during normal
use, but it has test patterns, LED tests, status output and settings for the bring-up
described in [docs/breadboard.md](docs/breadboard.md).

# Preparing the SD cards

Format two microSD cards (SDHC, FAT32) and copy

- everything from `media/sd_display/` to the root of the mask's card,
- everything from `media/sd_controller/` to the root of the staff's card.

Copy the files onto freshly formatted cards, so they are not fragmented. The mask reads its
videos at 800 KB/s.

# Usage

Both parts start on their own with the base scene (glowing eyes), and either one works
without the other. The staff connects to the mask and to the speaker in the background.

- **Pairing the speaker**: put the speaker in pairing mode, then long-push the encoder. The
  crystal blinks blue while the staff is searching. The staff remembers the speaker and
  reconnects to it automatically.
- **Turn the encoder** to select a scene. The crystal at the top of the staff breathes in the
  colours of the selected scene.
- **Stomp** the staff on the floor to start it. A stomp without a selected scene only shows a
  short wave on the staff.
- **Encoder push**: drop the selection.
- **Button 1**: start the selected scene without stomping. Long press: start the base scene.
- **Button 2**: back to the base scene, or, if already there, select the first scene of the
  playlist. Long press: switch the playlist (the crystal shows white for "scenes", rainbow for
  "fun" and pink-gold for "family").

# Adapting the firmware to your build

There is no configuration file yet. Many settings can be changed on the serial console to try
them out, but they are not saved, so put the values you want into the source code and rebuild:

| What | Where |
|---|---|
| LEDs per side, crystal LEDs, LEDs hidden by the grip ("dead" LEDs) | `controller/strip.h` (`STRIP_DEFAULT_*`); the console's LED test (`e`) and `L` help to find the numbers |
| Strip current calibration | `controller/strip.h` (`STRIP_DEFAULT_BASE_MA`, `STRIP_DEFAULT_UA_PER_CHANNEL`); measure with `m` on the console, see breadboard.md, controller test 5 |
| Panel current calibration | `display/hub75.h` (`HUB75_IDLE_MA`, `HUB75_FULL_WHITE_MA`) |
| Bluetooth latency of your speaker | `controller/main.c` (`L_BT_US`, 250 ms for a JBL Flip 7); measure it with the metronome of `bt_test` |
| Stomp threshold, staff axis, tilt check | `controller/stomp.c` (`stomp_default_config`); try values with `[`, `]`, `x` and `a`, and record data with `t` and `tools/plot_accel.py` |
| Encoder direction and steps per detent | `controller/encoder.h` |
| Strip colour order (GRB) | `controller/strip.c` (`order`); try others with `o` |
| Panel order and rotation, row addressing, colour order | `display/hub75.c` (`mapping`, `row_mode`) |
| FM6126A init at boot (only for panels with these drivers) | `display/main.c` (`fm6126a`); try it with `f` |
| Scenes, crystal colours and playlists | `controller/main.c` and `display/player.c` (the scene numbers must match) |

# Making your own scenes

The tools need Python 3.10 or newer with numpy, scipy, opencv-python, Pillow and (for
`plot_accel.py`) matplotlib and pyserial, plus `ffmpeg` on the path.

```sh
python3 tools/make_scenes.py                  # all scenes -> media/sd_display, media/sd_controller, media/preview
python3 tools/make_scenes.py eyes --sheet     # one scene, contact sheet instead of a preview video
python3 tools/make_sounds.py                  # the sounds -> media/sd_controller
python3 tools/convert_video.py in.mp4 media/sd_display/myscene.vid --preview myscene.mp4
```

- Each scene is a Python module in `tools/scenes/` that renders the mask and the staff
  procedurally (see the docstring of `tools/make_scenes.py`). Scenes are drawn on a "frontal
  canvas" in millimetres, and `tools/maskvid.py` maps them onto the bent and tilted panels, so
  you do not need to think about the eye slit or the curvature.
- `tools/convert_video.py` converts any video (for example a Blender render) into a mask video.
- `tools/facetrack.py` finds a face in a video and holds it still on the mask with the eyes at
  fixed positions and the background removed. The "me" scene is an example: it is made from
  `video original/me.mp4`.

Not everything from the video is included:

- **Darth Maul**: the face pattern was traced from someone else's drawing, so it is not
  included. Its slot in the scene list stays empty (the mask shows the fallback eyes).
- **Rick Astley**: needs the official music video of "Never Gonna Give You Up", which is
  copyrighted, so neither the video nor the generated files are included. Put your own copy at
  `nonfree original/rick-roll.mp4` (the folder is ignored by git) and run

  ```sh
  python3 tools/make_scenes.py rick
  python3 tools/make_sounds.py
  ```

  `tools/scenes/rick.py` cuts two shots from the video (the timestamps are at the top of the
  file and fit the official 3:32 version, which I had as 1080p at 25 fps), tracks the face and holds it still on the mask.
  The sound is the song from the start of the intro shot, so the lips are in sync during the
  intro. Without the video the scene shows a drawn portrait bobbing to the beat instead, and
  you can provide your own sound as `sound original/rick.wav`.
- **Family**: faces from your own photos, which stay in the `private/` folder (ignored by
  git):

  ```sh
  python3 tools/family.py scan ~/Pictures/family    # find all faces (takes minutes) and group them by person
  python3 tools/family.py pick P03 P07b P01         # choose people from the contact sheets in private/family/
  python3 tools/make_scenes.py family
  ```

  `scan` writes contact sheets (`private/family/people_*.png`) with one row per person and
  their best faces (a, b, c, ...). `pick` stores your choice, `show` and `similar` help to
  find more faces of a person. All options are in the docstring of `tools/family.py`. Without
  a selection `make_scenes.py` skips the scene.

# Licence

The code (firmware and tools) is released under the GNU General Public Licence 3
([LICENSE](LICENSE)). The 3D print files, the documentation in `docs/` and the media files
(`media/` and `sound original/signature.wav`) are released under the Creative Commons licence
CC-BY 4.0 ([LICENSE-CC-BY-4.0.txt](LICENSE-CC-BY-4.0.txt)), **except for the files that show my
face**.

**My face** is in `video original/me.mp4`, `media/sd_display/me.vid`,
`media/sd_display/me_intro.vid`, `media/preview/me.mp4` and `media/preview/me_sound.mp4`. These
files are not under a free licence: all rights reserved. You may use them, and anything you
generate from them, only to run and try out this project or to show or demonstrate this
project, as long as it is clear that this is what the footage is. Any other use, in particular
out of context, is not permitted.

**Additional permission under GNU GPL version 3 section 7**: The firmware uses BTstack
(BlueKitchen GmbH) and the CYW43 driver (George Robotics Pty Ltd) from the Raspberry Pi Pico
SDK. Their licences only allow their use with Raspberry Pi hardware or for non-commercial
purposes, which is not compatible with the GPL on its own. If you modify this program, or any
covered work, by linking or combining it with BTstack or the CYW43 driver (or a modified version
of those libraries), the licensors of this program grant you additional permission to convey
the resulting work. Corresponding Source for a non-source form of such a combination shall
include the source code for the parts of those libraries used as well as that of the covered
work. Note that these libraries keep their own licences.

Third-party parts:

- `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` by carlk3 (Apache 2.0) with FatFs by ChaN.
- `controller/ws2812.pio` is from the Raspberry Pi pico-examples (BSD-3-Clause), and
  `display/hub75.pio` and the `btstack_config.h` files are based on them.
- `controller/speaker.c` is based on BTstack's `example/a2dp_source_demo.c` (Copyright (C) 2016
  BlueKitchen GmbH, BTstack licence).
- `pico_sdk_import.cmake` is from the Pico SDK (BSD-3-Clause).
- The sound clips in `sound original/` are from freesound.org and are in the public domain
  (CC0), see [sound original/CREDITS.md](sound%20original/CREDITS.md).
- `tools/facetrack.py` and `tools/family.py` download OpenCV's YuNet and SFace models (from the
  OpenCV model zoo) on first use; they are not part of this repository.
