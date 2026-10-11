# macOS

`ninja macos` builds the game for Macs with Apple silicon (M1 and later).
The result is an app, `build/macos/Halo.app`.

The game shows its graphics with OpenGL ES 3, which ANGLE translates to
Metal. It plays sound through SDL3. It accepts keyboard, mouse and gamepad
input.

The macOS build uses the platform layer of the Linux build
(`port/linux/src`). The graphics, the sound, the controls, the menus, the
settings and the multiplayer functions are the same as on Linux. Refer to
[port/linux/README.md](../linux/README.md). The differences are below.

## Requirements

You do not need the Xbox SDK. To build:

- macOS 11 or later on a Mac with Apple silicon.
- The Xcode command line tools (`xcode-select --install`): clang, and the
  macOS SDK.
- Python 3, ninja and CMake.
- LLVM's linker and archiver, `ld.lld` and `llvm-ar`. For example, enter
  `brew install llvm lld ninja cmake`. `configure.py` looks for them in the
  `PATH` and in Homebrew's `llvm` and `lld`. The guest is compiled with
  Homebrew's clang if there is one (it can use the optimization profiles in
  `pgo/`), else with the clang of the system. The option
  `--macos-guest-cc` of `configure.py` selects a different compiler.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5, SDL 3.4.16 and ANGLE to `build/macos/third_party`.

ANGLE is a build of Chromium's branch 7151 made by Kivy's
[angle-builder](https://github.com/kivy/angle-builder)
(`angle-macos-arm64.tar.gz` of its release `chromium-7151_rev1`).
`configure.py` checks the SHA-256 of the download. To use a different
ANGLE build, give its folder (with `libEGL.dylib`, `libGLESv2.dylib` and
`include/`) to `configure.py` with the option `--macos-angle`.

To start the game: macOS 11 or later on a Mac with Apple silicon. Intel
Macs cannot run the game (refer to "How the port operates").

## Build the game

1. Go to the root folder of the repository.
2. Enter `python3 configure.py`.
3. Enter `ninja macos`. On a Mac, `ninja` alone does the same.

`configure.py` generates the macOS build only on macOS. To start the game,
open `build/macos/Halo.app` in the Finder, or enter
`open build/macos/Halo.app`. You can move the app to `/Applications`.

`ninja macos_dmg` also makes `build/macos/Halo.dmg`, a disk image with the
app beside a link to `/Applications`: the download for other Macs. The app
carries the notices of the code and fonts in it (`Licenses.txt`, in its
`Resources`).

### Sign and notarize

The app is signed for the computer that built it (an ad hoc signature).
Another Mac opens it only after the person allows it: open the app, then
System Settings > Privacy & Security > "Open Anyway". A notarized app opens
without this. To notarize it, you need a membership of the Apple Developer
Program and its Developer ID Application certificate in your keychain:

1. Enter `python3 configure.py --release --macos-sign "Developer ID Application: <name> (<team>)"`.
   The app is then signed with the hardened runtime and the entitlements in
   `Halo.entitlements` (the guest's code needs `allow-jit`, voice chat the
   microphone).
2. Enter `ninja macos_dmg`.
3. Store the notary service's credentials once:
   `xcrun notarytool store-credentials halo --apple-id <Apple ID> --team-id <team>`
   (it asks for an app-specific password).
4. Enter `python3 tools/macos_app.py notarize --dmg build/macos/Halo.dmg --keychain-profile halo`.
   It waits for Apple's answer, then staples the ticket to the disk image.

## Game data

At the first start, the game asks for the disc image (`.xiso` or `.iso`)
of Halo: Combat Evolved, and extracts its `maps` folder, as on Linux. The
app itself cannot be changed (it is signed), so the game keeps its files
in its data folder, `~/Library/Application Support/OpenCE`:

| Item | Location in the data folder |
| --- | --- |
| Maps | `maps` |
| Custom Edition maps | `custom_maps` |
| Saved games (`z:\` and `u:\`) | `save` |
| Settings | `config.toml` |
| The game's log | `debug.txt` |
| The log of the app (when it is not started from a terminal) | `halo.log` |
| Screenshots (F10) | next to `maps` |

To open the folder, enter `open ~/Library/Application\ Support/OpenCE`.
The environment variable `HALO_DATA_ROOT` selects a different data folder
(for example, `HALO_DATA_ROOT=~/halo open build/macos/Halo.app`). The game
reads the other settings' environment variables (`HALO_*`, refer to
"Settings" in the Linux README) when it is started from a terminal:

```sh
HALO_FULLSCREEN=0 build/macos/Halo.app/Contents/MacOS/Halo
```

## Differences from Linux

- The game does not update itself. Build it again from the repository.
- Invite links (`halo://join/...`) do not open the game. Copy the link and
  go to the game: it joins the invite on the clipboard (as on Android).
- The first use of voice chat asks for permission to use the microphone,
  and the first network game for permission to find devices on the local
  network. Allow both in System Settings > Privacy & Security if you
  refused them.

## How the port operates

### The guest

The data of the game contains 32-bit pointers, as on the Xbox (refer to
"How the port operates" in [port/android/README.md](../android/README.md)).
The Android build runs the game as ILP32 AArch64 code: 64-bit ARM
instructions with 32-bit pointers, in a 64-bit app, with the memory of the
game below 4 GB, where its 32-bit addresses are the addresses of the
process.

macOS on Apple silicon does not let an arm64 process use the memory below
4 GB: its `__PAGEZERO` must cover the first 4 GB, or the kernel stops the
program. (An Intel program under Rosetta can use it, but Apple is retiring
Rosetta.) So the macOS build runs the same guest as Android (the game, the platform
layer and musl, compiled for `arm64_32`), but with the memory of the game in
a window: guest address A is host address base + A. The base is a 4 GB
boundary that the host chooses at start-up.

The compiler makes code for addresses below 4 GB. `tools/macos_sfi.py`
changes the code so that it uses the window (the guest is compiled with
`-ffixed-x27 -ffixed-x28`, so the compiler does not use these registers):

- x28 holds the base. Each load, store, atomic operation and prefetch with
  a general register as its base reads or writes x28 + the low 32 bits of
  the guest address. x27 holds the intermediate address when the
  instruction has no form with a register offset.
- The stack is in the window. sp and x29 hold host addresses, and accesses
  relative to them do not change. A value made from sp or x29, or by `adr`
  or `adrp`, keeps only its low 32 bits, the guest address. The base is a
  4 GB boundary, so the two agree. So does a value made from x30, the
  return address, in a function that only reads x30. A function that writes
  x30 (other than to restore the return address) uses it as one more
  register, and its values stay whole: they can be 64-bit integers, which
  SHA-512 and the curves of internet play's keys and signatures use.
- `br` and `blr` go to x28 + the low 32 bits of the target. sp that is set
  from a general register gets the base again.
- Jump tables have 4-byte entries
  (`-aarch64-enable-compress-jump-tables=false`), because the changed code
  is longer. For the same reason, a `tbz` or `tbnz` whose target goes out of
  range becomes an inverted branch around a `b`.

The script stops the build at an instruction that its rules do not cover,
for example a 64-bit comparison with sp.

### The host

The host (`port/macos/host`) is an arm64 executable:

- It reserves the window, with unmapped space on each side
  (`host_memory.c`). The pools of the window supply the memory that the
  guest maps without an address. The guest has the fixed ranges: the Xbox
  memory (0x80000000, 512 MB as on the desktop builds), the Custom Edition
  tag cache (0x40440000) and the image (0xA0000000, `guest/guest.ld`).
- macOS on Apple silicon has 16 KB pages; the guest has 4 KB pages. The host
  records the window in 4 KB pages. A mapping that covers a 16 KB page
  replaces it; one that covers a part of it sets that part to zero.
- It loads the image (`host_loader.c`). Executable memory that is not a
  signed file must come from `MAP_JIT`. The code of the image is mapped
  that way at its address in the window.
- It does the Linux system calls of the guest's musl with the system calls
  of macOS (`host_syscall.c`): the flags, the structures and the errno
  values are changed. Futexes use `os_sync_wait_on_address` (or `__ulock`
  before macOS 14.4).
- It starts every thread that runs guest code with its stack in the window
  (`host_thread.c`). The game's main thread is the main thread of the
  process, because SDL uses the main thread for windows and events on
  macOS. It moves to a stack in the window.
- It gives SDL3 and OpenGL ES to the guest. The import stubs
  (`tools/macos_imports.py`) change the pointer arguments of the host
  functions to host addresses. The OpenGL ES functions and the `posix_*`
  functions of the platform layer do this in their generated guest
  wrappers (`tools/android_gl_stubs.py` and `tools/android_posix_stubs.py`
  with `--window`). The pointers of vertex attributes and indices are
  offsets in buffers and do not change. SDL's objects are small handles in
  the guest (`host_sdl.c`, and the desktop's functions in
  `guest/guest_sdl_desktop.c`).
- It answers the questions of Accessibility clients as they come
  (`host_cocoa.m`). AppKit answers them on the main thread when the game
  pumps its events, once a frame. A window manager that asks a series of
  them from an event tap holds the event until it has the answers: Magnet
  asks about twenty on each left click, so the click, and the mouse's
  motion behind it, came about 140 ms late. After a pump that answers a
  question, the main thread answers the next ones as they come, for up to
  4 ms.

The guest sees the data folder as the folder of its executable
(`/proc/self/exe`), and starts in it, so the desktop code finds `maps/`,
`config.toml` and `menus/` there.

### Defines

The guest is compiled with `HALO_ARM64_GUEST`, `HALO_GLES` and
`HALO_MACOS` (not `HALO_ANDROID`): the guest ABI and the OpenGL ES renderer
of Android, and the desktop's window, input and files. `HALO_MACOS` also
gives the 512 MB Xbox memory window and the caches of the desktop builds
(`platform.h`, `halo_port_capacity.h`), and turns off the self-updater.

## Find problems

- The log of the app is on standard error when you start it from a
  terminal (`build/macos/Halo.app/Contents/MacOS/Halo`), and in `halo.log`
  in the data folder otherwise.
- If the code stops, the log shows the registers and the frame chain, with
  guest addresses. To find the functions, enter
  `llvm-symbolizer --obj=build/macos/halo_guest.elf <address>`.
- `HALO_MEMORY_WATCH=0` makes the host compare page contents to find the
  game's writes to textures and vertices, instead of page protection.
- If the clicks or the aim lag, an app that watches the mouse can hold
  each click while it works: a window manager (for example Magnet, which
  snaps windows that you drag), or a mouse utility. Add the game to the
  app's ignored apps, or quit the app while you play.

## Limits

- Apple silicon only.
- Bink video is not available. The game skips the movies.
- The app is not notarized, and has no hardened runtime (with it, the code
  of the guest needs the `com.apple.security.cs.allow-jit` entitlement).
