# Eglantine on the Steam Deck: setup guide

This is the guide for the builds published at
[MFS-code/Eglantine-steamdeck/releases](https://github.com/MFS-code/Eglantine-steamdeck/releases).
They are `eden-cli`, the window-only frontend of Eglantine, built for the Steam Deck: no settings
menu, no game list, you point it at a game and it runs. Everything below is what the fork's own
measurements were made with, so if you follow it you should see the same numbers as in
[docs/SteamDeck.md](../SteamDeck.md). If you do not, the last two sections are for you.

The AppImage also runs on any x86_64 Linux with glibc 2.39 or newer (Ubuntu 24.04, Fedora 40,
Arch) and a Vulkan driver. The steps assume a Deck on SteamOS 3.8.

## What to expect

Nothing here is console-smooth. A Deck plays these games at half to a third of their target
frame rate, with bugs. Measured on the opening sections, with the settings in this guide:

| Game | Where | Frame rate |
| --- | --- | --- |
| Donkey Kong Bananza | title, menus | 60 fps |
| Donkey Kong Bananza | opening cave, before the first big terrain break | 12-15 fps |
| Donkey Kong Bananza | later open areas | 15-30 fps depending on the area |
| Mario Kart World | menus, opening movie | 60 fps |
| Mario Kart World | Grand Prix race, 24 karts | 18 fps; 25 fps with `--frames-ahead` (see below) |

The first time a game starts it sits on a black screen for a while and then stutters for a few
minutes while shaders are built; the first race or the first minutes of play are not
representative. The second start from the same save is.

Other titles have barely been tried (Welcome Tour and Splatoon Raiders launch; nothing beyond
that has been worked on).

## What you need

- A Steam Deck in **Desktop Mode** (Steam button > Power > Switch to Desktop) for the setup.
  Playing works in Desktop Mode and from a Steam shortcut in Game Mode.
- A **loose dump** of a game from your own Switch 2: a folder containing `exefs/` (with a file
  called `main` inside) and `romfs/`. Dumping is not covered here. NSP/XCI files are not what
  this build is tested with; use the folder layout. Dumps are big (Donkey Kong Bananza 9 GB,
  Mario Kart World 23 GB); the internal SSD or a fast SD card both work.
- No `prod.keys` and no firmware: a loose dump is already decrypted.

## 1. Check the Deck itself

These three things decide more than any emulator setting. Open **Konsole** (the terminal, in the
application menu under System) and paste each line.

**SMT must be on.** A Switch 2 game uses four emulated CPU cores plus the emulator's own threads;
the Deck has four cores and eight threads, and some tweak tools (PowerTools, Decky plugins)
switch the second thread of each core off. This must print `on`:

```sh
cat /sys/devices/system/cpu/smt/control
```

If it prints `off`, turn SMT back on in whatever tool turned it off. Every fps number in this
fork was measured with it on; without it, expect roughly half.

**Enough swap.** A Switch 2 game alone is 7-8 GB of RAM on the host, and the GPU takes its share
of the same 16 GB. SteamOS ships a 1 GB swap file, which is not enough to absorb the peaks. This
prints the swap you have:

```sh
swapon --show
```

The Deck this fork is developed on has a 16 GB swap file. [CryoUtilities](https://github.com/CryoByte33/steam-deck-utilities)
sets one up with two clicks (its "Recommended settings"); that is the easy way. The swap is a
safety margin, not speed: when it is missing the game or the whole Deck dies instead of slowing
down.

**Nothing else running.** Close Discover, the browser, Steam's own download queue and other
emulators before launching. The memory they hold is memory the game does not get, and the Deck
freezing hard when GPU memory runs out is the known failure mode here.

Not needed: a GPU clock lock, a TDP change, or a different Mesa. The development Deck runs with
the GPU forced to its top clock and the stock 15 W limit, and the measured difference from that
was a few percent.

## 2. Install

Download two files from the newest release on the
[releases page](https://github.com/MFS-code/Eglantine-steamdeck/releases): the one ending in
`.AppImage` (about 65 MB) and `eglantine-steamdeck-kit.tar.gz` (a few KB: the launcher script
and the settings file). Then, in Konsole:

```sh
mkdir -p ~/Applications/eglantine
mv ~/Downloads/eglantine-steamdeck-*.AppImage ~/Applications/eglantine/
tar -xzf ~/Downloads/eglantine-steamdeck-kit.tar.gz -C ~/Applications/eglantine/
chmod +x ~/Applications/eglantine/*.AppImage ~/Applications/eglantine/eglantine-deck.sh
```

`~/Applications/eglantine` now holds three files: the AppImage, `eglantine-deck.sh` and
`eglantine-deck.ini`. Keep them together.

If you would rather not use the launcher, the AppImage runs on its own; section 4 has the exact
command line it uses.

## 3. Put the games in place

Make a folder for Switch 2 games and copy each dump into it, one folder per game. Dolphin (the
file manager) is fine for this. The layout the launcher expects:

```
/home/deck/Emulation/roms/switch2/
    dk-bananza/
        exefs/
            main
            main.npdm
            ...
        romfs/
            ...
    mkw/
        exefs/
        romfs/
```

Folder names are yours to pick; keep them to letters, digits and `-`. The launcher recognizes
Donkey Kong Bananza by a name containing `dk` or `bananza` (to switch terrain batching on, and
to refuse the one option that freezes it); any other name is treated as a generic title. Games
somewhere else: set `EGLANTINE_ROMS=/path/to/folder` in the environment, or pass the full path to
the game folder instead of its name.

## 4. Run

From Konsole:

```sh
~/Applications/eglantine/eglantine-deck.sh dk-bananza
~/Applications/eglantine/eglantine-deck.sh mkw --frames-ahead
```

The launcher starts the AppImage full screen with the settings file, the environment the games
need, and the per-game switches from the next section. The Deck's own controls work as player 1
without setup. To stop, go back to the Konsole window (Alt+Tab, or the taskbar) and press
**Ctrl+C**, or in Game Mode use the Steam button > Exit Game.

What the launcher runs, for people who want to do it by hand or from another frontend:

```sh
ulimit -n "$(ulimit -Hn)"
EDEN_NX2_AUTO_STUB=1 ~/Applications/eglantine/eglantine-steamdeck-*.AppImage -f \
    -c ~/Applications/eglantine/eglantine-deck.ini \
    -g ~/Emulation/roms/switch2/dk-bananza/exefs/main
```

- `ulimit -n` raises the open-files limit: a loose dump opens thousands of files at once and the
  default limit of 1024 ends the game with "Too many open files".
- `EDEN_NX2_AUTO_STUB=1` stubs the Switch 2 system calls the emulator does not implement yet
  instead of stopping.
- `-c` loads the settings file; `-f` is fullscreen; `-g` is the game.

Saves, the shader cache and the log live in `~/.local/share/eden/` whichever way you launch.

## 5. The settings, and why they matter

`eglantine-deck.ini` pins six settings, five of them different from `eden-cli`'s defaults. The
defaults are tuned for a desktop PC running Switch 1 games; on a Deck running Switch 2 games
they cost most of the frame rate, and this is the usual reason someone "gets nothing like the
numbers in the docs".

| Setting | Default | This guide | Why |
| --- | --- | --- | --- |
| Console mode (`use_docked_mode`) | Docked | **Handheld** | Docked makes the game render at its TV resolution, roughly twice the pixels of handheld. |
| Resolution (`resolution_setup`) | 1x | **1x** | Already below the Deck's 1280x800 in handheld; anything above costs GPU time the Deck does not have. |
| GPU accuracy (`gpu_accuracy`) | High | **Normal** | High re-checks memory the GPU wrote on every access. The fork's own fences keep both titles correct at Normal. |
| ASTC recompression (`astc_recompression`) | Uncompressed | **BC3** | Mario Kart World keeps about 1 GB of GPU-decoded ASTC resident; uncompressed, that is four times the memory, on a machine where GPU memory running out is a freeze. |
| Asynchronous shaders (`use_asynchronous_shaders`) | off | **on** | Builds new shaders on a second thread instead of stalling the frame on each one. Objects can appear a few frames late the first time a shader is met; the stutter is much smaller. |
| CPU accuracy (`cpu_accuracy`) | Auto | **Unsafe** | The fast-math JIT options; both titles play with them, and the Deck has no CPU to spare for Accurate. |

Everything else is left at its default. The emulator writes the complete settings file back on
exit, so after the first run `eglantine-deck.ini` lists every setting; the six above are the ones
that differ.

To change a setting, edit the file: each one is two lines, `name\default=false` and
`name=value` (the first line tells the emulator to read the second at all). Enum values are the
position in the list, counted from 0: `resolution_setup` goes 1/4x, 1/2x, 3/4x, 1x, 5/4x, 3/2x,
2x, ...; `gpu_accuracy` is 0 Normal, 1 High; `cpu_accuracy` is 0 Auto, 1 Accurate, 2 Unsafe;
`astc_recompression` is 0 Uncompressed, 1 BC1, 2 BC3; `use_docked_mode` is 0 Handheld, 1 Docked.

## 6. Per-game switches

Some of the fork's changes are not safe or not useful for every game, so they are switches rather
than defaults. The launcher takes them as options; by hand they are environment variables.

| Launcher option | Environment | Effect | Use it for |
| --- | --- | --- | --- |
| `--frames-ahead` | `EDEN_NX2_FRAMES_AHEAD=a` | Lets the game run one frame ahead of the GPU thread once every GPU channel has finished the previous frame. | **Mario Kart World**: 18 to 25 fps in the race. **Never Donkey Kong Bananza**: it freezes at the first big terrain break (the game reuses command memory the GPU has not read yet). The launcher refuses the combination. |
| `--chunk-batch` | `EDEN_NX2_CHUNK_BATCH=1` | Draws Donkey Kong Bananza's diggable terrain in batches of 100+ chunks per pipeline setup. | **Donkey Kong Bananza**: +1-2 fps in the opening cave, same picture. The launcher sets it for any folder named `dk*`/`*bananza*`. Needs release `deck-2026.11.03` or newer; older builds ignore it. |
| `--no-mesh` | `EDEN_NX2_NO_MESH=1` | Skips task/mesh shader draws instead of running them through `VK_EXT_mesh_shader`. | **Mario Kart World**: about +2 fps in the race, but trail ribbons, culling quads and some splats disappear. Mesh on is the default because the picture is right; this is the trade if you want the frames. |
| `--windowed` | | No fullscreen. | Reading error output in Konsole. |

Example: `eglantine-deck.sh mkw --frames-ahead --no-mesh` is the fastest Mario Kart World this
fork does; `eglantine-deck.sh dk-bananza` is the right Donkey Kong Bananza.

Two things that are already on and need no switch: the memory fences that keep the Deck alive
through a Mario Kart World race, and the strict present pacing that keeps Donkey Kong Bananza's
terrain breaks from freezing. Their knobs are documented in [docs/SteamDeck.md](../SteamDeck.md)
for people working on the emulator; leave them alone to play.

## 7. A Steam shortcut for Game Mode

1. In Desktop Mode, open Steam, then **Games > Add a Non-Steam Game to My Library**.
2. **Browse**, set the file type to *All Files*, and pick
   `/home/deck/Applications/eglantine/eglantine-deck.sh`.
3. Right-click the new entry > **Properties**. Rename it to the game, and set **Launch Options**
   to the game folder name and any switches, exactly as you would type them after the script:

   ```
   dk-bananza
   ```

   or

   ```
   mkw --frames-ahead
   ```

4. Switch back to Game Mode and launch it like any other game. The Steam button > **Exit Game**
   stops it. Make one shortcut per game.

If you prefer to point the shortcut at the AppImage itself, the launch options are the manual
command from section 4 with `%command%` in place of the AppImage path:

```
EDEN_NX2_AUTO_STUB=1 %command% -f -c /home/deck/Applications/eglantine/eglantine-deck.ini -g /home/deck/Emulation/roms/switch2/dk-bananza/exefs/main
```

(The open-files limit cannot be raised from launch options; if a game stops with "Too many open
files" that way, use the script.)

## 8. Updating

Download the new `.AppImage`, delete the old one from `~/Applications/eglantine`, move the new
one in and `chmod +x` it. The launcher picks the newest `eglantine-steamdeck-*.AppImage` in its
folder. Replace the kit too when the release notes say it changed. Saves and the shader cache are
in `~/.local/share/eden/` and are kept; a new release usually means new shaders to build, so the
first minutes stutter again.

## 9. "I am slower than the numbers above"

In the order they usually turn out to be the reason:

1. **The settings file is not being loaded.** With the launcher, the line it prints at start
   says `settings eglantine-deck.ini`. By hand, make sure `-c` points at the file. Open the file
   after a run: it should be long (the emulator writes every setting back) and contain
   `use_docked_mode=0`.
2. **SMT is off** (section 1). This alone halves the frame rate.
3. **First run.** Shaders are built as they are met; the first race and the first minutes in a
   new area are slow for everyone. Play the same spot twice before comparing.
4. **Mario Kart World without `--frames-ahead`.** That is the 18 vs 25 fps difference in the
   race.
5. **Something else is using memory**: a browser, Discover updating in the background, another
   emulator. 1 GB less for the game shows up as stutter and then as a freeze.
6. **Docked mode or a resolution above 1x** set by hand, or a setting file from another Eden/yuzu
   fork. Start again from the kit's `eglantine-deck.ini`.
7. **An old release.** The numbers track `docs/SteamDeck.md` at the commit the release was built
   from; the release notes link it.

What does not help: GPU clock locks, TDP changes, a different Mesa, or `cpu_accuracy`
Accurate/Paranoid (slower, no fix for anything here).

## 10. When something goes wrong

- **"Permission denied"**: the `chmod +x` line was skipped, or the file was renamed after it.
- **Nothing happens, no window**: run the launcher with `--windowed` from Konsole and read what
  it prints. Usually the path after `-g` does not point at a file called `main` inside `exefs/`.
- **"Too many open files"** in the log: launch through the script (it raises the limit), not
  from a bare Steam launch option.
- **The Deck freezes completely**: the game ran out of GPU memory. Hold the power button. Check
  section 1 (swap, nothing else running) and report the game and where it froze.
- **Donkey Kong Bananza freezes at a terrain break**, picture stuck, music playing: you set
  `EDEN_NX2_FRAMES_AHEAD` somewhere. Remove it; the launcher refuses it for DK for this reason.
- **Objects pop in late** for a second or two in a new area: that is asynchronous shaders doing
  their job; the alternative is a stall of the same length.
- **It is slow**: section 9, then [docs/SteamDeck.md](../SteamDeck.md) for what is and is not
  done.

Problems go to the [issue tracker](https://github.com/MFS-code/Eglantine-steamdeck/issues) with
the game, the release name, the launcher line you used, and `~/.local/share/eden/log/eden_log.txt`
attached.
