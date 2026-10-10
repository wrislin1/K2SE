"""Prove the routine table, the NWScript header and the compiler all agree.

    python tools/routine_id_test.py

Three artifacts have to stay in lockstep, and nothing enforces it at build time:

    src/routines.cpp        kExtended[]  -- id, name, argc as K2SE dispatches them
    nss/nwscript_k2se.nss   prototypes   -- nwnnsscomp assigns ids BY POSITION
    the emitted .ncs        ACTION       -- what the interpreter will actually see

This is the failure mode DESIGN.md 3.4 calls out in bold: get the header wrong and
the compiler silently resolves a name against the vanilla table instead. There is
no error, no checksum in the .ncs, and the mistake only appears at runtime -- as
the wrong engine routine running with your arguments on the stack.

So: compile a real call to every extended routine and check the ACTION bytes.

ACTION encoding, verified from the interpreter at 0x00700212:
    byte 0   opcode 0x05
    byte 1   type   0x00
    byte 2-3 routine id, BIG ENDIAN
    byte 4   argument count
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
COMPILER = os.path.join(HERE, "nwnnsscomp", "nwnnsscomp.exe")
HEADER = os.path.join(ROOT, "nss", "nwscript_k2se.nss")
ROUTINES_CPP = os.path.join(ROOT, "src", "routines.cpp")

PROTO_RE = re.compile(r"^\s*(int|float|string|object|void|vector|effect|location|talent"
                      r"|event|action)\s+(\w+)\s*\(([^;{]*)\)\s*;", re.M)

# {880, "K2SE_EchoString", 1, &H_EchoString},
TABLE_RE = re.compile(r"\{\s*(\d+)\s*,\s*\"(\w+)\"\s*,\s*(\d+)\s*,\s*&\w+\s*\}")

# A literal of each type, for synthesising a call.
SAMPLE = {
    "int": "1",
    "float": "1.0",
    "string": '"x"',
    "object": "OBJECT_SELF",
    "vector": "Vector(1.0, 2.0, 3.0)",
}


def parse_prototypes(text):
    out = []
    for m in PROTO_RE.finditer(text):
        rettype, name, args = m.group(1), m.group(2), m.group(3)
        if rettype in ("return", "else", "if", "while", "for"):
            continue
        out.append((name, rettype, args.strip()))
    return out


def arg_types(argstr):
    """Declared parameter types, in order. '' for a no-arg routine."""
    if not argstr.strip():
        return []
    types = []
    for part in argstr.split(","):
        part = part.strip()
        if not part:
            continue
        types.append(part.split()[0])
    return types


def find_actions(ncs):
    """Every position that could be an ACTION, as (offset, id, argc).

    This is a byte scan, not a decoder, so it reports false positives: a string
    literal compiles to CONSTS `04 05 00 <len> <chars>`, whose `05 00` looks
    exactly like an ACTION opcode. Candidates therefore OVERLAP -- the scan
    advances one byte at a time rather than skipping five, because a naive
    step-by-five lands inside the string data and walks straight past the real
    instruction that follows it.

    Callers must ask "is the expected ACTION among these", never "is the first
    candidate the expected one". The authoritative check on the id itself is the
    routine's position in nwscript_k2se.nss; this only confirms the compiler
    agreed.
    """
    out = []
    for i in range(13, len(ncs) - 4):  # skip "NCS V1.0" + size header
        if ncs[i] == 0x05 and ncs[i + 1] == 0x00:
            out.append((i, struct.unpack_from(">H", ncs, i + 2)[0], ncs[i + 4]))
    return out


def compile_script(src_text, workdir, name):
    shutil.copy(HEADER, os.path.join(workdir, "nwscript.nss"))
    shutil.copy(COMPILER, os.path.join(workdir, "nwnnsscomp.exe"))
    nss = os.path.join(workdir, name + ".nss")
    ncs = os.path.join(workdir, name + ".ncs")
    with open(nss, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(src_text)
    proc = subprocess.run([os.path.join(workdir, "nwnnsscomp.exe"), "-c", nss, "-o", ncs],
                          capture_output=True, cwd=workdir, timeout=120)
    output = (proc.stdout + proc.stderr).decode("utf-8", "replace").strip()
    if os.path.exists(ncs):
        with open(ncs, "rb") as fh:
            return True, fh.read(), output
    return False, b"", output


def main():
    if not os.path.exists(COMPILER):
        raise SystemExit("nwnnsscomp.exe not found at %s\n"
                         "  run: python tools/fetch_compiler.py" % COMPILER)

    table = TABLE_RE.findall(open(ROUTINES_CPP, encoding="utf-8").read())
    if not table:
        raise SystemExit("could not parse kExtended[] out of src/routines.cpp")
    table = [(int(i), n, int(a)) for i, n, a in table]

    protos = parse_prototypes(open(HEADER, encoding="utf-8", errors="replace").read())
    by_name = {n: (idx, rt, ar) for idx, (n, rt, ar) in enumerate(protos)}

    print("routines.cpp kExtended[] : %d entries" % len(table))
    print("nwscript_k2se.nss        : %d prototypes (last id would be %d)"
          % (len(protos), len(protos) - 1))
    print("")

    failures = 0
    workdir = tempfile.mkdtemp(prefix="k2se_ids_")

    for want_id, name, want_argc in table:
        if name not in by_name:
            print("FAIL  %-20s not declared in nwscript_k2se.nss" % name)
            failures += 1
            continue

        pos, rettype, argstr = by_name[name]
        types = arg_types(argstr)

        if pos != want_id:
            print("FAIL  %-20s header position %d, but routines.cpp says id %d"
                  % (name, pos, want_id))
            failures += 1
            continue
        if len(types) != want_argc:
            print("FAIL  %-20s header declares %d args, routines.cpp says argc=%d"
                  % (name, len(types), want_argc))
            failures += 1
            continue

        unknown = [t for t in types if t not in SAMPLE]
        if unknown:
            print("SKIP  %-20s cannot synthesise a call for type(s) %s"
                  % (name, ", ".join(unknown)))
            continue

        call = "%s(%s)" % (name, ", ".join(SAMPLE[t] for t in types))
        body = ("    %s;\n" % call) if rettype == "void" else ("    %s r = %s;\n"
                                                               % (rettype, call))
        ok, ncs, out = compile_script("void main() {\n%s}\n" % body, workdir, name)
        if not ok:
            print("FAIL  %-20s did not compile: %s" % (name, out.replace("\n", " | ")[:160]))
            failures += 1
            continue

        candidates = find_actions(ncs)
        exact = [a for a in candidates if a[1] == want_id and a[2] == want_argc]
        if not exact:
            near = [a for a in candidates if a[1] == want_id]
            if near:
                print("FAIL  %-20s id %d emitted but argc %d, expected %d"
                      % (name, want_id, near[0][2], want_argc))
            else:
                print("FAIL  %-20s no ACTION with id %d; candidates %s"
                      % (name, want_id,
                         ["id=%d argc=%d" % (a[1], a[2]) for a in candidates]))
            failures += 1
            continue

        _off, rid, argc = exact[0]
        raw = "05 00 %02X %02X %02X" % (rid >> 8, rid & 0xFF, argc)
        print("OK    %-20s id %-4d argc %d   ACTION %s" % (name, rid, argc, raw))

    # The other direction: every prototype appended past the vanilla table must
    # have a routines.cpp row, or the compiler happily emits an ACTION the DLL
    # answers with -2002 and the script dies silently in game.
    FIRST_EXTENDED = 877
    in_table = {name for _id, name, _argc in table}
    for pos, (name, _rt, _args) in enumerate(protos):
        if pos < FIRST_EXTENDED or name in in_table:
            continue
        print("FAIL  %-20s declared at header position %d but has no routines.cpp row"
              % (name, pos))
        failures += 1

    print("")
    if failures:
        print("%d routine(s) FAILED -- routines.cpp, nwscript_k2se.nss and the "
              "compiler disagree." % failures)
        return 1
    print("ALL EXTENDED ROUTINES AGREE -- table, header and emitted bytecode match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
