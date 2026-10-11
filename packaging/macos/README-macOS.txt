Bloodborne for Mac
==================

The original PlayStation 4 game running on your Mac: its x86-64 code runs through Rosetta and
the graphics go to Metal. No game files are included: you need your own decrypted dump of
Bloodborne (version 1.09 recommended). This project is not affiliated with Sony Interactive
Entertainment or FromSoftware.

Requirements
------------
- A Mac with Apple Silicon (M1 or newer), macOS 15 Sequoia or newer
- 16 GB of memory or more. The game needs about 8 GB free: close browsers, virtual machines
  and other large apps before playing, otherwise the game stutters when it loads new areas.
- Rosetta (the app offers to install it if it is missing)

First start
-----------
1. Move Bloodborne.app to your Applications folder (or anywhere you like).
2. Double-click it. macOS blocks apps from unidentified developers the first time:
   open System Settings > Privacy & Security, scroll down to "Bloodborne was blocked",
   click "Open Anyway" and confirm with your password. You only do this once.
3. Choose your Bloodborne game folder (the one that contains eboot.bin). The app remembers it.
4. The first start prepares the game files and compiles shaders: the first minutes can stutter.

Playing
-------
- Controller: any controller macOS supports (DualSense, DualShock 4, Xbox).
- Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square, Q Triangle,
  1/3 L1/R1, R/F L2/R2, Z/C L3/R3, I/K/J/L d-pad, Enter Options, Tab touchpad.
- Settings menu in the game: Cmd+, (or Insert) on the keyboard, L3+R3 on a controller.
  Effects, cheats, the FPS counter and the resolution are there.
- The game runs at 30 FPS with even frame pacing.

Your files
----------
Everything the app writes is in ~/Library/Application Support/Bloodborne
(Finder: Go > Go to Folder, paste that path):
- user/savedata  your saves
- backups        a copy of your saves from each start when they changed (all copies retained)
- bbport.ini     the settings of the in-game menu
- logs           the log of the last two starts
- config.json    the game folder (delete this file to choose another folder)

Importing a save: drag a save folder (SPRJ0005, from shadPS4 or a decrypted PS4 save) onto
Bloodborne.app. Your current saves are backed up first.

When something goes wrong
-------------------------
If the game closes unexpectedly, the app offers to put a crash report on your Desktop: attach
it to your bug report together with your Mac model.
Uninstall: delete Bloodborne.app and the folder ~/Library/Application Support/Bloodborne.
