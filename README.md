# Feeding Frenzy 2 - Unofficial Remaster

A fan-made mod for **Feeding Frenzy 2: Shipwreck Showdown Deluxe** (the Steam version) that
brings the 2006 game up to date: widescreen, smooth high frame rates, and co-op for up to 4
players on one PC or 10 online.

**You need your own copy of the game.** This repository contains none of the game's files:
the installer builds the changed game data on your PC, from your own copy.

## Features

- **Widescreen and ultrawide.** The picture fills any screen shape (16:9, 21:9, 32:9) at its
  full height, instead of 4:3 with black bars. The menus, HUD and level backdrops are
  extended to the new width.
- **High frame rates.** The game runs at your monitor's refresh rate (144, 165, 240 Hz...)
  instead of 60, at the original speed.
- **Mouse fixes.** Steering feels the same at any resolution and frame rate.
- **Local co-op, up to 4 players.** Player 1 uses the mouse. Everyone else joins during a
  level by pressing a button on a device of their own:
  - a gamepad;
  - a keyboard's WASD keys;
  - a keyboard's arrow keys;
  - another mouse.

  Every player has their own colour, lives and score. The team shares the Frenzy chain, and
  the camera zooms out to keep everyone in view.
- **Online co-op, up to 10 players.** Friends join from their own PCs with `ff2guest.exe`.
  They don't need the game: they see and hear yours, and each steers a fish of their own.
- **Fixes for co-op.** Clams, jellyfish, ink, poison, bonuses, bosses and night levels all
  work with several fish, along with crashes found on the way.
- **Gameplay touches.**
  - A golden minnow comes into every level at least once, swimming in where you can see it.
    Now and then another one shows up in a long level.
  - After a perfect stage, your Frenzy chain holds until the mermaid arrives, instead of
    running out while the fish swim off the wider screen.

## Installing

1. Install Feeding Frenzy 2 Deluxe from Steam.
2. Download **FF2 Remaster Setup.exe** from [Releases](../../releases) and run it.
3. It finds the game by itself (or choose its folder with **Browse**). Click **Install**.
4. Start the game from Steam as usual.

**Uninstall** in the same program puts the original files back. Setup keeps them in the
game folder as `*.original`.

If Steam's *Verify integrity of game files* puts the original `FF2.saf` back, run Setup's
**Install** again.

Setup checks that your copy is the Steam version it was made for. Other versions of the
game may differ, so it asks before installing on them.

## Settings

The settings are in `ff2wide.ini` in the game folder; every setting is explained in the file.
The game's **Options** screen also has:
- **Co-op:** on from the next level; switched off, the other fish leave straight away.
- **Online:** hosting starts or stops straight away, and the screen shows the address
  friends type in.
- **Colour buttons** for players 2 to 4.

| Setting | What it does |
| --- | --- |
| `width` | Picture width at 600 tall; 0 = your screen's shape, 800 = the original 4:3 |
| `fps` | Frame rate cap; 0 = your monitor's refresh rate |
| `mouse_sensitivity` | How far the fish moves per pixel of mouse movement |
| `coop` | 1 = co-op on |
| `coop_p2_color` .. `coop_p10_color` | Each player's colour (red,green,blue) |
| `online` | 1 = host online games for friends |
| `online_port`, `online_upnp` | The port guests connect to, and whether to ask the router to open it |
| `online_kbps`, `online_fps` | The picture sent to each guest |

## Co-op controls

| Device | Move | Dash | Special |
| --- | --- | --- | --- |
| Mouse (player 1, or another mouse) | move the mouse | left button | right button |
| Gamepad | left stick | A | B |
| Keyboard, left side | W A S D | Space | E or Left Shift |
| Keyboard, right side | arrow keys | Right Ctrl | Enter or Right Shift |

Each keyboard has both sides, so one keyboard is two players. In co-op, Space doesn't
pause the game; P and Esc still do.

## Playing online

1. Tick **Online** in the game's Options screen (or set `online=1` in `ff2wide.ini`). The
   Options screen shows the address friends connect to.
2. Send your friends `ff2guest.exe` from the game folder. They run it and type in your
   address.
3. Each friend becomes the next player when they connect.

Every guest gets their own copy of the picture, so your upload needs `online_kbps` for each
one. Four guests at the default 6000 need 24 Mbit/s. Lower it if pictures stutter.

The game asks your router to forward UDP ports 47600 and 47601 (UPnP). If `ff2wide.log` says
that failed, forward them in your router's settings, or use a VPN like Tailscale or ZeroTier.

## Building from source

You need a 32-bit MinGW-w64 toolchain (i686), for example MSYS2's `mingw-w64-i686-gcc`.

```sh
MINGW=/mingw32/bin sh build.sh
```

`out/` then has:
- `version.dll`: the patch the game loads;
- `ff2online.dll` and `ff2guest.exe`: online play;
- `FF2 Remaster Setup.exe`: the installer, with all of the above inside it.

How the repo is laid out:

| Folder | What's in it |
| --- | --- |
| `src/` | The patch. A `version.dll` that the game loads, which changes the game's code in memory. |
| `online/` | The online host, `ff2online.dll`, loaded only with `online=1`. |
| `guest/` | The viewer friends use, `ff2guest.exe`. |
| `shared/` | Code shared by the patch, the host and the viewer. |
| `installer/` | Setup. `mods.c` makes the changed game data from the player's own `FF2.saf`. |
| `config/` | The default `ff2wide.ini` and cnc-ddraw's settings. |
| `third_party/` | cnc-ddraw, which scales the game's DirectDraw picture to your screen. |
| `tools/` | Python scripts to unpack and repack `FF2.saf`, for development. |

## Legal

This is an unofficial fan project. It is not affiliated with, endorsed by or connected to
PopCap Games, Sprout Games or Electronic Arts. *Feeding Frenzy* is their trademark.

No part of the game is included. The mod changes the game's code only in memory while it
runs, and builds its changed data files from the copy you own.

The mod's own code is under the [MIT License](LICENSE). Bundled third-party software is
listed in [THIRD_PARTY.md](THIRD_PARTY.md).
