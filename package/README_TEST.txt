NX-FSR 0.2.0
============

This package is the official SaltyNX 2.1.4 release with SaltyNX Core replaced
(SaltySD/saltynx_core.elf, NX-FSR built in), plus the NX-FSR overlay
(switch/.overlays/nx-fsr.ovl).

INSTALL
1. Back up the SaltySD folder and atmosphere/contents/0000000000534C56 on your SD card.
2. Copy the contents of this package to the root of the SD card, replacing files.
3. Reboot (Power menu -> Restart).

USE
Start a game, open the overlay menu (Tesla / Ultrahand) and choose NX-FSR:
  Effect          on / off for this game
  Sharpness       0 - 100 %
  Compare         effect on the left half only, line in the middle
  Picture flip    Auto / Off / On (if the picture is upside down)
Changes apply immediately and are saved for this game.

TURN EVERYTHING OFF
Create an empty file: sdmc:/config/fsrSwitch/disabled.flag

LOGS
  sdmc:/config/fsrSwitch/log_<TITLEID>.txt
  sdmc:/SaltySD/saltynx_core.log

IF SOMETHING GOES WRONG
- Game doesn't start or crashes: create disabled.flag and launch the game again.
- To restore everything: put back your backups.
