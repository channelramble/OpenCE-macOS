"""Ninja rules for the macOS build (``ninja macos``).

The macOS port (port/macos/README.md) runs the game as the Android port does:
as ILP32 AArch64 code - 32-bit pointers, as the game's data formats require -
inside an ordinary 64-bit process. macOS gives an arm64 process no memory
below 4 GB, so the host reserves a 4 GB window elsewhere and the guest's code
is rewritten to address it (tools/macos_sfi.py). This graph builds

- the guest image, build/macos/halo_guest.elf: the sources the Android guest
  is built from (tools/android_build.py), compiled the same way but for the
  desktop (HALO_MACOS rather than HALO_ANDROID), rewritten for the window and
  linked at a fixed address;
- the host, build/macos/host/halo: an arm64 executable, the loader and the
  services the guest calls, over SDL3 and OpenGL ES (ANGLE, on Metal);
- SDL3 itself, with CMake;
- the app, build/macos/Halo.app, holding the host, SDL3, ANGLE and the image,
  and (``ninja macos_dmg``) its disk image, build/macos/Halo.dmg, to drag it
  to /Applications from.

configure.py downloads musl and SDL's source as the Android build does, and
ANGLE's libraries (a build of Kivy's angle-builder, checked against its
SHA-256), to build/macos/third_party. The guest needs a clang with the
arm64_32 target, ld.lld and llvm-ar (Homebrew's llvm and lld).
"""

import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, List, Optional

from .android_build import (EXPAT_DIR, EXPAT_SOURCES, GUEST_ABI_FLAGS, GUEST_CODE_FLAGS, KCP_DIR, MBEDTLS_DIR,
                            MONOCYPHER_DIR, MUSL_DIRECTORIES, MUSL_EXCLUDE, MUSL_FILES, MUSL_THREAD_PREFIXES,
                            MUSL_URL, MUSL_VERSION, SDL_TAG, SDL_URL, TOML_DIR, VARIADIC_PROTOTYPE_FILES,
                            ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES)
from .embed_assets import hud_assets_build, hud_configure_inputs
from .linux_build import (LINUX_PROFILE, MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, XDK_INCLUDE,
                          compile_launcher, configuration_defines, game_defines_and_includes, game_sources,
                          miniupnpc_sources, musl_math_sources, opus_cflags, opus_sources, pgo_mode, pgo_profile,
                          profile_use_flags, updater_defines, xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/macos")
THIRD_PARTY = BUILD / "third_party"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
SDL_DIR = THIRD_PARTY / "SDL3"
# OpenGL ES 3 on Metal: ANGLE, as Chrome and Safari draw WebGL with on the
# Mac, built by Kivy's angle-builder (https://github.com/kivy/angle-builder)
# from Chromium's branch 7151; or configure.py --macos-angle DIRECTORY with
# libEGL.dylib, libGLESv2.dylib and include/ of another build
ANGLE_DIR = THIRD_PARTY / "angle"
ANGLE_URL = "https://github.com/kivy/angle-builder/releases/download/chromium-7151_rev1/angle-macos-arm64.tar.gz"
ANGLE_SHA256 = "fea89ee72ebb3426ef8fd5ec90db463f9b06beb74ea858a1091bcef5a2e99b73"
# the oldest macOS the app starts on (Apple silicon shipped with macOS 11)
MINIMUM_MACOS = "11.0"

# The guest ABI: the Android guest's (tools/android_build.py), for the desktop
# rather than the Android app, with x27 and x28 left to the window rewrite
# (tools/macos_sfi.py) and jump tables of 32-bit entries, which the rewrite's
# longer code cannot outgrow
GUEST_ABI_FLAGS_MACOS = [
    "-DHALO_MACOS=1" if flag == "-DHALO_ANDROID=1" else flag for flag in GUEST_ABI_FLAGS
] + [
    "-ffixed-x27",
    "-ffixed-x28",
    "-mllvm",
    "-aarch64-enable-compress-jump-tables=false",
]

HOST_FRAMEWORKS = ["Cocoa", "Foundation", "QuartzCore"]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _homebrew_tool(name: str) -> Optional[str]:
    """a tool on the PATH, else in Homebrew's llvm or lld"""
    found = shutil.which(name)
    if found:
        return found
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin", "/usr/local/opt/llvm/bin",
                   "/usr/local/opt/lld/bin"):
        candidate = Path(prefix) / name
        if candidate.is_file():
            return str(candidate)
    return None


def _guest_compiler(sln: Any) -> str:
    """the guest's clang: --macos-guest-cc, else Homebrew's (its LLVM matches
    ld.lld's, and it can use the profiles in pgo/), else the system's"""
    chosen = getattr(sln, "macos_guest_cc", None)
    if chosen:
        return chosen
    for candidate in ("/opt/homebrew/opt/llvm/bin/clang", "/usr/local/opt/llvm/bin/clang"):
        if Path(candidate).is_file():
            return candidate
    return "clang"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_third_party(angle: Optional[Path]) -> None:
    """Download musl, SDL3 and ANGLE (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not MUSL_DIR.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)
    if angle is None and not (ANGLE_DIR / "libGLESv2.dylib").is_file():
        print(f"Downloading {ANGLE_URL}")
        archive = THIRD_PARTY / "angle.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), ANGLE_URL], check=True)
        if _sha256(archive) != ANGLE_SHA256:
            archive.unlink()
            raise OSError(f"{ANGLE_URL} does not have the expected SHA-256")
        ANGLE_DIR.mkdir(parents=True, exist_ok=True)
        subprocess.run(["tar", "xzf", str(archive.resolve())], cwd=ANGLE_DIR, check=True)
        archive.unlink()


def _musl_sources() -> List[Path]:
    """musl's sources the guest uses (as tools/android_build.py's)"""
    src = MUSL_DIR / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


def macos_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "host", PORT_DIR / "guest", ANDROID_DIR / "guest" / "runtime",
            LINUX_DIR / "src", *hud_configure_inputs()]


def generate_macos_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if sys.platform != "darwin" or not config_path.is_file() or not (PORT_DIR / "host").is_dir():
        return
    lld = _homebrew_tool("ld.lld")
    ar = _homebrew_tool("llvm-ar")
    if not lld or not ar:
        n.comment("macOS build: needs ld.lld and llvm-ar (brew install llvm lld)")
        print("macOS build disabled: ld.lld and llvm-ar are needed (brew install llvm lld)", file=sys.stderr)
        return
    angle = Path(sln.macos_angle) if getattr(sln, "macos_angle", None) else None
    try:
        fetch_third_party(angle)
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"macOS build disabled: cannot fetch musl, SDL3 or ANGLE ({error})", file=sys.stderr)
        return
    angle = angle or ANGLE_DIR
    import json
    config = json.loads(config_path.read_text(encoding="utf-8"))

    guest_cc = _guest_compiler(sln)
    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    gl_include = angle / "include"
    arch = ANDROID_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    sdl_build = BUILD / "sdl3-build"
    libsdl = sdl_build / "libSDL3.0.dylib"
    host_dir = BUILD / "host"
    host_executable = host_dir / "halo"
    python = "$python"

    n.comment("macOS build (ninja macos); see port/macos/README.md")
    n.variable("macos_guest_cc", guest_cc)
    n.variable("macos_lld", lld)
    n.variable("macos_ar", ar)

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="macos_alltypes",
        command=f"sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="MACOS MUSL $out",
    )
    n.build(outputs=alltypes, rule="macos_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="macos_syscall_h",
        command="cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="MACOS MUSL $out",
    )
    n.build(outputs=syscall_h, rule="macos_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name="macos_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="MACOS MUSL $out",
    )
    n.build(outputs=version_h, rule="macos_version_h")
    # the guest's Linux system call numbers, for the host (host_syscall.c)
    host_syscall_numbers = host_dir / "guest_syscall_numbers.h"
    n.rule(
        name="macos_host_syscall_numbers",
        command="sed -n -e 's/^#define __NR_\\([a-z0-9_]*\\)[[:space:]]*\\([0-9]*\\)/#define GUEST_SYS_\\1 \\2/p' "
                "< $in > $out",
        description="MACOS HOST $out",
    )
    n.build(outputs=host_syscall_numbers, rule="macos_host_syscall_numbers", inputs=arch / "bits" / "syscall.h.in")

    # the OpenGL ES entry points, which widen the guest's pointers to the
    # window's host addresses
    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name="macos_gl_stubs",
        command=(f"{python} tools/android_gl_stubs.py --window {LINUX_DIR}/src/gl.h {gl_include}/GLES3/gl32.h "
                 f"{gl_include}/GLES2/gl2ext.h {guest_gl_c} {gl_imports}"),
        description="MACOS GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule="macos_gl_stubs",
            implicit=[Path("tools/android_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="macos_posix_stubs",
        command=(f"{python} tools/android_posix_stubs.py --window {LINUX_DIR}/src/posix.h {guest_posix_c} "
                 f"{posix_imports}"),
        description="MACOS POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="macos_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    # the host imports: Android's, and the desktop's (SDL's windows, displays
    # and dialogs), whose pointer arguments the stubs widen to host addresses
    imports_s = gen_dir / "imports.s"
    host_table_c = host_dir / "host_import_table.c"
    import_lists = [ANDROID_DIR / "host_imports.list", PORT_DIR / "host_imports.list", posix_imports, gl_imports]
    import_headers = [ANDROID_DIR / "guest" / "runtime" / "guest_host.h", PORT_DIR / "guest" / "guest_host_macos.h"]
    n.rule(
        name="macos_imports",
        command=(f"{python} tools/macos_imports.py --host-table {host_table_c} "
                 + " ".join(f"--header {header}" for header in import_headers)
                 + f" {imports_s} $in"),
        description="MACOS IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="macos_imports", inputs=import_lists,
            implicit=[Path("tools/macos_imports.py"), *import_headers])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header, platform_semantics_header]

    # ---------- guest compilation: C -> Darwin assembly -> ELF assembly
    # rewritten for the window -> object

    n.rule(
        name="macos_guest_cc",
        command=(f"{compile_launcher(sln)}$macos_guest_cc -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 f"{python} tools/macos_sfi.py --darwin $out.darwin.s $out.s && "
                 f"$macos_guest_cc --target=aarch64-linux-gnu -c $out.s -o $out"),
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="macos_guest_as",
        command="$macos_guest_cc --target=aarch64-linux-gnu -c $in -o $out",
        description="MACOS AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    guest_abi = " ".join(GUEST_ABI_FLAGS_MACOS + configuration_defines(sln))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [Path("tools/android_asm_convert.py"), Path("tools/macos_sfi.py"), *generated_headers]
    profile = pgo_profile(sln, LINUX_PROFILE if pgo_mode(sln) == "train" else None, [LINUX_PROFILE], guest_cc)
    profile_flags = " ".join(profile_use_flags(profile))
    if profile:
        tool_implicit.append(profile)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="macos_guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{ANDROID_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="macos_ar",
        command="rm -f $out && $macos_ar rcs $out @$out.rsp",
        description="MACOS AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="macos_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() == "source/main/main.c":
            cflags += " " + updater_defines(getattr(sln, "port_release", False))
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{ANDROID_DIR}/guest/runtime", f"-I{PORT_DIR}/guest",
        f"-I{ANDROID_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
        f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}  # replaced by guest_memory_watch.c
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    for source in hud_assets_build(n, "macos", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    for source in opus_sources():
        objects.append(guest_object(source, " ".join([opus_cflags(guest_abi), *libc_includes])))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    for name in ZLIB_SOURCES:
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES,
                                                               "-U__ARM_FEATURE_CRC32"])))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", profile_flags, *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{ANDROID_DIR}/guest/runtime", f"-I{ANDROID_DIR}/include",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{ANDROID_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{ANDROID_DIR}/guest/runtime", f"-I{PORT_DIR}/guest", f"-I{ANDROID_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes,
    ])
    runtime_dir = ANDROID_DIR / "guest" / "runtime"
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    for source in sorted((PORT_DIR / "guest").glob("*.c")):
        objects.append(guest_object(source, runtime_cflags + f" -I{LINUX_DIR}/include -include {prefix_header}"
                                    if source.name.startswith("guest_platform") else runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="macos_guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = PORT_DIR / "guest" / "guest.ld"
    n.rule(
        name="macos_guest_link",
        command=(f"$macos_lld -m aarch64linux -static -nostdlib --error-limit=0 -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="MACOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="macos_guest_link", inputs=objects, implicit=[libguestc, linker_script])

    # ---------- SDL3

    n.rule(
        name="macos_sdl3",
        command=(f"cmake -S {SDL_DIR} -B {sdl_build} -G Ninja -DCMAKE_BUILD_TYPE=Release "
                 f"-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET={MINIMUM_MACOS} "
                 f"-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 f"> {BUILD}/sdl3-configure.log && ninja -C {sdl_build} > {BUILD}/sdl3-build.log"),
        description="MACOS SDL3",
        pool="console",
    )
    n.build(outputs=libsdl, rule="macos_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])

    # ---------- the host

    host_objects: List[Path] = []
    host_obj_dir = host_dir / "obj"
    n.rule(
        name="macos_host_cc",
        command="clang -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_target = f"-arch arm64 -mmacosx-version-min={MINIMUM_MACOS}"
    host_cflags = " ".join([
        host_target, "-O2", "-g", "-Wall", "-Wno-unused-function", "-D_DARWIN_C_SOURCE",
        f"-I{PORT_DIR}/host", f"-I{ANDROID_DIR}/include", f"-I{ANDROID_DIR}/host", f"-I{host_dir}",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", f"-I{LINUX_DIR}/src", f"-I{TOML_DIR}",
    ])
    host_sources = sorted((PORT_DIR / "host").glob("*.c")) + sorted((PORT_DIR / "host").glob("*.S")) + \
        sorted((PORT_DIR / "host").glob("*.m")) + [
        ANDROID_DIR / "host" / "host_watch_hash.c",
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
        TOML_DIR / "tomlc17.c",
    ]
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": host_cflags + (" -fobjc-arc" if source.suffix == ".m" else "")},
                implicit=[host_syscall_numbers])
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    mbedtls_cflags = " ".join([host_cflags, f"-I{MBEDTLS_DIR / 'include'}", f"-I{MBEDTLS_DIR / 'library'}"])
    for source in [LINUX_DIR / "src" / "posix_dtls.c", *sorted((MBEDTLS_DIR / "library").glob("*.c"))]:
        obj = host_obj_dir / ("mbedtls_" + source.name + ".o" if source.parent.parent == MBEDTLS_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": mbedtls_cflags + (" -w" if source.name != "posix_dtls.c" else "")})
        host_objects.append(obj)
    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="macos_host_cc", inputs=host_table_c, variables={"cflags": host_cflags})
    host_objects.append(table_obj)
    n.rule(
        name="macos_host_link",
        command=(f"clang {host_target} -o $out $in -L{sdl_build} -lSDL3 "
                 + " ".join(f"-framework {framework}" for framework in HOST_FRAMEWORKS)
                 + " -Wl,-rpath,@executable_path/../Frameworks"),
        description="MACOS HOST LINK $out",
    )
    n.build(outputs=host_executable, rule="macos_host_link", inputs=host_objects, implicit=[libsdl])

    # ---------- the app

    app = BUILD / "Halo.app"
    contents = app / "Contents"
    icon = BUILD / "Halo.icns"
    n.rule(
        name="macos_icon",
        command=f"{python} tools/macos_app.py icon $in $out",
        description="MACOS ICON $out",
    )
    n.build(outputs=icon, rule="macos_icon", inputs=Path("port/assets/icon/opence-icon.png"),
            implicit=[Path("tools/macos_app.py")])
    # signed for this machine alone, or with --macos-sign's Developer ID
    # (port/macos/README.md, "Sign and notarize")
    signing = getattr(sln, "macos_sign", None) or ""
    sign_option = f' --sign "{signing}"' if signing else ""
    n.rule(
        name="macos_bundle",
        command=(f"{python} tools/macos_app.py bundle --app {app} --executable {host_executable} "
                 f"--image {image} --sdl {libsdl} --angle {angle} --musl {MUSL_DIR} --icon {icon} "
                 f"--info {PORT_DIR / 'Info.plist'} --entitlements {PORT_DIR / 'Halo.entitlements'} "
                 f"--brokers port/assets/network/brokers.txt --minimum {MINIMUM_MACOS}{sign_option} --stamp $out"),
        description=f"MACOS APP {app}",
    )
    stamp = BUILD / "Halo.app.stamp"
    n.build(outputs=stamp, rule="macos_bundle",
            inputs=[host_executable, image, libsdl, icon, PORT_DIR / "Info.plist", PORT_DIR / "Halo.entitlements",
                    Path("port/assets/network/brokers.txt")],
            implicit=[Path("tools/macos_app.py"), Path("port/web/licenses.py")])
    n.build(outputs="macos", rule="phony", inputs=[stamp])
    dmg = BUILD / "Halo.dmg"
    n.rule(
        name="macos_dmg",
        command=f"{python} tools/macos_app.py dmg --app {app} --output $out{sign_option}",
        description="MACOS DMG $out",
    )
    n.build(outputs=dmg, rule="macos_dmg", inputs=[stamp], implicit=[Path("tools/macos_app.py")])
    n.build(outputs="macos_dmg", rule="phony", inputs=[dmg])
    n.newline()
