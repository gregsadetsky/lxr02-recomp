"""elf frontend: functions, reachable instructions, jump tables.

reads a cortex-m7 elf built by arm-none-eabi-gcc, walks every function from its
entry (recursive descent, so literal pools inside .text are never decoded as code),
and returns, per function, the instructions in address order plus the set of branch
targets. nothing here interprets semantics; that is the backend's job.
"""
import re
from dataclasses import dataclass, field
import capstone
from capstone import arm as A
from elftools.elf.elffile import ELFFile

MODE = capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS | capstone.CS_MODE_V8


@dataclass
class Func:
    name: str
    start: int
    size: int
    insns: dict = field(default_factory=dict)      # addr -> capstone insn
    labels: set = field(default_factory=set)       # addrs that are jump targets
    tables: dict = field(default_factory=dict)     # tbb/tbh addr -> [targets]
    problems: list = field(default_factory=list)


class Image:
    def __init__(self, path):
        f = open(path, "rb")
        self.elf = ELFFile(f)
        self.mem = {}
        self.segs = []
        for seg in self.elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                self.segs.append((seg["p_vaddr"], seg.data(), seg["p_memsz"]))
        self.entry = self.elf["e_entry"]
        self.funcs = {}
        symtab = self.elf.get_section_by_name(".symtab")
        self.zero_size = []
        for s in symtab.iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC":
                a = s["st_value"] & ~1
                if s["st_size"] > 0:
                    if a not in self.funcs:
                        self.funcs[a] = Func(s.name, a, s["st_size"])
                else:
                    self.zero_size.append((a, s.name))
        self.syms = {}
        self.name_at = {}
        self.sym_addrs = set()
        for s in symtab.iter_symbols():
            if s.name:
                self.syms.setdefault(s.name, s["st_value"])
                if s["st_info"]["type"] in ("STT_FUNC", "STT_NOTYPE") and not s.name.startswith("$"):
                    a = s["st_value"] & ~1
                    self.name_at.setdefault(a, s.name)
                    self.sym_addrs.add(a)

        self.md = capstone.Cs(capstone.CS_ARCH_ARM, MODE)
        self.md.detail = True

    def read(self, addr, n):
        for base, data, _ in self.segs:
            if base <= addr and addr + n <= base + len(data):
                return data[addr - base: addr - base + n]
        raise KeyError(hex(addr))

    def u8(self, a): return self.read(a, 1)[0]
    def u16(self, a): return int.from_bytes(self.read(a, 2), "little")
    def u32(self, a): return int.from_bytes(self.read(a, 4), "little")

    def decode(self, addr):
        code = self.read(addr, 4) if self._readable(addr, 4) else self.read(addr, 2)
        for i in self.md.disasm(code, addr, 1):
            return i
        return None

    def _readable(self, addr, n):
        try:
            self.read(addr, n); return True
        except KeyError:
            return False


def is_return(i):
    m = i.mnemonic
    ops = i.operands
    if m == "bx" and ops[0].reg == A.ARM_REG_LR: return True
    if m in ("pop", "pop.w", "ldm", "ldm.w", "ldmia", "ldmia.w") and any(o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC for o in ops): return True
    if m in ("ldr", "ldr.w") and ops[0].reg == A.ARM_REG_PC and ops[1].mem.base == A.ARM_REG_SP and not ops[1].mem.index: return True
    return False


def is_ldr_table(i):
    """gcc switch: `adr base, table` then `ldr.w pc, [base, idx, lsl #2]`, table of thumb addresses."""
    return (i.mnemonic in ("ldr", "ldr.w") and i.operands[0].reg == A.ARM_REG_PC
            and i.operands[1].mem.index != 0)


def walk(img: Image, fn: Func):
    end = fn.start + fn.size
    todo = [fn.start]
    fn.labels.add(fn.start)
    while todo:
        a = todo.pop()
        while True:
            if a in fn.insns: break
            if not (fn.start <= a < end):
                fn.problems.append(f"flow leaves function at {a:#x}"); break
            i = img.decode(a)
            if i is None:
                fn.problems.append(f"undecodable at {a:#x}"); break
            fn.insns[a] = i
            m = i.mnemonic
            nxt = a + i.size
            if re.fullmatch(r"it[te]{0,3}", m):
                # capstone knows an it block's conditions (and that 16 bit ops inside it
                # do not set flags) only within one disasm call: decode the block together.
                n = len(m) - 1
                blk = list(img.md.disasm(img.read(a, 2 + 4 * n), a, 1 + n))
                if len(blk) != 1 + n: fn.problems.append(f"it block at {a:#x} short"); break
                fn.insns[a] = blk[0]
                for j in blk[1:]:
                    fn.insns[j.address] = j
                    jm = j.mnemonic
                    if jm.startswith("b") and jm[1:3] in CONDS and len(j.operands) == 1 and j.operands[0].type == A.ARM_OP_IMM:
                        t = j.operands[0].imm
                        if fn.start <= t < end: fn.labels.add(t); todo.append(t)
                a = blk[-1].address + blk[-1].size
                continue
            uncond = i.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID)
            if is_ldr_table(i):
                fn.tables[a] = tbl = ldr_table(img, fn, i)
                for t in tbl:
                    fn.labels.add(t); todo.append(t)
                break
            if m in ("tbb", "tbh"):
                fn.tables[a] = tbl = jump_table(img, fn, i)
                for t in tbl:
                    fn.labels.add(t); todo.append(t)
                break
            if m in ("b", "b.w", "beq", "bne") or (m.startswith("b") and m[1:3] in CONDS and len(m) in (3, 5)):
                t = i.operands[0].imm
                if fn.start <= t < end:
                    fn.labels.add(t); todo.append(t)
                # else: tail call, handled by the backend
                if m in ("b", "b.w") and not in_it(img, fn, i): break
                fn.labels.add(nxt); a = nxt; continue
            if m in ("cbz", "cbnz"):
                t = i.operands[1].imm
                fn.labels.add(t); todo.append(t)
                fn.labels.add(nxt); a = nxt; continue
            if m == "bl" and uncond and img.funcs.get(i.operands[0].imm) and img.funcs[i.operands[0].imm].name in NORETURN:
                break
            if is_return(i) and uncond and not in_it(img, fn, i):
                break
            if m == "bx" or (m in ("mov", "mov.w") and i.operands[0].reg == A.ARM_REG_PC):
                fn.problems.append(f"indirect jump {m} {i.op_str} at {a:#x}")
                if uncond: break
            if m == "udf" or (m == "bkpt" and i.operands[0].imm != 0xab):
                break
            a = nxt


NORETURN = {"exit", "_exit", "_kill_shared", "abort", "__assert_func", "lxr_fault_c"}
CONDS = {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"}


def in_it(img, fn, i):
    """capstone reports the cc of an instruction inside an it block; a plain b in an it
    block is conditional, so flow continues after it."""
    return i.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID)


def jump_table(img, fn, i):
    """gcc's tbb/tbh tables sit right after the instruction (pc-relative, base = addr+4).
    the entry count comes from the bounds check gcc emits before it:
    `cmp idx, #n` then `bhi default` (n+1 entries). a table without that guard is an error."""
    base = i.address + 4
    sz = 1 if i.mnemonic == "tbb" else 2
    idx = i.operands[0].mem.index
    n = None
    prev = sorted(a for a in fn.insns if a < i.address)[-6:]
    for a in reversed(prev):
        p = fn.insns[a]
        if p.mnemonic in ("cmp", "cmp.w") and p.operands[0].reg == idx and p.operands[1].type == A.ARM_OP_IMM:
            n = p.operands[1].imm + 1
            break
    if n is None:
        raise RuntimeError(f"{fn.name}: no bounds check before {i.mnemonic} at {i.address:#x}")
    out = []
    for k in range(n):
        e = img.u8(base + k) if sz == 1 else img.u16(base + 2 * k)
        out.append(base + 2 * e)
    return out


def guard_count(fn, i, idx):
    """entries = n+1 from the `cmp idx, #n` gcc puts in the few instructions before a table."""
    for a in sorted((a for a in fn.insns if a < i.address), reverse=True)[:6]:
        p = fn.insns[a]
        if p.mnemonic in ("cmp", "cmp.w") and p.operands[0].reg == idx and p.operands[1].type == A.ARM_OP_IMM:
            return p.operands[1].imm + 1
    raise RuntimeError(f"{fn.name}: no bounds check before {i.mnemonic} at {i.address:#x}")


def ldr_table(img, fn, i):
    mem = i.operands[1].mem
    assert mem.lshift == 2 or i.operands[1].shift.value == 2, i.op_str
    prev = fn.insns[max(a for a in fn.insns if a < i.address)]
    if prev.mnemonic in ("adr", "adr.w"):
        off = prev.operands[1].imm
    elif prev.mnemonic in ("addw", "add", "add.w") and len(prev.operands) == 3 and prev.operands[1].reg == A.ARM_REG_PC:
        off = prev.operands[2].imm
    else:
        raise RuntimeError(f"no adr before table at {i.address:#x}: {prev.mnemonic} {prev.op_str}")
    assert prev.operands[0].reg == mem.base, hex(i.address)
    tbase = ((prev.address + 4) & ~3) + off
    n = guard_count(fn, i, mem.index)
    out = []
    for k in range(n):
        t = img.u32(tbase + 4 * k)
        assert t & 1, f"table entry {t:#x} not thumb at {i.address:#x}"
        out.append(t & ~1)
    return out


def load(path):
    img = Image(path)
    for fn in list(img.funcs.values()):
        walk(img, fn)
    # function symbols without a size (crtbegin's frame_dummy, ...): size up to the next symbol
    for a, name in img.zero_size:
        if a not in img.funcs:
            nxt = min(x for x in img.sym_addrs if x > a)
            fn = img.funcs[a] = Func(name, a, nxt - a)
            walk(img, fn)
    # assembly routines (newlib's memchr/strlen, crt0 _start, _init) carry no elf size:
    # add every call target that is missing, sized up to the next symbol, until none is.
    while True:
        missing = set()
        for fn in img.funcs.values():
            for i in fn.insns.values():
                if i.mnemonic == "bl" and i.operands[0].imm not in img.funcs:
                    missing.add(i.operands[0].imm)
        if not missing: break
        for t in missing:
            name = img.name_at.get(t, f"sub_{t:x}")
            nxt = min(a for a in img.sym_addrs if a > t)
            fn = img.funcs[t] = Func(name, t, nxt - t)
            walk(img, fn)
    return img


if __name__ == "__main__":
    import sys, collections
    img = load(sys.argv[1])
    forms = collections.Counter()
    n = 0
    for fn in img.funcs.values():
        for p in fn.problems: print(fn.name, p)
        for i in fn.insns.values():
            n += 1
            forms[i.mnemonic] += 1
    print(len(img.funcs), "functions,", n, "instructions,", len(forms), "mnemonics")
    print(" ".join(f"{m}:{c}" for m, c in forms.most_common()))
