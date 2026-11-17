# Recovery

Before flashing any ERISIAN build:

1. Full flash backup in M-Flasher ("Back up the whole flash"). Keep two copies, one off this computer.
2. Know your stock version. If V16, return to V15 via the Firmware Hub first.
3. Rehearse once: reinstall V15 on purpose and confirm it boots.

## If something goes wrong

1. **Firmware still runs:** the installer's "Return to official V15", or M-VAVE's updater.
2. **Firmware broken, USB still seen:** hold OCT− at power-on (rescue mode; ERISIAN
   will support this from Phase 4), or enter download mode with M-Flasher, then
   restore V15 or your full backup.
3. **Black screen, nothing on USB:** the ROM bootloader can't be erased and still
   answers. Use the [FM1 Unbricker](https://quixotic7.github.io/MvaveFM1Unbricker/)
   or the [Firmware Hub rescue page](https://thefirmwarehub.com/tools/rescue/).
   Some cases need the FM-1 Transporter hardware.
4. Never unplug the unit while it's writing.
