"""Tests of the macOS build's code generators (port/macos/README.md), which
run anywhere: the rewrite of the guest's assembly for its window
(tools/macos_sfi.py), the import stubs' pointer arguments
(tools/macos_imports.py), and the OpenCE GL and posix_* wrappers' --window
mode (tools/android_gl_stubs.py, tools/android_posix_stubs.py)."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import macos_imports  # noqa: E402
import macos_sfi  # noqa: E402


def rewrite(*lines):
    """the rewritten instructions of lines (in a text section), without
    tabs"""
    out = macos_sfi.Rewriter().run(["\t.text", *(f"\t{line}" for line in lines)])
    return [" ".join(line.split()) for line in out.splitlines() if line.strip() and line.strip() != ".text"]


@pytest.mark.parametrize("line,expected", [
    # a register base: the window's base and the low 32 bits
    ("ldr w9, [x0]", ["ldr w9, [x28, w0, uxtw]"]),
    ("str q0, [x10]", ["str q0, [x28, w10, uxtw]"]),
    ("ldrsh w10, [x0, #4]", ["add x27, x28, w0, uxtw", "ldrsh w10, [x27, #4]"]),
    ("ldr q1, [x8, :lo12:.LCPI0_0]", ["add x27, x28, w8, uxtw", "ldr q1, [x27, :lo12:.LCPI0_0]"]),
    ("ldur w8, [x9, #-4]", ["add x27, x28, w9, uxtw", "ldur w8, [x27, #-4]"]),
    # a register offset: the guest address first
    ("ldr w1, [x9, w8, uxtw #2]", ["add x27, x9, w8, uxtw #2", "ldr w1, [x28, w27, uxtw]"]),
    ("ldrb w0, [x8, x9]", ["add x27, x8, x9", "ldrb w0, [x28, w27, uxtw]"]),
    ("ldrsw x12, [x10, x9, lsl #2]", ["add x27, x10, x9, lsl #2", "ldrsw x12, [x28, w27, uxtw]"]),
    ("strh wzr, [x8, wzr, sxtw #1]", ["add x27, x8, wzr, sxtw #1", "strh wzr, [x28, w27, uxtw]"]),
    # pairs, exclusives and vectors have no register offset
    ("ldp q0, q1, [x1]", ["add x27, x28, w1, uxtw", "ldp q0, q1, [x27]"]),
    ("stp w8, wzr, [x2, #8]", ["add x27, x28, w2, uxtw", "stp w8, wzr, [x27, #8]"]),
    ("ldaxr w8, [x0]", ["add x27, x28, w0, uxtw", "ldaxr w8, [x27]"]),
    ("stlxr w10, w9, [x0]", ["add x27, x28, w0, uxtw", "stlxr w10, w9, [x27]"]),
    ("ld1 { v0.s }[1], [x8]", ["add x27, x28, w8, uxtw", "ld1 { v0.s }[1], [x27]"]),
    # writeback: the base register moves as before
    ("ldr w0, [x1, #4]!", ["add x1, x1, #4", "ldr w0, [x28, w1, uxtw]"]),
    ("ldr x0, [x1], #-8", ["ldr x0, [x28, w1, uxtw]", "sub x1, x1, #8"]),
    ("ld1 { v0.16b }, [x0], x2", ["add x27, x28, w0, uxtw", "ld1 { v0.16b }, [x27]", "add x0, x0, x2"]),
    ("stp x19, x20, [x9, #-16]!", ["sub x9, x9, #16", "add x27, x28, w9, uxtw", "stp x19, x20, [x27]"]),
    # the stack and the frame pointer hold host addresses
    ("stp x29, x30, [sp, #-16]!", ["stp x29, x30, [sp, #-16]!"]),
    ("ldur w8, [x29, #-4]", ["ldur w8, [x29, #-4]"]),
    ("ldr x8, [sp, x9, lsl #3]", ["ldr x8, [sp, x9, lsl #3]"]),
    # values made from host addresses keep their low 32 bits
    ("adrp x8, symbol", ["adrp x8, symbol", "mov w8, w8"]),
    ("adr x11, .Ltmp0", ["adr x11, .Ltmp0", "mov w11, w11"]),
    ("add x0, sp, #16", ["add x0, sp, #16", "mov w0, w0"]),
    ("sub x8, x29, #4", ["sub x8, x29, #4", "mov w8, w8"]),
    ("mov x0, x30", ["mov x0, x30", "mov w0, w0"]),
    ("add w8, w8, #1", ["add w8, w8, #1"]),
    ("add x29, sp, #16", ["add x29, sp, #16"]),
    ("mov x29, sp", ["mov x29, sp"]),
    # x30 the compiler uses as a register (it saved the return address)
    ("add x30, x5, x11", ["add x30, x5, x11"]),
    ("cmp w30, w8", ["cmp w30, w8"]),
    # indirect branches go into the window
    ("blr x8", ["add x27, x28, w8, uxtw", "blr x27"]),
    ("br x10", ["add x27, x28, w10, uxtw", "br x27"]),
    ("ret", ["ret"]),
    ("bl function", ["bl function"]),
    # sp from a general register gets the base again
    ("mov sp, x0", ["mov x27, x0", "add sp, x28, w27, uxtw"]),
    ("sub sp, x9, x8", ["sub x27, x9, x8", "add sp, x28, w27, uxtw"]),
    ("mov sp, x29", ["mov sp, x29"]),
    ("sub sp, sp, #16", ["sub sp, sp, #16"]),
    # the base read whole (guest_window.h)
    ("mov x8, x28", ["mov x8, x28"]),
])
def test_rewrite(line, expected):
    assert rewrite(line) == expected


@pytest.mark.parametrize("line", [
    "add x0, x27, #1",
    "ldr x0, [x28]",
    "cmp x8, sp",
    "cmp x29, x0",
    "casal w0, w1, [x2]",
    "ldadd w0, w1, [x2]",
    "blr x29",
    "mov x29, x8",
])
def test_rewrite_refuses(line):
    with pytest.raises(macos_sfi.SfiError):
        rewrite(line)


def test_data_sections_left_alone():
    out = macos_sfi.Rewriter().run(["\t.data", "label:", "\t.long symbol", "\t.text", "\tldr w0, [x1]"])
    assert "\t.long symbol" in out
    assert "[x28, w1, uxtw]" in out


def test_string_data_kept():
    # (Opus's cache_bits50: bytes 51, 95 ("3_") after an escaped quote's 34;
    # an underscore that starts a word was taken for a symbol's and dropped)
    from android_asm_convert import Converter

    data = r'"(\007\"$&3_\206\252_tail"'
    out = Converter(["\t.section\t__TEXT,__const", "_cache_bits50:", f"\t.ascii\t{data}",
                     '\t.asciz\t"_start \\"_quoted\\" l_local"']).run()
    assert data in out
    assert '"_start \\"_quoted\\" l_local"' in out
    assert "cache_bits50:" in out and "_cache_bits50:" not in out


def x30_rewrite(*functions):
    """the rewritten instructions of functions (name, lines), without tabs"""
    lines = ["\t.text"]
    for name, body in functions:
        lines += [f"{name}:"] + [f"\t{line}" for line in body]
    out = macos_sfi.Rewriter().run(lines)
    return [" ".join(line.split()) for line in out.splitlines() if line.strip()]


def test_x30_as_a_register_keeps_its_values():
    # (SHA-512's and the curves' 64-bit arithmetic: the compiler saved the
    # return address, and x30 holds data until it loads it back)
    out = x30_rewrite(("multiply", ["stp x29, x30, [sp, #-16]!", "mul x30, x1, x2", "add x8, x30, x9",
                                    "smaddl x2, w11, w15, x30", "ldp x29, x30, [sp], #16", "ret"]))
    assert "add x8, x30, x9" in out and "mov w8, w8" not in out
    assert "mov w2, w2" not in out
    # (a value spilled and loaded back into x30)
    out = x30_rewrite(("reload", ["str x30, [sp, #-16]!", "ldr x30, [sp, #24]", "eor x8, x30, x9",
                                  "ldr x30, [sp], #16", "ret"]))
    assert "mov w8, w8" not in out


def test_x30_return_address_keeps_its_low_bits():
    # (each function by itself: the second only reads the return address,
    # restoring it is no other use)
    out = x30_rewrite(("data_user", ["mov x30, x1", "add x8, x30, x9", "ret"]),
                      ("caller", ["stp x29, x30, [sp, #-16]!", "mov x0, x30", "bl callee",
                                  "ldp x29, x30, [sp], #16", "ret"]))
    caller = out[out.index("caller:"):]
    assert "mov w8, w8" not in out[:out.index("caller:")]
    assert caller[caller.index("mov x0, x30") + 1] == "mov w0, w0"


def test_far_test_bit_branches_relaxed():
    lines = ["\t.text", "start:", "\ttbz w8, #0, far"] + ["\tadd w0, w0, #1"] * 9000 + ["far:", "\tret"]
    out = macos_sfi.Rewriter().run(lines).splitlines()
    assert "\ttbnz\tw8, #0, .Lsfi_relax_1" in out
    assert "\tb\tfar" in out
    assert ".Lsfi_relax_1:" in out


def test_near_test_bit_branches_kept():
    lines = ["\t.text", "\ttbz w8, #0, near", "\tadd w0, w0, #1", "near:", "\tret"]
    out = macos_sfi.Rewriter().run(lines)
    assert "tbz\tw8, #0, near" in out


def test_rewritten_code_assembles(tmp_path):
    """the rewrite's output is valid AArch64 (with a clang that targets ELF)"""
    clang = shutil.which("clang")
    if not clang:
        pytest.skip("no clang")
    source = tmp_path / "in.s"
    source.write_text("\n".join([
        "\t.text", "f:", "\tldr w9, [x0, #8]", "\tldr w1, [x9, w8, uxtw #2]", "\tldp q0, q1, [x1], #32",
        "\tadrp x8, f", "\tadd x0, sp, #16", "\tblr x8", "\tmov sp, x0", "\tstrh wzr, [x8, wzr, sxtw #1]",
        "\tldaxr w8, [x0]", "\tret",
    ]) + "\n")
    output = tmp_path / "out.s"
    subprocess.run([sys.executable, str(ROOT / "tools" / "macos_sfi.py"), str(source), str(output)], check=True)
    result = subprocess.run([clang, "--target=aarch64-linux-gnu", "-c", str(output), "-o", str(tmp_path / "out.o")],
                            capture_output=True, text=True)
    if result.returncode and "unknown target" in result.stderr:
        pytest.skip("clang without the AArch64 target")
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("params,registers", [
    (["unsigned int window", "float x", "float y", "const char *text"], [1]),
    (["const char *title", "int width", "int height", "long long flags"], [0]),
    (["unsigned int display", "int width", "int height", "float refresh_rate", "int include_high_density",
      "void *mode"], [4]),
    (["long long number", "long long a"], []),
    (["const int *controls"], [0]),
])
def test_import_pointer_registers(params, registers):
    assert macos_imports.pointer_registers(params) == registers


def test_import_stubs_widen_pointers(tmp_path):
    header = tmp_path / "host.h"
    header.write_text("int host_open(unsigned int window, const char *name);\n"
                      "/* a comment */ void host_plain(int value) __attribute__((noreturn));\n")
    names = tmp_path / "imports.list"
    names.write_text("# the imports\nhost_open\nhost_plain\nhostgl_glClear\n")
    output = tmp_path / "imports.s"
    table = tmp_path / "table.c"
    subprocess.run([sys.executable, str(ROOT / "tools" / "macos_imports.py"), "--host-table", str(table),
                    "--header", str(header), str(output), str(names)], check=True)
    stubs = output.read_text()
    open_stub = stubs[stubs.index("host_open:"):stubs.index("host_plain:")]
    assert "cbz w1, .Lhost_open_1" in open_stub
    assert "add x1, x28, w1, uxtw" in open_stub
    plain_stub = stubs[stubs.index("host_plain:"):stubs.index("hostgl_glClear:")]
    assert "x28" not in plain_stub
    assert "hostgl_glClear" not in table.read_text()


GL_HEADER = """
#define GL_APICALL
#define GL_APIENTRY
GL_APICALL void GL_APIENTRY glVertexAttribPointer (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
GL_APICALL void GL_APIENTRY glDrawElements (GLenum mode, GLsizei count, GLenum type, const void *indices);
GL_APICALL void GL_APIENTRY glTexImage2D (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
GL_APICALL void GL_APIENTRY glVertexAttrib4fv (GLuint index, const GLfloat *v);
GL_APICALL void GL_APIENTRY glBufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
GL_APICALL void GL_APIENTRY glBlitFramebuffer (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
"""
GL_LIST = """/* ANDROID_GL_FUNCTIONS_BEGIN */
X(glVertexAttribPointer) X(glDrawElements) X(glTexImage2D) X(glVertexAttrib4fv) X(glBufferSubData)
X(glBlitFramebuffer)
/* ANDROID_GL_FUNCTIONS_END */
"""


def gl_wrappers(tmp_path, *options):
    (tmp_path / "gl.h").write_text(GL_LIST)
    (tmp_path / "gl32.h").write_text(GL_HEADER)
    (tmp_path / "gl2ext.h").write_text("")
    output = tmp_path / "guest_gl.c"
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_gl_stubs.py"), *options, str(tmp_path / "gl.h"),
                    str(tmp_path / "gl32.h"), str(tmp_path / "gl2ext.h"), str(output), str(tmp_path / "gl.list")],
                   check=True)
    return output.read_text()


def test_gl_wrappers_window(tmp_path):
    text = gl_wrappers(tmp_path, "--window")
    assert '#include "guest_window.h"' in text
    # (offsets into the bound buffers pass as they are)
    assert "hostgl_glVertexAttribPointer(a0, a1, a2, a3, a4, a5);" in text
    assert "hostgl_glDrawElements(a0, a1, a2, a3);" in text
    # (the guest's memory, widened)
    assert "guest_window_pointer(a8)" in text
    assert "hostgl_glVertexAttrib4fv(a0, guest_window_pointer(a1));" in text
    assert "hostgl_glBufferSubData(a0, (long long)a1, (long long)a2, guest_window_pointer(a3));" in text
    # (the arguments on the stack packed by size, as Apple's arm64 reads them:
    # in 8-byte slots, the filter would be read from the mask's padding)
    assert ("void hostgl_glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, "
            "GLenum);") in text
    assert "hostgl_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);" in text


def test_gl_wrappers_android_unchanged(tmp_path):
    text = gl_wrappers(tmp_path)
    assert "guest_window" not in text
    assert "hostgl_glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, (unsigned long long)(unsigned int)a8);" in text
    # (Android's host reads the arguments on the stack from 8-byte slots)
    assert "hostgl_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, (long long)a8, (long long)a9);" in text


def test_posix_wrappers_window(tmp_path):
    output = tmp_path / "guest_posix.c"
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_posix_stubs.py"), "--window",
                    str(ROOT / "port" / "linux" / "src" / "posix.h"), str(output), str(tmp_path / "posix.list")],
                   check=True)
    text = output.read_text()
    # (a directory stream's handle is a number, not an address)
    assert "hostposix_directory_next(directory, guest_window_pointer(name), name_size)" in text
    assert "hostposix_directory_close(directory)" in text
    assert ("hostposix_find_entry_case_insensitive(guest_window_pointer(directory), guest_window_pointer(name), "
            "guest_window_pointer(result), result_size)") in text
    assert "hostposix_stat(guest_window_pointer(path), guest_window_pointer(information))" in text
