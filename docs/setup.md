# Setting up the toolchain

How we got a CC2650 firmware build working on Linux (Fedora 44, x86-64) without Code
Composer Studio. TI built these tools for Windows and CCS in 2016–2018; most have Linux
downloads, one doesn't, and the rest of the gap is in `build.py` and `tools/`.

The versions are the ones BLE-Stack 2.2.8's release notes name. Use exactly these:
older stacks are picky, and `build.py` has the version numbers in its paths.

| Piece | Version | Where it runs | Download |
|---|---|---|---|
| BLE-Stack SDK | 2.02.08.12 | Windows installer, once | ti.com, see below |
| TI ARM compiler | 16.9.4.LTS | Linux | software-dl.ti.com |
| TI-RTOS for CC13xx/CC26xx | 2.21.01.08 | Linux | software-dl.ti.com |
| XDCtools | 3.32.00.06 core | Linux | software-dl.ti.com |

Everything goes in one folder, `~/apps/ti` by default; set `TI_ROOT` to use another.
In the end it looks like this:

    ~/apps/ti/
      simplelink/ble_sdk_2_02_08_12/
      ti-cgt-arm_16.9.4.LTS/
      tirtos_cc13xx_cc26xx_2_21_01_08/
      xdctools_3_32_00_06_core/

## 1. The SDK, on Windows

`ble_sdk_2_02_08_12_setup.exe` (237 MB) is on TI's BLE-Stack 2.x download page,
<https://www.ti.com/tool/download/BLE-STACK-2-X/2.02.08.12>. Ours had sha256
`e1e191c23432a29f22ba42a594b3e319347b97a51b8eaa072ee554e614beac1c`.

It's a BitRock installer, and nothing on Linux unpacks it: 7-Zip sees one opaque blob.
Wine may run it; we didn't try. We ran it on a Windows machine:

1. Install to `C:\ti` (TI's default). Avoid paths with spaces such as `Program Files`;
   TI's build steps of that era break on them.
2. Copy `C:\ti\simplelink` to `~/apps/ti/simplelink` on the Linux machine. A plain
   folder copy over a Samba share worked.

The installer also puts Windows builds of TI-RTOS and XDCtools in `C:\ti`. Don't copy
those; step 2 installs the Linux builds.

Running an installer straight from a Samba share fails with a permission error unless the
file has the Linux execute bit (`chmod +x`).

## 2. Compiler, TI-RTOS and XDCtools, on Linux

All three are public downloads, no login:

    mkdir -p ~/down/ti ~/apps/ti && cd ~/down/ti
    curl -fLO https://software-dl.ti.com/codegen/esd/cgt_public_sw/TMS470/16.9.4.LTS/ti_cgt_tms470_16.9.4.LTS_linux_installer_x86.bin
    curl -fLO https://software-dl.ti.com/dsps/dsps_public_sw/sdo_sb/targetcontent/tirtos/2_21_01_08/exports/tirtos_cc13xx_cc26xx_setuplinux_2_21_01_08.bin
    curl -fLO https://software-dl.ti.com/dsps/dsps_public_sw/sdo_sb/targetcontent/rtsc/3_32_00_06/exports/xdccore/xdctools_3_32_00_06_core_linux.zip
    sha256sum *

TI publishes no checksums. Ours were:

    c6b2ebd25c1945694719a09d462201912c848e434e656aa0ccbea99b8ffc2f68  ti_cgt_tms470_16.9.4.LTS_linux_installer_x86.bin
    95f58837d98983bf24bf8717dc5e0ad1c8d132858f7a7fce1b263037f0e86872  tirtos_cc13xx_cc26xx_setuplinux_2_21_01_08.bin
    d6f38a7f846ae4b1478554d75fc5f72cf677a1e2f348e214a725d1e849bd39c7  xdctools_3_32_00_06_core_linux.zip

The two installers and the compiler are 32-bit programs. Without 32-bit libraries the
installers exit at once with status 127. On Fedora:

    sudo dnf install glibc.i686 libstdc++.i686

Then install, without prompts:

    cd ~/apps/ti && unzip -q ~/down/ti/xdctools_3_32_00_06_core_linux.zip
    cd ~/down/ti && chmod +x *.bin
    ./ti_cgt_tms470_16.9.4.LTS_linux_installer_x86.bin --mode unattended --prefix ~/apps/ti
    ./tirtos_cc13xx_cc26xx_setuplinux_2_21_01_08.bin --mode unattended --prefix ~/apps/ti

The TI-RTOS installer runs for several minutes with no output. Check with
`~/apps/ti/ti-cgt-arm_16.9.4.LTS/bin/armcl -version`, and the end of the install log in
`~/apps/ti/tirtos_cc13xx_cc26xx_2_21_01_08/` should say "Installation completed".

## 3. Python packages

    pip install --user intelhex bleak

`intelhex` builds update images; `bleak` (BlueZ over D-Bus) sends them and listens to the
tag. `build.py` also needs `git`, to stamp the commit into the firmware.

## 4. Build

    ./build.py stack && ./build.py app --oad && ./build.py oad

The result, `build/oad_merged.bin`, is the image to send (README, and `device.md` for
updates).

## What we wrote to replace CCS

CCS is TI's Eclipse IDE. It builds from the project files in the SDK and calls a few
Windows helper programs. We didn't install it; instead:

- **`build.py`** reads the SDK's CCS project files (`.project`, `.cproject`) for the
  SensorTag stack and app, and runs the compiler, linker, XDC configuration and helper
  steps in the order CCS would. Files in `src/<part>/` replace the SDK's file of the same
  name, so the SDK itself stays untouched.
- **TI's helpers** (`lib_search`, `frontier`, `oad_image_tool`) ship as Windows `.exe`
  files with Python 2 sources in the SDK's `tools/`. `tools/` here holds those sources
  ported to Python 3, under TI's licence.
- **Letter case.** The SDK was written on Windows, so some `#include`s don't match the
  file names' case (`OSAL_Memory.h` for `osal_memory.h`). `build.py` resolves each one
  the way Windows would and puts symlinks in `build/caseshim/`, searched first.
- **Generated files** that CCS writes into the SDK's `ccs/config/` go to `build/config/`
  instead. One exception: XDC still writes a ROM kernel library into the SDK's
  `examples/cc2650stk/sensortag/ccs/config/src/`.
- **Sending updates.** TI's BTool needs a CC2650 LaunchPad as its radio.
  `tools/oad_send.py` speaks the same over-the-air update protocol through any BlueZ
  adapter.

None of this was compared byte for byte with a CCS build, because TI's prebuilt images
use a different compiler. It's tested the practical way: our images run on the tag.
