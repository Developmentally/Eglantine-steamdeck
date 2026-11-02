# Running the Steam Deck AppImage

This is for the builds published at
[MFS-code/Eglantine-steamdeck/releases](https://github.com/MFS-code/Eglantine-steamdeck/releases).
They are `eden-cli`, the window-only frontend of Eglantine, built for the Steam Deck. There is no
settings menu and no game list: you point it at a game and it runs. What runs and how fast is in
[docs/SteamDeck.md](../SteamDeck.md); expect 15-30 fps in the two titles that have been worked on
(Donkey Kong Bananza, Mario Kart World), and crashes.

The AppImage also runs on any x86_64 Linux with glibc 2.39 or newer (Ubuntu 24.04, Fedora 40,
Arch) and a Vulkan driver. The steps below assume a Deck on SteamOS 3.8.

## What you need

- A Steam Deck in **Desktop Mode** (Steam button > Power > Switch to Desktop).
- A **loose dump** of a game from your own Switch 2: a folder containing `exefs/` (with a file
  called `main` inside) and `romfs/`. Dumping is not covered here. NSP/XCI files are not what
  this build is tested with; use the folder layout.
- About 10 GB of free RAM while playing: Switch 2 games are large. Close Discover, the browser and
  anything else before launching.

No `prod.keys` and no firmware are needed for a loose dump; it is already decrypted.

## 1. Download

On the Deck, open the releases page in a browser, download the file ending in `.AppImage` from
the newest release (about 65 MB), and leave it in `Downloads`. The `.tar.gz` is for people who
want the bare binary; skip it.

## 2. Put the game in place

Make a folder for Switch 2 games and copy your dump into it, one folder per game. Dolphin (the
file manager) is fine for this. The layout that the rest of this guide assumes is:

```
/home/deck/Emulation/roms/switch2/
    dk-bananza/
        exefs/
            main
            main.npdm
            ...
        romfs/
            ...
```

The game folder name is yours to pick; keep it to letters, digits and `-` so it is easy to type.

## 3. First launch from Konsole

Open **Konsole** (the terminal, in the application menu under System) and paste these lines one at
a time. The first two run once, the third every time you want to play.

```sh
mkdir -p ~/Applications && mv ~/Downloads/eglantine-steamdeck-*.AppImage ~/Applications/
chmod +x ~/Applications/eglantine-steamdeck-*.AppImage
EDEN_NX2_AUTO_STUB=1 ~/Applications/eglantine-steamdeck-*.AppImage -f -g ~/Emulation/roms/switch2/dk-bananza/exefs/main
```

Replace `dk-bananza` with your game's folder name. `EDEN_NX2_AUTO_STUB=1` tells the emulator to
stub the Switch 2 system calls it does not implement yet instead of stopping; the games need it.
`-f` is fullscreen.

The first start of a game sits on a black screen for a while and then stutters as shaders are
built; the second start is better. The Deck's own controls work as player 1 without setup.

To stop, go back to the Konsole window (Alt+Tab, or the taskbar) and press **Ctrl+C**.

If the window opens and closes right away, run the same command without `-f` and read what
Konsole printed. The emulator's own log is in `~/.local/share/eden/log/eden_log.txt`.

## 4. Optional: a Steam shortcut for Game Mode

1. In Desktop Mode, open Steam, then **Games > Add a Non-Steam Game to My Library**.
2. **Browse**, set the file type to *All Files*, and pick the AppImage in `/home/deck/Applications`.
3. Right-click the new entry > **Properties**. Rename it to the game, and set **Launch Options** to:

   ```
   EDEN_NX2_AUTO_STUB=1 %command% -f -g /home/deck/Emulation/roms/switch2/dk-bananza/exefs/main
   ```

4. Switch back to Game Mode and launch it like any other game. The Steam button > **Exit Game**
   stops it. Make one shortcut per game.

## Updating

Download the new `.AppImage`, delete the old one from `~/Applications`, and move the new one in.
The `chmod +x` step is needed again. Saves and the shader cache live in `~/.local/share/eden/`
and are kept.

## When something goes wrong

- **"Permission denied"**: the `chmod +x` line was skipped or the file was renamed after it.
- **Nothing happens, no window**: check the path after `-g` points at a file called `main`
  inside `exefs/`, not at the game folder itself.
- **The Deck freezes completely**: the game ran out of GPU memory. Hold the power button, and
  make sure nothing else was running. This is the known failure mode on 16 GB and the reason the
  fork exists; report the game and where it froze.
- **It is slow**: yes. See [docs/SteamDeck.md](../SteamDeck.md) for what is and is not done.

Problems go to the
[issue tracker](https://github.com/MFS-code/Eglantine-steamdeck/issues) with the game, the
release name, and `~/.local/share/eden/log/eden_log.txt` attached.
