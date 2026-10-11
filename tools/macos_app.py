#!/usr/bin/env python3
"""Makes the macOS app and its disk image (tools/macos_build.py, ``ninja macos``
and ``ninja macos_dmg``; port/macos/README.md).

    macos_app.py icon artwork.png Halo.icns
    macos_app.py bundle --app Halo.app --executable halo --image halo_guest.elf --sdl libSDL3.0.dylib
                        --angle ANGLE_DIR --musl MUSL_DIR --icon Halo.icns --info Info.plist
                        --entitlements Halo.entitlements --brokers brokers.txt --minimum 11.0
                        [--sign IDENTITY] --stamp Halo.app.stamp
    macos_app.py dmg --app Halo.app --output Halo.dmg [--sign IDENTITY]
    macos_app.py notarize --dmg Halo.dmg (--keychain-profile NAME | --key KEY.p8 --key-id ID --issuer ISSUER)

icon: the app's icon, every size macOS asks for, from the square artwork
(sips and iconutil, which macOS has).

bundle: Halo.app, made again from nothing: the host as Contents/MacOS/Halo;
SDL3 and ANGLE's libEGL and libGLESv2 in Contents/Frameworks (the host finds
SDL3 through its rpath, and gives SDL ANGLE's paths); the guest image,
brokers.txt, the icon and Licenses.txt (the notices of the code and fonts in
the app, which their licences ask copies to carry) in Contents/Resources;
Info.plist with the version (HALO_BUILD_NUMBER, as the other builds take it)
and the oldest macOS filled in. Signed with IDENTITY (a Developer ID
Application certificate's name, for an app that other Macs open without
asking: with the hardened runtime, and the entitlements the guest's code
and voice chat need), or else for this machine alone (an ad hoc signature,
which Apple silicon needs to run anything).

dmg: Halo.dmg, the app beside a link to /Applications, to drag it there; with
IDENTITY, signed.

notarize: sends the disk image to Apple's notary service, waits for it, and
staples the ticket to it, so that Gatekeeper opens the app without asking
even offline. The app must have been signed with a Developer ID.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ICON_SIZES = [16, 32, 128, 256, 512]

sys.path.insert(0, str(ROOT / "port" / "web"))
from licenses import NOTICES  # noqa: E402  (the notices the web build carries; the app carries them too)

# the app's own, besides the web build's: what its host and guest add
APP_NOTICES = [
    ("musl (the C library of the game's code)", None),
    ("Mbed TLS (internet play's DTLS with browsers)", "port/third_party/mbedtls/LICENSE"),
    ("miniupnpc (internet play's UPnP)", "port/third_party/miniupnpc/LICENSE"),
    ("SMAA (the anti-aliasing)", "port/third_party/smaa/LICENSE"),
]


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


def licenses_text(sdl_license: Path, angle_license: Path, musl_copyright: Path) -> str:
    parts = [
        "OpenCE for macOS\n\n"
        "OpenCE's own code is CC0 (https://github.com/OpenCommunityEdition/OpenCE). The game data\n"
        "comes from the player's own disc. The app carries the following, under their licences:\n",
        f"\n==================== SDL 3 (windows, input and audio)\n\n{sdl_license.read_text(encoding='utf-8')}",
        f"\n==================== ANGLE (OpenGL ES on Metal)\n\n{angle_license.read_text(encoding='utf-8')}",
    ]
    for title, path in APP_NOTICES + NOTICES:
        text = musl_copyright.read_text(encoding="utf-8") if path is None else (ROOT / path).read_text(encoding="utf-8")
        parts.append(f"\n==================== {title}\n\n{text}")
    return "".join(parts)


def sign(path: Path, identity: str, entitlements: Path = None) -> None:
    """signs path: with identity (a Developer ID), the hardened runtime and a
    secure timestamp, which notarization asks for; without, ad hoc"""
    command = ["codesign", "--force", "--sign", identity or "-"]
    if identity:
        command += ["--options", "runtime", "--timestamp"]
        if entitlements:
            command += ["--entitlements", str(entitlements)]
    subprocess.run(command + [str(path)], check=True, stdout=subprocess.DEVNULL)


def make_bundle(arguments: argparse.Namespace) -> None:
    app = Path(arguments.app)
    contents = app / "Contents"
    if app.exists():
        shutil.rmtree(app)
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Frameworks").mkdir()
    (contents / "Resources").mkdir()

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
    sdl_license = sdl.parent.parent / "third_party" / "SDL3" / "LICENSE.txt"
    (contents / "Resources" / "Licenses.txt").write_text(
        licenses_text(sdl_license, angle / "LICENSE", Path(arguments.musl) / "COPYRIGHT"), encoding="utf-8")

    number = os.environ.get("HALO_BUILD_NUMBER", "0")
    if not number.isdigit():
        number = "0"
    info = Path(arguments.info).read_text(encoding="utf-8")
    info = info.replace("@BUILD_NUMBER@", number).replace("@MINIMUM_MACOS@", arguments.minimum)
    (contents / "Info.plist").write_text(info, encoding="utf-8")
    (contents / "PkgInfo").write_text("APPL????", encoding="ascii")

    for library in sorted((contents / "Frameworks").iterdir()):
        sign(library, arguments.sign)
    sign(app, arguments.sign, Path(arguments.entitlements))
    Path(arguments.stamp).write_text("", encoding="ascii")


def make_dmg(arguments: argparse.Namespace) -> None:
    app = Path(arguments.app)
    output = Path(arguments.output)
    with tempfile.TemporaryDirectory() as temporary:
        staging = Path(temporary) / "Halo"
        staging.mkdir()
        # (ditto keeps the bundle whole: its signature, permissions and
        # extended attributes)
        subprocess.run(["ditto", str(app), str(staging / app.name)], check=True)
        (staging / "Applications").symlink_to("/Applications")
        if output.exists():
            output.unlink()
        subprocess.run(["hdiutil", "create", "-volname", "Halo", "-srcfolder", str(staging), "-fs", "HFS+",
                        "-format", "UDZO", "-imagekey", "zlib-level=9", "-ov", "-quiet", str(output)], check=True)
    if arguments.sign:
        sign(output, arguments.sign)


def notarize(arguments: argparse.Namespace) -> None:
    command = ["xcrun", "notarytool", "submit", str(arguments.dmg), "--wait"]
    if arguments.keychain_profile:
        command += ["--keychain-profile", arguments.keychain_profile]
    elif arguments.key and arguments.key_id and arguments.issuer:
        command += ["--key", arguments.key, "--key-id", arguments.key_id, "--issuer", arguments.issuer]
    else:
        raise SystemExit("notarize: give --keychain-profile, or --key, --key-id and --issuer")
    result = subprocess.run(command, capture_output=True, text=True)
    print(result.stdout + result.stderr)
    if result.returncode != 0 or "status: Accepted" not in result.stdout:
        raise SystemExit("notarize: the notary service did not accept the disk image (xcrun notarytool log <id> "
                         "has why)")
    subprocess.run(["xcrun", "stapler", "staple", str(arguments.dmg)], check=True)


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "icon":
        if len(sys.argv) != 4:
            raise SystemExit(__doc__)
        make_icon(Path(sys.argv[2]), Path(sys.argv[3]))
        return
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    bundle = commands.add_parser("bundle")
    for name in ("app", "executable", "image", "sdl", "angle", "musl", "icon", "info", "entitlements", "brokers",
                 "minimum", "stamp"):
        bundle.add_argument(f"--{name}", required=True)
    bundle.add_argument("--sign", default="")
    dmg = commands.add_parser("dmg")
    dmg.add_argument("--app", required=True)
    dmg.add_argument("--output", required=True)
    dmg.add_argument("--sign", default="")
    notary = commands.add_parser("notarize")
    notary.add_argument("--dmg", required=True)
    notary.add_argument("--keychain-profile")
    notary.add_argument("--key")
    notary.add_argument("--key-id")
    notary.add_argument("--issuer")
    arguments = parser.parse_args()
    if arguments.command == "bundle":
        make_bundle(arguments)
    elif arguments.command == "dmg":
        make_dmg(arguments)
    else:
        notarize(arguments)


if __name__ == "__main__":
    main()
