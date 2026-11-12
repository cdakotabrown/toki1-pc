#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build/run wholly authored BIOS CPU probes. No game/ROM bytes are embedded.

Use a fresh ignored output directory. CD boot matters: launching the same
EXE directly changes printf timing. BIOS selection belongs to regtest; inspect
its log before attributing results to a particular firmware.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
from safe_process import lines, run

BASE = 0x80010000
RESULTS = 0x80018000


def cases():
    rows = []
    for n in (0, 1, 2, 4, 8, 16, 32):
        rows += [("literal-" + str(n), "A" * n, []),
                 ("line-" + str(n), "A" * n + "\n", [])]
    for fmt in ("%x", "%08x", "%d", "%u", "%04d"):
        for value in (0, 1, 0x1234, 0x80000000, 0xffffffff):
            rows.append((fmt + "-" + hex(value), fmt, [value]))
    for n in (0, 1, 4, 16):
        rows.append(("string-" + str(n), "%s", ["A" * n]))
    rows += [("character", "%c", [65]), ("newline-character", "%c", [10]),
             ("percent", "%%", []),
             ("two-hex", "tag=%08x,%08x\n", [0x12345678, 0x89abcdef])]
    result = []
    for name, fmt, args in rows:
        for repeat in range(3):
            result.append(dict(kind="printf", name=name, format=fmt,
                               args=args, repeat=repeat))
    for n in (0, 1, 2, 3, 4, 8, 32, 128, 256, 1024, 6796):
        for repeat in range(3):
            result.append(dict(kind="zero", name="bzero-" + str(n),
                               n=n, repeat=repeat))
    return result


def assembly(rows):
    code = [".set noreorder", ".text", ".globl _start", "_start:",
            "li $sp,0x801fff00", "addiu $sp,$sp,-32",
            "li $t0,0x80018000", "sw $zero,0($t0)"]
    data = []
    for i, row in enumerate(rows):
        row["return_label"] = f"returned_{i}"
        if row["kind"] == "zero":
            code += ["li $a0,0x80020000", "li $a1," + str(row["n"])]
        else:
            code.append(f"la $a0,format_{i}")
            data += [".align 2", f"format_{i}:",
                     ".asciz " + json.dumps(row["format"])]
            for arg in range(3):
                value = row["args"][arg] if arg < len(row["args"]) else 0
                if isinstance(value, str):
                    code.append(f"la $a{arg+1},string_{i}_{arg}")
                    data += [".align 2", f"string_{i}_{arg}:",
                             ".asciz " + json.dumps(value)]
                else:
                    code.append(f"li $a{arg+1},0x{value:08x}")
        code += ["jal " + row["kind"] + "_stub", "nop", row["return_label"] + ":",
                 "li $t0,0x80018000", f"sw $v0,{4+i*4}($t0)"]
    code += ["li $t0,0x80018000", "li $t1,1", "sw $t1,0($t0)",
             "finished:", "nop", "halt:", "b halt", "nop"]
    for name, number in (("printf", 0x3f), ("zero", 0x28)):
        code += [name + "_stub:", "li $t2,0xa0", "jr $t2",
                 f"li $t1,0x{number:x}"]
    return "\n".join(code + data) + "\n"


def both(n, width):
    return n.to_bytes(width, "little") + n.to_bytes(width, "big")


def record(name, lba, size, flags=0):
    b = bytearray(33 + len(name) + (len(name) % 2 == 0))
    b[0] = len(b)
    b[2:10], b[10:18] = both(lba, 4), both(size, 4)
    b[18:25] = bytes([126, 10, 27, 0, 0, 0, 0])
    b[25], b[28:32], b[32] = flags, both(1, 2), len(name)
    b[33:33+len(name)] = name
    return b


def iso_image(exe, events=50, threads=8):
    if not 1 <= events <= 256 or not 1 <= threads <= 32:
        raise ValueError("unsupported kernel table capacity")
    if not exe.startswith(b"PS-X EXE") or len(exe) > 0x8800:
        raise ValueError("unexpected authored executable")
    config = ("BOOT = cdrom:PROBE.EXE;1\r\n"
              f"TCB = {threads:x}\r\nEVENT = {events:x}\r\n"
              "STACK = 801fff00\r\n").encode("ascii")
    image = bytearray(1024 * 2048)
    root = record(b"\0", 20, 2048, 2)
    vd = bytearray(2048)
    vd[:7] = b"\x01CD001\x01"
    vd[8:40] = b"PLAYSTATION".ljust(32)
    vd[40:72] = b"AUTHORED_BIOS_PROBE".ljust(32)
    vd[80:88] = both(1024, 4)
    vd[120:124] = vd[124:128] = both(1, 2)
    vd[128:132], vd[132:140] = both(2048, 2), both(10, 4)
    struct.pack_into("<I", vd, 140, 18)
    struct.pack_into(">I", vd, 148, 19)
    vd[156:190], vd[881] = root, 1
    image[16*2048:17*2048] = vd
    image[17*2048:17*2048+7] = b"\xffCD001\x01"
    for sector, endian in ((18, "<"), (19, ">")):
        image[sector*2048:sector*2048+10] = (
            bytes([1, 0]) + struct.pack(endian+"I", 20) +
            struct.pack(endian+"H", 1) + bytes(2))
    directory = (root + record(b"\1", 20, 2048, 2) +
                 record(b"SYSTEM.CNF;1", 21, len(config)) +
                 record(b"PROBE.EXE;1", 22, len(exe)))
    for sector, payload in ((20, directory), (21, config), (22, exe)):
        image[sector*2048:sector*2048+len(payload)] = payload
    return bytes(image)


def build(out, events=50, threads=8):
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    rows = cases()
    (out / "probe.s").write_text(assembly(rows))
    (out / "probe.ld").write_text(
        "ENTRY(_start)\nSECTIONS { . = 0x80010000; .text : { *(.text*) "
        "*(.rodata*) *(.data*) } /DISCARD/ : { *(.reginfo) "
        "*(.MIPS.abiflags) *(.pdr) *(.comment) *(.gnu.attributes) } }\n")
    for cmd in (["as", "-mips1", "-mabi=32", "-o", "probe.o", "probe.s"],
                ["ld", "-T", "probe.ld", "-o", "probe.elf", "probe.o"],
                ["objcopy", "-O", "binary", "probe.elf", "probe.bin"]):
        run(["mipsel-linux-gnu-" + cmd[0], *cmd[1:]],
            cwd=out, timeout=30).require_ok()
    symbols = {}
    for line in run(["mipsel-linux-gnu-nm", "-n", "probe.elf"],
                    cwd=out).require_ok().stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    body = (out / "probe.bin").read_bytes()
    body += bytes(-len(body) % 2048)
    if len(body) > RESULTS - BASE:
        raise ValueError("authored code overlaps result buffer")
    header = bytearray(2048)
    header[:8] = b"PS-X EXE"
    for off, value in ((0x10, BASE), (0x18, BASE), (0x1c, len(body)),
                       (0x30, 0x801fff00)):
        struct.pack_into("<I", header, off, value)
    exe = header + body
    (out / "probe.exe").write_bytes(exe)
    image = iso_image(exe, events, threads)
    (out / "probe.iso").write_bytes(image)
    manifest = dict(schema=1, authored=True, events=events, threads=threads,
                    cases=rows, symbols=symbols,
                    iso_sha256=hashlib.sha256(image).hexdigest())
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    return manifest


def measure(out, manifest, reference, library):
    symbols, rows = manifest["symbols"], manifest["cases"]
    trace = out / "probe.trace"
    command = [str(reference.resolve()), "-frames", "650", "-console",
               "-cpu", "Interpreter", "-trace", str(trace), "-tracereg",
               "80018000", str(4 + 4*len(rows)), "returns"]
    for label in ("printf_stub", "zero_stub", "finished"):
        command += ["-tracepc", f"{symbols[label]:08x}", label]
    for row in rows:
        label = row["return_label"]
        command += ["-tracepc", f"{symbols[label]:08x}", label]
    command += ["--", str(out / "probe.iso")]
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.resolve()))
    run(command, cwd="/tmp", env=env, timeout=120,
        log_path=out/"reference.log",
        watch_files=[(trace, 8*1024*1024, "TRACE_LIMIT")]).require_ok()
    started, durations, payload = None, {}, None
    for line in lines(trace, max_bytes=8*1024*1024):
        m = re.match(r"pc=(\w+) tick=(\d+)", line)
        if m:
            label, tick = m[1], int(m[2])
            if label in ("printf_stub", "zero_stub"):
                if started is not None:
                    raise ValueError("overlapping calls in reference trace")
                started = tick
            elif label.startswith("returned_"):
                if started is None or label in durations:
                    raise ValueError("missing or duplicate probe entry")
                durations[label] = tick - started
                started = None
        m = re.search(r"returns=([0-9a-fA-F]+)", line)
        if m:
            data = bytes.fromhex(m[1])
            if len(data) == 4 + len(rows)*4 and data[:4] == b"\1\0\0\0":
                payload = data
    if len(durations) != len(rows) or payload is None:
        raise ValueError("reference did not finish every authored probe")
    samples = [dict(row, ticks=durations[row["return_label"]],
                    returned=struct.unpack_from("<I", payload, 4+i*4)[0])
               for i, row in enumerate(rows)]
    report = dict(schema=1, command=command, samples=samples,
                  trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest(),
                  firmware="inspect reference.log; no ROM is embedded")
    (out/"measurements.json").write_text(json.dumps(report, indent=2)+"\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--events", type=int, default=50)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--reference-lib", type=Path)
    args = parser.parse_args()
    if bool(args.reference) != bool(args.reference_lib):
        parser.error("--reference and --reference-lib must be supplied together")
    manifest = build(args.out, args.events, args.threads)
    if args.reference:
        measure(args.out.resolve(), manifest, args.reference, args.reference_lib)
    print("Authored probe:", len(manifest["cases"]), "calls;", args.out)


if __name__ == "__main__":
    main()
