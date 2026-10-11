#!/usr/bin/env python3
"""Makes the macOS app (tools/macos_build.py, ``ninja macos``).

    macos_app.py icon artwork.png Halo.icns
    macos_app.py bundle --app Halo.app --executable halo --image halo_guest.elf --sdl libSDL3.0.dylib
                        --angle ANGLE_DIR --icon Halo.icns --info Info.plist --brokers brokers.txt
                        --minimum 11.0 --stamp Halo.app.stamp

icon: the app's icon, every size macOS asks for, from the square artwork
(sips and iconutil, which macOS has).

bundle: Halo.app, made again from nothing: the host as Contents/MacOS/Halo;
SDL3 and ANGLE's libEGL and libGLESv2 in Contents/Frameworks (the host finds
SDL3 through its rpath, and gives SDL ANGLE's paths); the guest image,
brokers.txt, the icon and the libraries' licences in Contents/Resources;
Info.plist with the version (HALO_BUILD_NUMBER, as the other builds take
it) and the oldest macOS filled in. The app is signed for this machine (an
ad hoc signature, which Apple silicon needs to run it), without the
hardened runtime, under which the guest's code would need the JIT
entitlement.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ICON_SIZES = [16, 32, 128, 256, 512]


def make_icon(artwork: Path, output: Path) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        iconset = Path(temporary) / "Halo.iconset"
        iconset.mkdir()
        for size in ICON_SIZES:
            for scale in (1, 2):
                pixels = size * scale
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                subprocess.run(["sips", "-z", str(pixels), str(pixels), str(artwork), "--out", str(iconset / name)],
                               check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(output)], check=True)


def make_bundle(arguments: argparse.Namespace) -> None:
    app = Path(arguments.app)
    contents = app / "Contents"
    if app.exists():
        shutil.rmtree(app)
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Frameworks").mkdir()
    (contents / "Resources" / "Licenses").mkdir(parents=True)

    shutil.copy2(arguments.executable, contents / "MacOS" / "Halo")
    sdl = Path(arguments.sdl)
    # (the library under its install name, @rpath/libSDL3.0.dylib)
    shutil.copy2(sdl.resolve(), contents / "Frameworks" / "libSDL3.0.dylib")
    angle = Path(arguments.angle)
    for name in ("libEGL.dylib", "libGLESv2.dylib"):
        shutil.copy2(angle / name, contents / "Frameworks" / name)
    shutil.copy2(arguments.image, contents / "Resources" / "halo_guest.elf")
    shutil.copy2(arguments.brokers, contents / "Resources" / "brokers.txt")
    shutil.copy2(arguments.icon, contents / "Resources" / "Halo.icns")
    licences = {
        "ANGLE.txt": angle / "LICENSE",
        "SDL3.txt": sdl.parent.parent / "third_party" / "SDL3" / "LICENSE.txt",
    }
    for name, source in licences.items():
        if source.is_file():
            shutil.copy2(source, contents / "Resources" / "Licenses" / name)

    number = os.environ.get("HALO_BUILD_NUMBER", "0")
    if not number.isdigit():
        number = "0"
    info = Path(arguments.info).read_text(encoding="utf-8")
    info = info.replace("@BUILD_NUMBER@", number).replace("@MINIMUM_MACOS@", arguments.minimum)
    (contents / "Info.plist").write_text(info, encoding="utf-8")
    (contents / "PkgInfo").write_text("APPL????", encoding="ascii")

    for library in sorted((contents / "Frameworks").iterdir()):
        subprocess.run(["codesign", "--force", "--sign", "-", str(library)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    Path(arguments.stamp).write_text("", encoding="ascii")


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "icon":
        if len(sys.argv) != 4:
            raise SystemExit(__doc__)
        make_icon(Path(sys.argv[2]), Path(sys.argv[3]))
        return
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["bundle"])
    for name in ("app", "executable", "image", "sdl", "angle", "icon", "info", "brokers", "minimum", "stamp"):
        parser.add_argument(f"--{name}", required=True)
    make_bundle(parser.parse_args())


if __name__ == "__main__":
    main()
