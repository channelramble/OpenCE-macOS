#!/usr/bin/env python3
"""Rewrite the macOS guest's AArch64 assembly to address its own window.

The macOS port runs the game as the Android port does: ILP32 AArch64 code
(clang's arm64_32 target, converted to ELF assembly by
tools/android_asm_convert.py) inside an ordinary 64-bit process. On Android
the guest's 32-bit addresses are the process's own, below 4 GB. macOS gives
an arm64 process nothing below 4 GB (its __PAGEZERO must cover the first
4 GB), so the host reserves a 4 GB window at a 4 GB-aligned base instead and
the guest's code is rewritten here to address it: guest address A is host
address base + A. The base lives in x28 and x27 is this rewrite's scratch
register; the guest is compiled with -ffixed-x27 -ffixed-x28 so that the
compiler leaves both alone (port/macos/README.md).

The rules:

- A load, store, atomic or prefetch whose base register is a general
  register addresses x28 plus the low 32 bits of the guest address
  ([x28, wN, uxtw], or x27 = x28 + wN for the forms without a register
  offset). Pre- and post-indexed forms update the base register as before.
- sp and x29 hold host addresses (the stack is in the window), so accesses
  based on them are left alone. A value computed from sp or x29, or by adr
  or adrp, is a host address: it is truncated to its low 32 bits, the guest
  address (the base is 4 GB-aligned), since the compiler keeps pointers
  zero-extended in registers. So is one computed from x30 in a function that
  only reads it (the return address); a function that writes x30 otherwise
  than restoring it (ldp x29, x30 or ldr x30, [sp], #n) uses it as one more
  register, whose values are data, 64-bit integers among them, and kept
  whole. A 32-bit comparison sees only those bits; a 64-bit one with sp or
  x29 is refused.
- br and blr go to x28 plus the low 32 bits of the target. sp set from a
  general register gets the base added back.

Anything the rules do not cover (an instruction that uses x27 or x28, a
64-bit comparison with sp) stops the conversion with an error, so that new
code generation is looked at rather than silently mistranslated. The one
use of x28 allowed is reading it whole (mov xN, x28): the guest's calls into
the host widen pointers with it (port/macos/guest/guest_window.h).

The rewrite makes code longer; conditional branches whose reach (tbz/tbnz:
32 KB) the longer code could exceed are turned into an inverted branch over
an unconditional one.

Usage: macos_sfi.py [--darwin] input.s output.s

With --darwin, the input is clang's Darwin assembly, converted to ELF
assembly first (tools/android_asm_convert.py).
"""

import re
import sys

from android_asm_convert import ConvertError, Converter

BASE = "x28"
SCRATCH = "x27"
SCRATCH_W = "w27"

# loads and stores that take a register offset ([xN, wM, uxtw]); the others
# take only an immediate offset
REGISTER_OFFSET_OPS = {
    "ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh", "ldrsw", "prfm",
}
# every mnemonic with a memory operand this rewrite knows; an unknown one is
# an error
IMMEDIATE_ONLY_OPS = {
    "ldur", "stur", "ldurb", "sturb", "ldurh", "sturh", "ldursb", "ldursh", "ldursw", "prfum",
    "ldp", "stp", "ldpsw", "ldnp", "stnp",
    "ldxr", "ldxrb", "ldxrh", "stxr", "stxrb", "stxrh",
    "ldaxr", "ldaxrb", "ldaxrh", "stlxr", "stlxrb", "stlxrh",
    "ldar", "ldarb", "ldarh", "stlr", "stlrb", "stlrh",
    "ldapr", "ldaprb", "ldaprh",
    "ldxp", "stxp", "ldaxp", "stlxp",
    "ld1", "ld2", "ld3", "ld4", "st1", "st2", "st3", "st4",
    "ld1r", "ld2r", "ld3r", "ld4r",
}
# (LSE atomics, which ARMv8.0 code has none of, are refused below)

GPR64 = re.compile(r"^x([0-9]|[12][0-9]|30)$")
GPR32 = re.compile(r"^w([0-9]|[12][0-9]|30)$")
HOST_REGISTERS = {"sp", "wsp", "x29", "w29", "x30", "w30"}
# (a 64-bit comparison with these would compare a host address)
HOST_ADDRESS_REGISTERS = {"sp", "x29"}
ZERO_REGISTERS = {"wzr", "xzr"}
RESERVED = re.compile(r"\b[xw]2[78]\b")
# instructions whose first operand is not a register they write
NO_DESTINATION = {
    "cmp", "cmn", "tst", "ccmp", "ccmn", "cbz", "cbnz", "tbz", "tbnz", "b", "bl", "br", "blr", "ret",
    "fcmp", "fcmpe", "fccmp", "fccmpe", "msr", "hint", "dmb", "dsb", "isb", "clrex", "nop", "yield",
    "brk", "hlt", "udf", "svc", "sev", "sevl", "wfe", "wfi", "dc", "ic", "tlbi", "at",
}
BRANCH_LIMITS = {"tbz": 8191, "tbnz": 8191}
INVERTED = {"tbz": "tbnz", "tbnz": "tbz"}


class SfiError(Exception):
    pass


def split_operands(text: str):
    """the comma-separated operands, keeping [...] and {...} together"""
    parts, depth, current = [], 0, []
    for ch in text:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current or parts:
        parts.append("".join(current).strip())
    return parts


def w_of(register: str) -> str:
    return "w" + register[1:]


def immediate_value(text: str):
    """an immediate operand's value (#imm), or None for anything else"""
    m = re.match(r"^#?(-?(?:0x[0-9a-fA-F]+|\d+))$", text.strip())
    if not m:
        return None
    return int(m.group(1), 0)


def add_immediate(register: str, value: int):
    """register += value, with value small enough for one add or sub"""
    if value >= 0:
        return f"\tadd\t{register}, {register}, #{value}"
    return f"\tsub\t{register}, {register}, #{-value}"


def destinations(op: str, operands):
    """the registers an instruction writes: a load's before its memory
    operand, most others' first operand"""
    mem_index = next((i for i, o in enumerate(operands) if o.startswith("[")), None)
    if op.startswith("st") or op.startswith("prf"):
        return []
    if mem_index is not None:
        return operands[:mem_index]
    if op in NO_DESTINATION or op.startswith("b."):
        return []
    return operands[:1]


def writes_x30_as_register(op: str, operands) -> bool:
    """whether the instruction writes x30 otherwise than restoring the
    return address (ldp x29, x30, [...], or ldr x30, [sp], #n)"""
    written = destinations(op, operands)
    if not any(register in ("x30", "w30") for register in written):
        return False
    if op == "ldp" and written == ["x29", "x30"]:
        return False
    return not (op == "ldr" and written == ["x30"] and len(operands) == 3 and operands[1] == "[sp]")


class Rewriter:
    def __init__(self):
        self.out = []
        # whether the function being rewritten uses x30 as one more register
        # (its values are data, not the return address)
        self.x30_data = False

    def emit(self, line: str):
        self.out.append(line)

    # ---------- memory operands

    def memory(self, op: str, operands, index: int):
        """rewrites an instruction whose operands[index] is a memory operand"""
        mem = operands[index]
        pre_index = mem.endswith("!")
        inner = mem[1:mem.rindex("]")].strip()
        post = operands[index + 1:] if index + 1 < len(operands) else []
        regs = operands[:index]
        parts = [p.strip() for p in inner.split(",")]
        base = parts[0]
        rest = parts[1:]
        if base in ("sp", "x29"):
            return None  # host addresses: unchanged
        if not GPR64.match(base):
            raise SfiError(f"unexpected base register {base}")
        if op not in REGISTER_OFFSET_OPS and op not in IMMEDIATE_ONLY_OPS:
            raise SfiError(f"unknown memory instruction {op}")
        register_offset = op in REGISTER_OFFSET_OPS
        head = f"\t{op}\t" + (", ".join(regs) + ", " if regs else "")
        lines = []

        if pre_index:
            if len(rest) != 1:
                raise SfiError("unexpected pre-indexed operand " + mem)
            value = immediate_value(rest[0])
            if value is None:
                raise SfiError("unexpected pre-index " + mem)
            lines.append(add_immediate(base, value))
            rest = []
        if post and len(post) != 1:
            raise SfiError("unexpected post-index operands " + ", ".join(post))

        if not rest:
            if register_offset:
                lines.append(f"{head}[{BASE}, {w_of(base)}, uxtw]")
            else:
                lines.append(f"\tadd\t{SCRATCH}, {BASE}, {w_of(base)}, uxtw")
                lines.append(f"{head}[{SCRATCH}]")
        elif len(rest) == 1 and (rest[0].startswith("#") or rest[0].startswith(":")):
            # an immediate offset (or a :lo12: relocation)
            lines.append(f"\tadd\t{SCRATCH}, {BASE}, {w_of(base)}, uxtw")
            lines.append(f"{head}[{SCRATCH}, {rest[0]}]")
        else:
            # a register offset: the guest address first, then the window's
            if not register_offset:
                raise SfiError(f"register offset on {op}")
            index_register = rest[0]
            extend = ", ".join(rest[1:])
            if GPR64.match(index_register) or GPR32.match(index_register) or index_register in ZERO_REGISTERS:
                if extend:
                    lines.append(f"\tadd\t{SCRATCH}, {base}, {index_register}, {extend}")
                else:
                    if index_register.startswith("w"):
                        raise SfiError("w register offset without extend " + mem)
                    lines.append(f"\tadd\t{SCRATCH}, {base}, {index_register}")
            else:
                raise SfiError("unexpected offset " + mem)
            lines.append(f"{head}[{BASE}, {SCRATCH_W}, uxtw]")

        if post:
            value = immediate_value(post[0])
            if value is not None:
                lines.append(add_immediate(base, value))
            elif GPR64.match(post[0]):
                lines.append(f"\tadd\t{base}, {base}, {post[0]}")
            else:
                raise SfiError("unexpected post-index " + post[0])
        return lines

    # ---------- instructions

    def instruction(self, op: str, args: str):
        operands = split_operands(args) if args else []
        mem_index = next((i for i, o in enumerate(operands) if o.startswith("[")), None)

        if op == "mov" and len(operands) == 2 and operands[1] == BASE and GPR64.match(operands[0]) \
                and not RESERVED.search(operands[0]) and operands[0] not in ("x29", "x30"):
            # (the window's base, read whole: guest_window.h)
            self.emit(f"\t{op}\t{args}")
            return
        if RESERVED.search(args):
            raise SfiError(f"{op} {args}: uses x27 or x28, which the rewrite owns")

        if mem_index is not None:
            if op.startswith(("cas", "swp", "ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin",
                              "ldumax", "ldumin", "stadd", "stclr", "steor", "stset", "stsmax",
                              "stsmin", "stumax", "stumin")):
                raise SfiError(f"LSE atomic {op} (the guest is ARMv8.0 code)")
            if op in ("prfm", "prfum"):
                # a prefetch never faults, but it should prefetch the right
                # line: rewritten like a load
                pass
            lines = self.memory(op, operands, mem_index)
            if lines is None:
                self.emit(f"\t{op}\t{args}")
                # (a load from the stack into x29 or x30 restores a host
                # value saved there; into another register it is data)
            else:
                self.out.extend(lines)
            return

        if op in ("adr", "adrp"):
            destination = operands[0]
            self.emit(f"\t{op}\t{args}")
            if not GPR64.match(destination) or destination == "x29":
                raise SfiError(f"{op} into {destination}")
            self.emit(f"\tmov\t{w_of(destination)}, {w_of(destination)}")
            return

        if op in ("br", "blr"):
            target = operands[0]
            if not GPR64.match(target) or target in ("x29", "x30"):
                raise SfiError(f"{op} {target}")
            self.emit(f"\tadd\t{SCRATCH}, {BASE}, {w_of(target)}, uxtw")
            self.emit(f"\t{op}\t{SCRATCH}")
            return

        if op == "ret":
            if operands and operands[0] != "x30":
                raise SfiError(f"ret {operands[0]}")
            self.emit(f"\t{op}\t{args}" if args else f"\t{op}")
            return

        if op in ("b", "bl") or op.startswith("b."):
            self.emit(f"\t{op}\t{args}")
            return

        destination = operands[0] if operands and op not in NO_DESTINATION else None
        sources = operands[1:] if destination is not None else operands

        if destination in ("sp", "wsp"):
            if any(s in ("sp", "x29") for s in sources):
                # sp from sp or the frame pointer: host addresses already
                self.emit(f"\t{op}\t{args}")
                return
            # sp computed from general registers (a variable-length array's
            # allocation, an epilogue): computed in the scratch register, then
            # rebased
            self.emit(f"\t{op}\t{SCRATCH}, " + ", ".join(sources))
            self.emit(f"\tadd\tsp, {BASE}, {SCRATCH_W}, uxtw")
            return

        if destination in ("x29", "w29"):
            if all(s in ("sp", "x29") or immediate_value(s) is not None or s.startswith("lsl") for s in sources):
                self.emit(f"\t{op}\t{args}")
                return
            raise SfiError(f"{op} {args}: the frame pointer set from a general register")

        # (x30 written here is one more register: the compiler saved the
        # return address, and loads it back before it returns. Read in a
        # function that does so, it is data)
        host_registers = HOST_REGISTERS - {"x30", "w30"} if self.x30_data else HOST_REGISTERS
        reads_host = any(s.split()[0] in host_registers for s in sources if s)
        if destination is None:
            if op in ("cmp", "cmn", "tst", "ccmp", "ccmn") and \
                    any(s.split()[0] in HOST_ADDRESS_REGISTERS for s in sources if s):
                raise SfiError(f"{op} {args}: a 64-bit comparison with a host address")
            self.emit(f"\t{op}\t{args}")
            return

        self.emit(f"\t{op}\t{args}")
        if reads_host and GPR64.match(destination):
            self.emit(f"\tmov\t{w_of(destination)}, {w_of(destination)}")

    # ---------- branch reach

    def relax_branches(self):
        """turns tbz/tbnz whose target moved out of reach into an inverted
        branch over a b. Positions count every instruction line, and each
        alignment directive at its largest padding."""
        lines = self.out
        positions, labels, position = [], {}, 0
        for line in lines:
            positions.append(position)
            text = line.strip()
            if not text:
                continue
            if text.endswith(":") and not line.startswith("\t"):
                labels[text[:-1]] = position
                continue
            if text.startswith(".p2align"):
                m = re.match(r"\.p2align\s+(\d+)", text)
                if m:
                    position += (1 << int(m.group(1))) // 4
                continue
            if text.startswith(".section") or text in (".text", ".data", ".bss"):
                continue
            if text.startswith("."):
                continue
            position += 1
        result = []
        counter = 0
        for line, at in zip(lines, positions):
            m = re.match(r"^\t(tbz|tbnz)\t([wx]\d+|[wx]zr), (#\d+), ([\w.$]+)$", line)
            if m and m.group(4) in labels and abs(labels[m.group(4)] - at) > BRANCH_LIMITS[m.group(1)] // 2:
                counter += 1
                skip = f".Lsfi_relax_{counter}"
                result.append(f"\t{INVERTED[m.group(1)]}\t{m.group(2)}, {m.group(3)}, {skip}")
                result.append(f"\tb\t{m.group(4)}")
                result.append(f"{skip}:")
                continue
            result.append(line)
        self.out = result

    # ---------- the file

    @staticmethod
    def section_is_code(directive: str, current: bool) -> bool:
        """whether the section a section directive selects holds code"""
        words = directive.replace(",", " ").split()
        if words[0] == ".text":
            return True
        if words[0] in (".data", ".bss", ".rodata"):
            return False
        if words[0] in (".section", ".pushsection") and len(words) > 1:
            return words[1].startswith(".text")
        return current

    @staticmethod
    def functions_using_x30(lines):
        """for each line, whether the function it is in (from one global
        label in code to the next) writes x30 as one more register"""
        flags = [False] * len(lines)
        in_text = True
        stack = []
        start = 0
        using = False
        for number, raw in enumerate(lines):
            line = raw.rstrip()
            text = line.strip()
            if not text:
                continue
            if text.startswith("."):
                word = text.split()[0]
                if word == ".pushsection":
                    stack.append(in_text)
                    in_text = Rewriter.section_is_code(text, in_text)
                elif word == ".popsection":
                    in_text = stack.pop() if stack else True
                elif word in (".text", ".data", ".bss", ".rodata", ".section"):
                    in_text = Rewriter.section_is_code(text, in_text)
                continue
            if text.endswith(":") and not line.startswith("\t"):
                if in_text and not text.startswith(".L"):
                    flags[start:number] = [using] * (number - start)
                    start, using = number, False
                continue
            if not in_text:
                continue
            parts = text.split(None, 1)
            if writes_x30_as_register(parts[0].lower(), split_operands(parts[1]) if len(parts) > 1 else []):
                using = True
        flags[start:] = [using] * (len(lines) - start)
        return flags

    def run(self, lines):
        in_text = True
        stack = []
        x30_data = self.functions_using_x30(lines)
        for number, raw in enumerate(lines, 1):
            self.x30_data = x30_data[number - 1]
            line = raw.rstrip()
            text = line.strip()
            try:
                if not text:
                    continue
                if text.startswith("."):
                    word = text.split()[0]
                    if word == ".pushsection":
                        stack.append(in_text)
                        in_text = self.section_is_code(text, in_text)
                    elif word == ".popsection":
                        in_text = stack.pop() if stack else True
                    elif word in (".text", ".data", ".bss", ".rodata", ".section"):
                        in_text = self.section_is_code(text, in_text)
                    self.emit(line)
                    continue
                if text.endswith(":") and not line.startswith("\t"):
                    self.emit(line)
                    continue
                if not in_text:
                    self.emit(line)
                    continue
                parts = text.split(None, 1)
                op = parts[0].lower()
                args = parts[1].strip() if len(parts) > 1 else ""
                self.instruction(op, args)
            except SfiError as error:
                raise SfiError(f"line {number}: {text}: {error}") from None
        self.relax_branches()
        return "\n".join(self.out) + "\n"


def main():
    arguments = sys.argv[1:]
    darwin = bool(arguments) and arguments[0] == "--darwin"
    if darwin:
        arguments = arguments[1:]
    if len(arguments) != 2:
        print(__doc__)
        sys.exit(2)
    source, output = arguments
    with open(source, "r", encoding="utf-8", errors="surrogateescape") as f:
        lines = f.read().split("\n")
    try:
        if darwin:
            lines = Converter(lines).run().split("\n")
        result = Rewriter().run(lines)
    except (SfiError, ConvertError) as error:
        print(f"{source}: {error}", file=sys.stderr)
        sys.exit(1)
    with open(output, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(result)


if __name__ == "__main__":
    main()
