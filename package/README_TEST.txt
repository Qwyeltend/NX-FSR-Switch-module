NX-FSR - diagnostic build (CAS sharpening, comparison mode)
==========================================================

This package is the official SaltyNX 2.1.4 release with one file replaced:
SaltySD/saltynx_core.elf (SaltyNX Core with NX-FSR built in).

INSTALL
1. Back up the SaltySD folder on your SD card.
2. Copy the contents of this package to the root of the SD card, replacing files.
3. Reboot.

WHAT YOU SHOULD SEE
In game, the left half of the screen is sharpened, the right half is unchanged,
with a thin yellow vertical line between them.

SETTINGS (per game)
Created on the first launch of a game:
  sdmc:/config/fsrSwitch/<TITLEID>.ini
  enabled=1       (0 = off for this game)
  sharpness=0.50  (0.0 soft ... 1.0 strong)
  split=1         (1 = left half only, 0 = whole screen)
Changes apply on the next launch of the game.

TURN EVERYTHING OFF
Create an empty file: sdmc:/config/fsrSwitch/disabled.flag

LOGS
  sdmc:/config/fsrSwitch/log_<TITLEID>.txt
  sdmc:/SaltySD/saltynx_core.log

IF SOMETHING GOES WRONG
- Game doesn't start or crashes: create disabled.flag and launch the game again.
- To restore everything: put back your SaltySD backup.
