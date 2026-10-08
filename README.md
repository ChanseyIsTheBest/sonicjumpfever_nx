Sonic Jump Fever — Nintendo Switch port (SEGA engine / NativeActivity wrapper)
====

This is a native wrapper / loader that runs the original ARM64 Android build of Sonic Jump Fever v1.6.1 on Switch homebrew. It contains no game code and no game assets.

Install & run
----

You need your own copy of the Sonic Jump Fever v1.6.1 APK (arm64-v8a).

```
sdmc:/switch/sonicjumpfever_nx
├── sonicjumpfever_nx.nro
├── game.apk                <- your APK, unmodified (any name ending in .apk). Required: the engine
│                              reads its assets and music straight out of the zip
├── libsonicjumpfever.so    (extracted from your APK on first launch)
├── config.txt              (written on first launch)
├── save_edit.txt           (written on first launch)
└── save_contents.txt       (rewritten on every launch)
```

The folder can have any name under `/switch`.

Controls
----

Sonic Jump Fever steers by tilting a phone and jumps by tapping. The controller gives you both, and keeps them apart.

| Input | Action |
|---|---|
| Touchscreen | Direct, handheld only |
| A / X / Y | Tap the centre of the screen — this is the jump |
| B | Back — closes popups and menus. Ignored where it would quit the game from the home screen |
| ZL / ZR | Tap at the cursor — menus, either shoulder so it plays one-handed |
| Left stick | Move the cursor, and steer |
| Tilt | Steers. The pose you are holding when a level starts becomes neutral |
| L3 | Turn the controller's motion off / back on — stick-only play (saved) |
| L + R + ZL + ZR | Flip the tilt direction, in-game, and save it |
| + | Toggle the cursor on/off |
| L / R | Recenter the cursor |
| D-pad up / down | Adjust cursor sensitivity |
| D-pad left / right | Adjust how far the left stick leans Sonic (saved) |

Settings
----

`config.txt` is written next to the `.nro` on first launch, documented inline:

```
language=auto     # auto, or en fr de it es pt ru
back_button=1     # B is the Android back key; 0 makes it a jump button
tilt_invert=0     # mirror the steering left/right
gyro=1            # motion steers as well as the stick; 0 leaves the sensors off
stick_sens=100    # left-stick steering strength, 25-300 %
rotation=0        # 0 none, 1 = 90 clockwise, 2 = 90 counter-clockwise
debug_log=0       # write sonicjumpfever_nx.log and run the heartbeat
watchdog=1        # turn a freeze into an Atmosphère crash report
```

Language codes are the ones the game itself checks, so Brazilian Portuguese is `pt`. The game has no Japanese, Korean or Chinese text.

Save editing
----

`save_edit.txt` is written on first launch with every line commented out. Uncomment a line and it's applied to `profile0.dat` at every launch. The save is re-signed exactly as the game signs it, and the untouched original is kept once as `profile0.dat.orig`.

```
rings = 50000
red_star_rings = 500
rank = 10                    # the rank the game shows, 1-52
gold_totem = 10              # and every other booster
double_rings = true          # the two permanent upgrades
energy_refill_reducer = true
char.Knuckles.owned = true   # Sonic, Tails, Amy, Knuckles, Blaze, Shadow, Silver
char.all.upgrades = 6
prop.<path> = value          # anything else in the save
```

`save_contents.txt` shows what was applied (or why not), then everything the save holds. Don't edit `profile0.dat` directly: the game throws away a save whose signature doesn't match.

Building
----

Requires devkitPro with the switch-dev group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-sdl2 switch-mesa switch-libdrm_nouveau switch-zlib \
          switch-ffmpeg switch-libpng switch-pkg-config

export DEVKITPRO=/opt/devkitpro
make                        # -> sonicjumpfever_nx.nro
```

[PORTING.md](PORTING.md) has the technical notes: what changed from Sonic Jump, how the loader works, and how to debug a freeze or crash.

Credits
----

The loader and shim infrastructure derives from the open-source Switch .so-loader lineage: so_util, libc_shim, jni_fake, opensles, fakefd, the pointer module and the diagnostics. That lineage is Andy Nguyen, fgsfds and ChanseyIsTheBest, building on TheOfficialFloW's Vita/Switch loader tradition. It reaches this project through the sonicjump_nx port of the original Sonic Jump, which runs on the same SEGA/Hardlight engine. The save editor follows the sonicdash_nx design. Music is decoded with FFmpeg, which keeps its own licence; everything else is MIT-licensed. Thanks to everyone in that lineage for making this approach possible.

Sonic Jump Fever is © SEGA. This project isn't affiliated with or endorsed by SEGA.
