Slopborne: Bloodborne for Mac
=============================

https://github.com/kryksyh/slopborne

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
2. Double-click it. The app is signed and notarized: macOS only asks once whether to open
   an app downloaded from the internet. Click Open.
3. Choose your Bloodborne game folder (the one that contains eboot.bin). The app remembers it.
4. The first start prepares the game files and compiles shaders: the first minutes can stutter.

Playing
-------
- Controller: any controller macOS supports (DualSense, DualShock 4, Xbox).
- Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square, Q Triangle,
  1/3 L1/R1, R/F L2/R2, Z/C L3/R3, I/K/J/L d-pad, Enter Options, Tab touchpad.
- Settings menu in the game: Cmd+, (or Insert) on the keyboard, L3+R3 on a controller.
  Display contains resolution, upscaling, the FPS counter and visual effects.
  Controls contains button glyphs and calibration. Gameplay contains convenience
  options and a separate Cheats section. Rendering experiments are in Advanced.
  Switch pages with L1/R1 (the shoulder buttons), Ctrl+Tab / Ctrl+Shift+Tab, or a mouse.
  Settings save when the menu closes. Pending restart changes stay visible at the
  bottom; restarting asks you to confirm because unsaved progress may be lost.
  The game continues running while this menu is open.
- The default frame-rate target is 30 FPS; actual performance depends on the scene.

Controller button prompts
-------------------------
Open settings with Cmd+, or L3+R3, then select Controls.
- Automatic reads the connected controller's face-button labels. On a Nintendo
  layout, PlayStation Cross corresponds to B, Circle to A, Square to Y, Triangle to X.
- If Steam or controller firmware hides the layout, choose Nintendo, Xbox or
  PlayStation under Button labels. Positions is available for unlabelled buttons.
- If your buttons are swapped in Steam, use Calibrate remapped face buttons:
  press and release the physical bottom, right, left and top face buttons in order.
  This supports a one-to-one remapping of the four face inputs. Options/Menu or
  Esc cancels without replacing your previous calibration.
- Calibration is saved for the detected controller type. Repeat it after changing
  Steam or firmware mappings. Labels never change gameplay bindings.
- The launcher generates matching face-button glyphs for the original game menus
  and dialogs in a separate asset view. Restart the game after changing labels or
  calibration. Original game files are never overwritten. Port name-entry prompts
  change immediately. Unknown controllers retain PlayStation glyphs until you
  choose a layout. Connect the controller before launching for automatic detection.
The game continues running while settings are open.

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
