"""thumb-2 / fpv5 -> c. one c function per elf function; every instruction becomes c that
follows the armv7-m pseudocode for the forms gcc emitted (see shapes.txt). anything not
handled stops the translator: nothing is guessed.

usage: backend.py lxr.elf outdir
"""
import re, struct, sys, os
from capstone import arm as A
import frontend

COND = {A.ARM_CC_EQ: "Z", A.ARM_CC_NE: "!Z", A.ARM_CC_HS: "C", A.ARM_CC_LO: "!C",
        A.ARM_CC_MI: "N", A.ARM_CC_PL: "!N", A.ARM_CC_VS: "V", A.ARM_CC_VC: "!V",
        A.ARM_CC_HI: "(C && !Z)", A.ARM_CC_LS: "(!C || Z)", A.ARM_CC_GE: "(N == V)",
        A.ARM_CC_LT: "(N != V)", A.ARM_CC_GT: "(!Z && N == V)", A.ARM_CC_LE: "(Z || N != V)"}
FLAGSET = {"adds", "adcs", "subs", "sbcs", "rsbs", "ands", "orrs", "eors", "bics", "orns",
           "mvns", "movs", "lsls", "lsrs", "asrs", "rors", "muls", "negs", "cmp", "cmn", "tst", "teq"}
NORETURN = frontend.NORETURN
SFT = {A.ARM_SFT_ASR: "asr", A.ARM_SFT_LSL: "lsl", A.ARM_SFT_LSR: "lsr", A.ARM_SFT_ROR: "ror", A.ARM_SFT_RRX: "rrx"}


class Unhandled(Exception):
    pass


CCSUF = {A.ARM_CC_EQ: "eq", A.ARM_CC_NE: "ne", A.ARM_CC_HS: "hs", A.ARM_CC_LO: "lo", A.ARM_CC_MI: "mi",
         A.ARM_CC_PL: "pl", A.ARM_CC_VS: "vs", A.ARM_CC_VC: "vc", A.ARM_CC_HI: "hi", A.ARM_CC_LS: "ls",
         A.ARM_CC_GE: "ge", A.ARM_CC_LT: "lt", A.ARM_CC_GT: "gt", A.ARM_CC_LE: "le"}


def clean(i):
    """mnemonic without its condition suffix (ldreq.w -> ldr.w, beq -> b): the condition
    itself is applied around the instruction from i.cc."""
    m = i.mnemonic
    suf = CCSUF.get(i.cc)
    if not suf or re.fullmatch(r"it[te]{0,3}", m): return m
    parts = m.split(".")
    if not parts[0].endswith(suf): raise Unhandled(f"cc {suf} not in mnemonic {m}")
    parts[0] = parts[0][:-len(suf)]
    return ".".join(parts)


def fpkind(name):
    """'s' or 'd' for vfp registers s0..s31 / d0..d15, else None (r9 is spelled 'sb')."""
    if len(name) >= 2 and name[0] in "sd" and name[1:].isdigit(): return name[0]
    return None


class Gen:
    def __init__(self, img):
        self.img = img
        self.md = img.md
        self.funcs = img.funcs
        self.by_name = {f.name: a for a, f in img.funcs.items()}

    # ---------------------------------------------------------------- operands
    def rn(self, reg):
        n = self.md.reg_name(reg)
        m = {"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}
        if n in m: return m[n]
        if n.startswith("r"): return int(n[1:])
        raise Unhandled("not a core reg " + n)

    def sreg(self, reg):
        n = self.md.reg_name(reg)
        if fpkind(n): return (n[0], int(n[1:]))
        raise Unhandled("not an fp reg " + n)

    def R(self, i, reg):
        """value of a core register as read by instruction i (pc reads addr+4)."""
        k = self.rn(reg)
        if k == 15: return "0x%xu" % (i.address + 4)
        return "R[%d]" % k

    def shifted(self, i, o, carry=False):
        """register operand with optional immediate shift. returns (expr, carry_expr or None)."""
        v = self.R(i, o.reg)
        t, n = o.shift.type, o.shift.value
        if not t or n == 0 and t != A.ARM_SFT_RRX:
            return v, None
        if t == A.ARM_SFT_LSL: return f"({v} << {n})", (f"(({v} >> {32 - n}) & 1u)" if carry else None)
        if t == A.ARM_SFT_LSR:
            if n == 32: return "0u", (f"({v} >> 31)" if carry else None)
            return f"({v} >> {n})", (f"(({v} >> {n - 1}) & 1u)" if carry else None)
        if t == A.ARM_SFT_ASR:
            if n == 32: return f"(uint32_t)((int32_t){v} >> 31)", (f"({v} >> 31)" if carry else None)
            return f"(uint32_t)((int32_t){v} >> {n})", (f"(((uint32_t)((int32_t){v} >> {n - 1})) & 1u)" if carry else None)
        if t == A.ARM_SFT_ROR: return f"(({v} >> {n}) | ({v} << {32 - n}))", (f"(({v} >> {n - 1}) & 1u)" if carry else None)
        if t == A.ARM_SFT_RRX: return f"((C << 31) | ({v} >> 1))", (f"({v} & 1u)" if carry else None)
        raise Unhandled("shift %d" % t)

    def op2(self, i, o, carry=False):
        if o.type == A.ARM_OP_IMM:
            v = o.imm & 0xffffffff
            c = None
            if carry:
                c = self.imm_carry(i, v)
            return "0x%xu" % v, c
        if o.type == A.ARM_OP_REG:
            return self.shifted(i, o, carry)
        raise Unhandled("op2 type %d" % o.type)

    def imm_carry(self, i, v):
        """ThumbExpandImm_C: carry out is unchanged unless the 12 bit immediate rotates."""
        if i.size == 2: return None
        hw1, hw2 = struct.unpack("<HH", bytes(i.bytes))
        imm12 = ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xff)
        if (imm12 >> 10) == 0: return None
        return "%du" % ((v >> 31) & 1)

    def addr(self, i, o):
        m = o.mem
        if m.base == A.ARM_REG_PC:
            base = "0x%xu" % (((i.address + 4) & ~3))
        else:
            base = "R[%d]" % self.rn(m.base)
        if m.index:
            idx = "R[%d]" % self.rn(m.index)
            sh = m.lshift or (o.shift.value if o.shift.type == A.ARM_SFT_LSL else 0)
            if o.shift.type not in (0, A.ARM_SFT_LSL): raise Unhandled("index shift")
            if sh: idx = f"({idx} << {sh})"
            return f"({base} {'-' if o.subtracted else '+'} {idx})", base
        d = m.disp
        if d: return f"({base} + 0x{d & 0xffffffff:x}u)", base
        return base, base

    # ------------------------------------------------------------- flags
    @staticmethod
    def nz(r):
        return f"N = {r} >> 31; Z = {r} == 0;"

    # ------------------------------------------------------------- one insn
    def insn(self, fn, i):
        m = clean(i)
        base = m.split(".")[0]
        ops = i.operands
        S = base in FLAGSET
        if base in ("it", "ite", "itt", "itte", "itet", "itete", "ittt", "itttt", "itee", "iteee", "ittee", "itett"):
            return ""
        if base in ("nop", "dsb", "isb", "dmb"): return ""
        if base == "cpsid": return "PRIMASK = 1;"
        if base == "cpsie": return "PRIMASK = 0;"

        # -------------------------------------------------------- branches
        if base == "bl":
            t = ops[0].imm
            if t not in self.funcs: raise Unhandled("bl into non function %x" % t)
            s = f"R[14] = 0x{i.address + i.size | 1:x}u; FL_OUT(); F_{t:x}(); FL_IN();"
            if self.funcs[t].name in NORETURN: s += " rt_fatal(\"returned from noreturn\", 0x%xu);" % i.address
            return s
        if base == "blx":
            return f"{{ uint32_t t_ = R[{self.rn(ops[0].reg)}]; R[14] = 0x{i.address + i.size | 1:x}u; FL_OUT(); rt_call(t_); FL_IN(); }}"
        if base == "bx":
            k = self.rn(ops[0].reg)
            if k == 14: return "FL_OUT(); return;"
            return f"{{ uint32_t t_ = R[{k}]; FL_OUT(); rt_call(t_); return; }}"      # tail call through pointer
        if base == "b" or (base[0] == "b" and base[1:] in frontend.CONDS):
            t = ops[0].imm
            if fn.start <= t < fn.start + fn.size:
                return f"goto L_{t:x};"
            if t not in self.funcs: raise Unhandled("tail branch to non function %x" % t)
            return f"FL_OUT(); F_{t:x}(); return;"
        if base in ("cbz", "cbnz"):
            r = self.R(i, ops[0].reg); t = ops[1].imm
            return f"if ({r} {'==' if base == 'cbz' else '!='} 0) goto L_{t:x};"
        if base in ("tbb", "tbh"):
            tbl = fn.tables[i.address]
            idx = "R[%d]" % self.rn(ops[0].mem.index)
            cases = " ".join(f"case {k}: goto L_{t:x};" for k, t in enumerate(tbl))
            return f"switch ({idx}) {{ {cases} default: rt_fatal(\"table index\", 0x{i.address:x}u); }}"
        if frontend.is_ldr_table(i):
            tbl = fn.tables[i.address]
            idx = "R[%d]" % self.rn(ops[1].mem.index)
            cases = " ".join(f"case {k}: goto L_{t:x};" for k, t in enumerate(tbl))
            return f"switch ({idx}) {{ {cases} default: rt_fatal(\"table index\", 0x{i.address:x}u); }}"
        if base == "bkpt":
            if ops[0].imm == 0xab: return "R[0] = rt_semihost(R[0], R[1]);"
            return f"rt_fatal(\"bkpt\", 0x{i.address:x}u);"
        if base == "udf":
            return f"rt_fatal(\"udf\", 0x{i.address:x}u);"

        # -------------------------------------------------------- push / pop / ldm / stm
        if base in ("push", "stmdb") and (base == "push" or (ops[0].reg == A.ARM_REG_SP and i.writeback)):
            regs = [self.rn(o.reg) for o in (ops if base == "push" else ops[1:])]
            n = len(regs)
            st = " ".join(f"st32(a_ + {4 * k}, R[{r}]);" for k, r in enumerate(regs))
            return f"{{ uint32_t a_ = R[13] - {4 * n}; {st} R[13] = a_; }}"
        if base == "pop" or (base in ("ldm", "ldmia") and ops[0].reg == A.ARM_REG_SP and i.writeback):
            regs = [self.rn(o.reg) for o in (ops if base == "pop" else ops[1:])]
            n = len(regs)
            out = ["uint32_t a_ = R[13];"]
            ret = False
            for k, r in enumerate(regs):
                if r == 15: ret = True; out.append(f"uint32_t pc_ = ld32(a_ + {4 * k});")
                else: out.append(f"R[{r}] = ld32(a_ + {4 * k});")
            out.append(f"R[13] = a_ + {4 * n};")
            if ret: out.append("(void)pc_; FL_OUT(); return;")
            return "{ " + " ".join(out) + " }"
        if base in ("ldm", "ldmia", "ldmdb", "stm", "stmia", "stmdb"):
            b = self.rn(ops[0].reg)
            regs = [self.rn(o.reg) for o in ops[1:]]
            n = len(regs)
            if 15 in regs or 13 in regs: raise Unhandled("ldm/stm with pc/sp")
            db = base.endswith("db")
            out = [f"uint32_t a_ = R[{b}]{' - %d' % (4 * n) if db else ''};"]
            ld = base.startswith("ldm")
            for k, r in enumerate(regs):
                out.append(f"R[{r}] = ld32(a_ + {4 * k});" if ld else f"st32(a_ + {4 * k}, R[{r}]);")
            if i.writeback:
                if ld and b in regs: raise Unhandled("ldm wb with base in list")
                out.append(f"R[{b}] = {'a_' if db else 'a_ + %d' % (4 * n)};")
            return "{ " + " ".join(out) + " }"

        # -------------------------------------------------------- loads / stores
        if base in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh", "ldrd", "strd"):
            return self.ldst(fn, i, base, ops)

        # -------------------------------------------------------- data processing
        if base in ("mov", "movs", "mvn", "mvns", "movw", "movt"):
            d = self.rn(ops[0].reg)
            if base == "movt":
                return f"R[{d}] = (R[{d}] & 0xffffu) | (0x{ops[1].imm & 0xffff:x}u << 16);"
            if d == 15: raise Unhandled("mov pc")
            v, c = self.op2(i, ops[1], carry=S)
            if base.startswith("mvn"): v = f"~{v}"
            s = f"{{ uint32_t r_ = {v};"
            if S:
                s += " " + self.nz("r_")
                if c: s += f" C = {c};"
            return s + f" R[{d}] = r_; }}"
        if base in ("and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics", "orn", "orns", "tst", "teq"):
            opn = {"and": "&", "orr": "|", "eor": "^", "bic": "& ~", "orn": "| ~", "tst": "&", "teq": "^"}[base.rstrip("s") if base not in ("tst",) else "tst"]
            if base in ("tst", "teq"):
                a, b2 = self.R(i, ops[0].reg), ops[1]
                d = None
            elif len(ops) == 2:
                d = self.rn(ops[0].reg); a = self.R(i, ops[0].reg); b2 = ops[1]
            else:
                d = self.rn(ops[0].reg); a = self.R(i, ops[1].reg); b2 = ops[2]
            v, c = self.op2(i, b2, carry=S)
            s = f"{{ uint32_t r_ = {a} {opn}({v});"
            if S:
                s += " " + self.nz("r_")
                if c: s += f" C = {c};"
            if d is not None: s += f" R[{d}] = r_;"
            return s + " }"
        if base in ("add", "adds", "adc", "adcs", "sub", "subs", "sbc", "sbcs", "rsb", "rsbs", "cmp", "cmn", "addw", "subw", "adr"):
            return self.arith(fn, i, base, ops, S)
        if base in ("lsl", "lsls", "lsr", "lsrs", "asr", "asrs", "ror", "rors"):
            d = self.rn(ops[0].reg)
            src, amt = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
            x = self.R(i, src.reg)
            k = base[:3]
            if amt.type == A.ARM_OP_IMM:
                n = amt.imm
                if k == "lsl" and n == 0:
                    s = f"{{ uint32_t r_ = {x};"
                else:
                    s = f"{{ uint32_t c_ = C, r_ = {k}_c({x}, {n}u, &c_);"
                    if S: s += " C = c_;"
            else:
                s = f"{{ uint32_t c_ = C, r_ = {k}_c({x}, {self.R(i, amt.reg)}, &c_);"
                if S: s += " C = c_;"
            if S: s += " " + self.nz("r_")
            return s + f" R[{d}] = r_; }}"
        if base in ("mul", "muls"):
            d = self.rn(ops[0].reg)
            a, b2 = (self.R(i, ops[1].reg), self.R(i, ops[2].reg)) if len(ops) == 3 else (self.R(i, ops[0].reg), self.R(i, ops[1].reg))
            s = f"{{ uint32_t r_ = {a} * {b2};"
            if S: s += " " + self.nz("r_")
            return s + f" R[{d}] = r_; }}"
        if base in ("mla", "mls"):
            d, a, b2, c = [self.rn(o.reg) for o in ops]
            return f"R[{d}] = R[{c}] {'+' if base == 'mla' else '-'} R[{a}] * R[{b2}];"
        if base in ("umull", "smull", "umlal", "smlal"):
            lo, hi, a, b2 = [self.rn(o.reg) for o in ops]
            if base[0] == "u": p = f"(uint64_t)R[{a}] * R[{b2}]"
            else: p = f"(uint64_t)((int64_t)(int32_t)R[{a}] * (int32_t)R[{b2}])"
            if base.endswith("lal"): p = f"({p} + (((uint64_t)R[{hi}] << 32) | R[{lo}]))"
            return f"{{ uint64_t p_ = {p}; R[{lo}] = (uint32_t)p_; R[{hi}] = (uint32_t)(p_ >> 32); }}"
        if base == "smulbb":
            d, a, b2 = [self.rn(o.reg) for o in ops]
            return f"R[{d}] = (uint32_t)((int32_t)(int16_t)R[{a}] * (int16_t)R[{b2}]);"
        if base == "smlabb":
            d, a, b2, c = [self.rn(o.reg) for o in ops]
            return f"R[{d}] = (uint32_t)((int32_t)(int16_t)R[{a}] * (int16_t)R[{b2}]) + R[{c}];"
        if base in ("udiv", "sdiv"):
            d, a, b2 = [self.rn(o.reg) for o in ops] if len(ops) == 3 else [self.rn(ops[0].reg), self.rn(ops[0].reg), self.rn(ops[1].reg)]
            return f"R[{d}] = {base}(R[{a}], R[{b2}]);"
        if base in ("uxtb", "uxth", "sxtb", "sxth"):
            d = self.rn(ops[0].reg); x, _ = self.shifted(i, ops[1])
            f = {"uxtb": "({x} & 0xffu)", "uxth": "({x} & 0xffffu)", "sxtb": "sx8({x})", "sxth": "sx16({x})"}[base]
            return f"R[{d}] = " + f.format(x=x) + ";"
        if base == "uxtab":
            d, a = self.rn(ops[0].reg), self.R(i, ops[1].reg); x, _ = self.shifted(i, ops[2])
            return f"R[{d}] = {a} + ({x} & 0xffu);"
        if base == "ubfx" or base == "sbfx":
            d, x = self.rn(ops[0].reg), self.R(i, ops[1].reg); lsb, w = ops[2].imm, ops[3].imm
            if base == "ubfx":
                mask = (1 << w) - 1
                return f"R[{d}] = ({x} >> {lsb}) & 0x{mask:x}u;"
            return f"R[{d}] = (uint32_t)(((int32_t)({x} << {32 - lsb - w})) >> {32 - w});"
        if base == "bfi":
            d, x = self.rn(ops[0].reg), self.R(i, ops[1].reg); lsb, w = ops[2].imm, ops[3].imm
            mask = ((1 << w) - 1) << lsb
            return f"R[{d}] = (R[{d}] & 0x{(~mask) & 0xffffffff:x}u) | (({x} << {lsb}) & 0x{mask:x}u);"
        if base == "bfc":
            d = self.rn(ops[0].reg); lsb, w = ops[1].imm, ops[2].imm
            mask = ((1 << w) - 1) << lsb
            return f"R[{d}] &= 0x{(~mask) & 0xffffffff:x}u;"
        if base == "clz":
            return f"R[{self.rn(ops[0].reg)}] = clz32({self.R(i, ops[1].reg)});"
        if base in ("ssat", "usat"):
            d = self.rn(ops[0].reg); bits = ops[1].imm; x, _ = self.shifted(i, ops[2])
            return f"R[{d}] = {base}((int32_t){x}, {bits});"
        if base == "uadd8":
            d, a, b2 = [self.rn(o.reg) for o in ops]
            return (f"{{ uint32_t r_ = 0; GE = 0; for (int k_ = 0; k_ < 4; k_++) {{ uint32_t s_ = ((R[{a}] >> (8 * k_)) & 0xffu) + ((R[{b2}] >> (8 * k_)) & 0xffu);"
                    f" r_ |= (s_ & 0xffu) << (8 * k_); if (s_ >= 0x100u) GE |= 1u << k_; }} R[{d}] = r_; }}")
        if base == "sel":
            d, a, b2 = [self.rn(o.reg) for o in ops]
            return (f"{{ uint32_t r_ = 0; for (int k_ = 0; k_ < 4; k_++) r_ |= (((GE >> k_) & 1u) ? R[{a}] : R[{b2}]) & (0xffu << (8 * k_)); R[{d}] = r_; }}")
        if base in ("qadd16", "qsub16"):
            d, a, b2 = [self.rn(o.reg) for o in ops]
            return f"R[{d}] = {base}(R[{a}], R[{b2}]);"
        if base == "revsh":
            d, x = self.rn(ops[0].reg), self.R(i, ops[1].reg)
            return f"R[{d}] = sx16((({x} & 0xffu) << 8) | (({x} >> 8) & 0xffu));"
        if base == "rev":
            d, x = self.rn(ops[0].reg), self.R(i, ops[1].reg)
            return f"R[{d}] = __builtin_bswap32({x});"
        if base == "mrs":
            d = self.rn(ops[0].reg); sr = self.md.reg_name(ops[1].reg) if ops[1].type == A.ARM_OP_REG else i.op_str.split(",")[1].strip()
            src = {"msp": "R[13]", "psp": "R[13]", "primask": "PRIMASK", "basepri": "BASEPRI", "control": "CONTROL", "ipsr": "0u",
                   "apsr": "((N << 31) | (Z << 30) | (C << 29) | (V << 28) | (GE << 16))"}.get(sr.lower())
            if src is None: raise Unhandled("mrs " + i.op_str)
            return f"R[{d}] = {src};"
        if base == "msr":
            sr = i.op_str.split(",")[0].strip().lower(); x = self.R(i, ops[1].reg)
            dst = {"primask": "PRIMASK = {x} & 1u", "basepri": "BASEPRI = {x} & 0xffu", "control": "CONTROL = {x} & 7u", "msp": "R[13] = {x}"}.get(sr)
            if dst is None: raise Unhandled("msr " + i.op_str)
            return dst.format(x=x) + ";"

        if base.startswith("v"):
            return self.vfp(fn, i, m, ops)
        raise Unhandled(f"{m} {i.op_str}")

    def arith(self, fn, i, base, ops, S):
        if base == "adr":
            d = self.rn(ops[0].reg)
            return f"R[{d}] = 0x{((i.address + 4) & ~3) + ops[1].imm & 0xffffffff:x}u;"
        if base in ("cmp", "cmn"):
            a = self.R(i, ops[0].reg); b2, _ = self.op2(i, ops[1])
            if base == "cmp": return f"{{ uint32_t r_ = awc({a}, ~({b2}), 1u, &C, &V); {self.nz('r_')} }}"
            return f"{{ uint32_t r_ = awc({a}, {b2}, 0u, &C, &V); {self.nz('r_')} }}"
        d = self.rn(ops[0].reg)
        if len(ops) == 2:
            a = self.R(i, ops[0].reg); bo = ops[1]
        else:
            a = self.R(i, ops[1].reg); bo = ops[2]
            # add rd, pc, #imm (the adr form): pc is Align(pc, 4)
            if ops[1].reg == A.ARM_REG_PC and base in ("add", "addw", "sub", "subw") and bo.type == A.ARM_OP_IMM:
                a = "0x%xu" % ((i.address + 4) & ~3)
        b2, _ = self.op2(i, bo)
        if d == 15: raise Unhandled("arith to pc")
        k = {"add": "add", "adds": "add", "addw": "add", "adc": "adc", "adcs": "adc", "sub": "sub", "subs": "sub",
             "subw": "sub", "sbc": "sbc", "sbcs": "sbc", "rsb": "rsb", "rsbs": "rsb"}[base]
        if k == "add": x, y, cin = a, b2, "0u"
        elif k == "adc": x, y, cin = a, b2, "C"
        elif k == "sub": x, y, cin = a, f"~({b2})", "1u"
        elif k == "sbc": x, y, cin = a, f"~({b2})", "C"
        else: x, y, cin = f"~({a})", b2, "1u"
        if S:
            return f"{{ uint32_t r_ = awc({x}, {y}, {cin}, &C, &V); {self.nz('r_')} R[{d}] = r_; }}"
        if k == "add": return f"R[{d}] = {a} + {b2};"
        if k == "sub": return f"R[{d}] = {a} - ({b2});"
        if k == "rsb": return f"R[{d}] = ({b2}) - {a};"
        if k == "adc": return f"R[{d}] = {a} + {b2} + C;"
        if k == "sbc": return f"R[{d}] = {a} + ~({b2}) + C;"
        raise Unhandled(base)

    def ldst(self, fn, i, base, ops):
        pair = base in ("ldrd", "strd")
        regs = [self.rn(ops[0].reg)] + ([self.rn(ops[1].reg)] if pair else [])
        mo = ops[2] if pair else ops[1]
        post = None
        rest = ops[3:] if pair else ops[2:]
        if rest:                                    # post-indexed: [rn], #imm
            post = rest[0].imm if rest[0].type == A.ARM_OP_IMM else None
            if post is None: raise Unhandled("post index reg")
            if rest[0].subtracted: post = -post
        a, b = self.addr(i, mo)
        bn = None if mo.mem.base == A.ARM_REG_PC else self.rn(mo.mem.base)
        if post is not None:
            a = b
        width = {"ldr": 32, "str": 32, "ldrb": 8, "strb": 8, "ldrh": 16, "strh": 16, "ldrsb": 8, "ldrsh": 16, "ldrd": 32, "strd": 32}[base]
        out = [f"uint32_t a_ = {a};"]
        if base.startswith("ld"):
            ld = {32: "ld32", 16: "ld16", 8: "ld8"}[width]
            if 15 in regs:
                # ldr pc, [sp], #4 is a return (tables were handled before)
                if not (base == "ldr" and bn == 13 and post == 4): raise Unhandled("ldr pc " + i.op_str)
                out.append("R[13] = a_ + 4; FL_OUT(); return;")
                return "{ " + " ".join(out) + " }"
            vals = []
            for k, r in enumerate(regs):
                v = f"{ld}(a_ + {4 * k})"
                if base == "ldrsb": v = f"sx8({v})"
                if base == "ldrsh": v = f"sx16({v})"
                vals.append((r, v))
            tmp = [f"uint32_t v{k}_ = {v};" for k, (r, v) in enumerate(vals)]
            out += tmp
            if i.writeback or post is not None:
                if bn in regs: raise Unhandled("load writeback into base")
                out.append(f"R[{bn}] = {'a_ + %d' % post if post is not None else 'a_'};")
            out += [f"R[{r}] = v{k}_;" for k, (r, v) in enumerate(vals)]
        else:
            st = {32: "st32", 16: "st16", 8: "st8"}[width]
            for k, r in enumerate(regs):
                out.append(f"{st}(a_ + {4 * k}, {self.R(i, ops[k].reg)});")
            if i.writeback or post is not None:
                out.append(f"R[{bn}] = {'a_ + %d' % post if post is not None else 'a_'};")
        return "{ " + " ".join(out) + " }"

    # ------------------------------------------------------------------ vfp
    def S(self, o):
        k, n = self.sreg(o.reg)
        if k != "s": raise Unhandled("expected s reg")
        return f"F.s[{n}]"

    def D(self, o):
        k, n = self.sreg(o.reg)
        if k != "d": raise Unhandled("expected d reg")
        return f"F.d[{n}]"

    def vfp(self, fn, i, m, ops):
        parts = m.split(".")
        base = parts[0]
        ty = parts[1:]
        is64 = ty and ty[-1] == "f64" if base not in ("vcvt", "vcvtb") else None
        if base in ("vldr", "vstr"):
            k, n = self.sreg(ops[0].reg)
            a, _ = self.addr(i, ops[1])
            if base == "vldr":
                return f"F.{k}[{n}] = {'ld64' if k == 'd' else 'ld32'}({a});"
            return f"{'st64' if k == 'd' else 'st32'}({a}, F.{k}[{n}]);"
        if base in ("vpush", "vpop"):
            regs = [self.sreg(o.reg) for o in ops]
            sz = sum(8 if k == "d" else 4 for k, _ in regs)
            out = []
            off = 0
            for k, n in regs:
                if base == "vpush": out.append(f"{'st64' if k == 'd' else 'st32'}(a_ + {off}, F.{k}[{n}]);")
                else: out.append(f"F.{k}[{n}] = {'ld64' if k == 'd' else 'ld32'}(a_ + {off});")
                off += 8 if k == "d" else 4
            if base == "vpush": return f"{{ uint32_t a_ = R[13] - {sz}; " + " ".join(out) + " R[13] = a_; }"
            return "{ uint32_t a_ = R[13]; " + " ".join(out) + f" R[13] = a_ + {sz}; }}"
        if base == "vldmia":
            b = self.rn(ops[0].reg)
            regs = [self.sreg(o.reg) for o in ops[1:]]
            out, off = [], 0
            for k, n in regs:
                out.append(f"F.{k}[{n}] = {'ld64' if k == 'd' else 'ld32'}(a_ + {off});"); off += 8 if k == "d" else 4
            if i.writeback: out.append(f"R[{b}] = a_ + {off};")
            return f"{{ uint32_t a_ = R[{b}]; " + " ".join(out) + " }"
        if base == "vmsr":
            if not i.op_str.lower().startswith("fpscr"): raise Unhandled("vmsr " + i.op_str)
            return f"FPSCR = {self.R(i, ops[-1].reg)}; rt_fz_mode_check();"
        if base == "vmrs":
            if self.md.reg_name(ops[0].reg).lower().startswith("apsr"):
                return "N = FPSCR >> 31; Z = (FPSCR >> 30) & 1u; C = (FPSCR >> 29) & 1u; V = (FPSCR >> 28) & 1u;"
            return f"R[{self.rn(ops[0].reg)}] = FPSCR;"
        if base == "vmov":
            return self.vmov(i, m, ops)
        if base in ("vadd", "vsub", "vmul", "vdiv"):
            w = "64" if is64 else "32"; R_ = self.D if is64 else self.S
            f = {"vadd": "xadd", "vsub": "xsub", "vmul": "xmul", "vdiv": "xdiv"}[base] + w
            return f"{R_(ops[0])} = {f}({R_(ops[1])}, {R_(ops[2])});"
        if base in ("vmaxnm", "vminnm"):
            w = "64" if is64 else "32"; R_ = self.D if is64 else self.S
            return f"{R_(ops[0])} = x{base[1:]}{w}({R_(ops[1])}, {R_(ops[2])});"
        if base == "vrintz":
            w = "64" if is64 else "32"; R_ = self.D if is64 else self.S
            return f"{R_(ops[0])} = xrintz{w}({R_(ops[1])});"
        if base in ("vabs", "vneg", "vsqrt"):
            R_ = self.D if is64 else self.S
            if base == "vsqrt" and is64: raise Unhandled("vsqrt.f64")
            f = {"vabs": "fabs", "vneg": "fneg", "vsqrt": "xsqrt"}[base] + ("64" if is64 else "32")
            return f"{R_(ops[0])} = {f}({R_(ops[1])});"
        if base in ("vfma", "vfms", "vfnma", "vfnms"):
            w = "64" if is64 else "32"; R_ = self.D if is64 else self.S
            d, n, mm = R_(ops[0]), R_(ops[1]), R_(ops[2])
            neg = "fneg" + w
            # FPMulAdd(addend, op1, op2): vfma d+n*m, vfms d+(-n)*m, vfnma (-d)+(-n)*m, vfnms (-d)+n*m
            ad = d if base in ("vfma", "vfms") else f"{neg}({d})"
            o1 = n if base in ("vfma", "vfnms") else f"{neg}({n})"
            return f"{d} = xmuladd{w}({ad}, {o1}, {mm});"
        if base in ("vmla", "vmls", "vnmla", "vnmls"):
            w = "64" if is64 else "32"; R_ = self.D if is64 else self.S
            d, n, mm = R_(ops[0]), R_(ops[1]), R_(ops[2])
            neg = "fneg" + w
            p = f"xmul{w}({n}, {mm})"
            # vmla d+p, vmls d+(-p), vnmla (-d)+(-p), vnmls (-d)+p  (FPAdd, non fused)
            ad = d if base in ("vmla", "vmls") else f"{neg}({d})"
            pp = p if base in ("vmla", "vnmls") else f"{neg}({p})"
            return f"{d} = xadd{w}({ad}, {pp});"
        if base in ("vcmp", "vcmpe"):
            R_ = self.D if is64 else self.S
            b2 = R_(ops[1]) if ops[1].type == A.ARM_OP_REG else "0"
            if ops[1].type != A.ARM_OP_REG and (ops[1].type not in (A.ARM_OP_IMM, A.ARM_OP_FP) or (ops[1].imm if ops[1].type == A.ARM_OP_IMM else ops[1].fp) != 0):
                raise Unhandled("vcmp imm " + i.op_str)
            return f"FPSCR = (FPSCR & 0x0fffffffu) | (xcmp{'64' if is64 else '32'}({R_(ops[0])}, {b2}) << 28);"
        if base in ("vseleq", "vselgt", "vselge", "vselvs"):
            R_ = self.D if is64 else self.S
            c = {"vseleq": "Z", "vselgt": "(!Z && N == V)", "vselge": "(N == V)", "vselvs": "V"}[base]
            return f"{R_(ops[0])} = {c} ? {R_(ops[1])} : {R_(ops[2])};"
        if base in ("vcvt", "vcvtb"):
            return self.vcvt(i, m, ops)
        raise Unhandled(f"{m} {i.op_str}")

    def vmov(self, i, m, ops):
        k0 = fpkind(self.md.reg_name(ops[0].reg)) or "r"
        if len(ops) == 2 and ops[1].type == A.ARM_OP_FP:
            if m.endswith("f64"):
                return f"{self.D(ops[0])} = 0x{struct.unpack('<Q', struct.pack('<d', ops[1].fp))[0]:x}ull;"
            return f"{self.S(ops[0])} = 0x{struct.unpack('<I', struct.pack('<f', ops[1].fp))[0]:x}u;"
        if len(ops) == 2:
            k1 = fpkind(self.md.reg_name(ops[1].reg)) or "r"
            if k0[0] == "s" and k1[0] == "s": return f"{self.S(ops[0])} = {self.S(ops[1])};"
            if k0[0] == "d" and k1[0] == "d": return f"{self.D(ops[0])} = {self.D(ops[1])};"
            if k0[0] == "s": return f"{self.S(ops[0])} = {self.R(i, ops[1].reg)};"
            if k1[0] == "s": return f"R[{self.rn(ops[0].reg)}] = {self.S(ops[1])};"
            raise Unhandled("vmov " + i.op_str)
        if len(ops) == 3:
            if k0[0] == "d":
                return f"{self.D(ops[0])} = (uint64_t){self.R(i, ops[1].reg)} | ((uint64_t){self.R(i, ops[2].reg)} << 32);"
            if fpkind(self.md.reg_name(ops[2].reg)) == "d":
                lo, hi = self.rn(ops[0].reg), self.rn(ops[1].reg)
                return f"{{ uint64_t t_ = {self.D(ops[2])}; R[{lo}] = (uint32_t)t_; R[{hi}] = (uint32_t)(t_ >> 32); }}"
        raise Unhandled("vmov " + i.op_str)

    def vcvt(self, i, m, ops):
        parts = m.split(".")
        if parts[0] == "vcvtb":
            if parts[1:] == ["f32", "f16"]: return f"{self.S(ops[0])} = cvt_f32_f16({self.S(ops[1])});"
            if parts[1:] == ["f16", "f32"]:
                d = self.S(ops[0])
                return f"{d} = ({d} & 0xffff0000u) | xcvt_f16_f32({self.S(ops[1])});"
            raise Unhandled(m)
        to, frm = parts[1], parts[2]
        fix = ops[2].imm if len(ops) == 3 else None
        key = (to, frm)
        if fix is not None:
            if key == ("f32", "s32"): return f"{self.S(ops[0])} = cvt_f32_s32_fix({self.S(ops[1])}, {fix});"
            if key == ("s32", "f32"): return f"{self.S(ops[0])} = xcvt_s32_f32_fix({self.S(ops[1])}, {fix});"
            raise Unhandled(m + " fixed")
        tab = {("f32", "s32"): ("S", "cvt_f32_s32", "S"), ("f32", "u32"): ("S", "cvt_f32_u32", "S"),
               ("s32", "f32"): ("S", "xcvt_s32_f32", "S"), ("u32", "f32"): ("S", "xcvt_u32_f32", "S"),
               ("f64", "f32"): ("D", "xcvt_f64_f32", "S"), ("f32", "f64"): ("S", "xcvt_f32_f64", "D"),
               ("f64", "s32"): ("D", "cvt_f64_s32", "S"), ("s32", "f64"): ("S", "xcvt_s32_f64", "D")}
        if key not in tab: raise Unhandled(m)
        dk, f, sk = tab[key]
        return f"{getattr(self, dk)(ops[0])} = {f}({getattr(self, sk)(ops[1])});"

    # ------------------------------------------------------------- functions
    def function(self, fn):
        lines = [f"void F_{fn.start:x}(void) /* {fn.name} */", "{",
                 "    uint32_t N, Z, C, V, GE; FL_IN();",
                 f"    RT_ENTER(0x{fn.start:x}u);"]
        addrs = sorted(fn.insns)
        for a in addrs:
            i = fn.insns[a]
            if a in fn.labels: lines.append(f"L_{a:x}:;")
            body = self.insn(fn, i)
            if not body: continue
            cc = i.cc
            base = i.mnemonic.split(".")[0]
            is_bcc = base[0] == "b" and base[1:] in frontend.CONDS
            if cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                body = f"if ({COND[cc]}) {{ {body} }}"
            lines.append(f"    {body} /* {a:x}: {i.mnemonic} {i.op_str} */")
            nxt = a + i.size
            # fall off the end of the decoded flow (noreturn call at the end, etc.)
            if nxt not in fn.insns and not self.ends_flow(i):
                lines.append(f"    rt_fatal(\"fell off decoded flow\", 0x{nxt:x}u);")
        lines.append("    rt_fatal(\"end of function\", 0x%xu);" % fn.start)
        lines.append("}")
        return "\n".join(lines)

    def ends_flow(self, i):
        base = i.mnemonic.split(".")[0]
        uncond = i.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID)
        if not uncond: return False
        if base in ("b", "bx", "tbb", "tbh", "udf"): return True
        if frontend.is_return(i) or frontend.is_ldr_table(i): return True
        return False


def main(elf, outdir):
    img = frontend.load(elf)
    g = Gen(img)
    os.makedirs(outdir, exist_ok=True)
    errors = []
    bodies = []
    for a in sorted(img.funcs):
        fn = img.funcs[a]
        try:
            bodies.append(g.function(fn))
        except Unhandled as e:
            errors.append(f"{fn.name}: {e}")
    if errors:
        print("\n".join(sorted(set(errors))), file=sys.stderr)
        print(len(errors), "functions with unhandled instructions", file=sys.stderr)
        sys.exit(1)
    with open(os.path.join(outdir, "fw.c"), "w") as f:
        f.write("/* generated by 3.recomp/backend.py from %s. do not edit. */\n" % os.path.basename(elf))
        f.write('#include "rt.h"\n')
        for a in sorted(img.funcs):
            f.write(f"void F_{a:x}(void);\n")
        for b in bodies:
            f.write(b + "\n\n")
        f.write("void rt_call(uint32_t a)\n{\n    switch (a & ~1u) {\n")
        for a in sorted(img.funcs):
            f.write(f"    case 0x{a:x}u: F_{a:x}(); return;\n")
        f.write("    default: rt_fatal(\"indirect call to unknown address\", a);\n    }\n}\n")
        f.write(f"const uint32_t rt_entry = 0x{img.entry:x}u;\n")
    # initial memory image: every PT_LOAD segment's file bytes (the rest is zero)
    with open(os.path.join(outdir, "image.c"), "w") as f:
        f.write("/* generated: initial memory contents of the elf's load segments. */\n#include <stdint.h>\n")
        for k, (b, d, _) in enumerate(img.segs):
            f.write(f"static const uint8_t seg{k}[{len(d)}] = {{\n")
            for o in range(0, len(d), 32):
                f.write(",".join(str(x) for x in d[o:o + 32]) + ",\n")
            f.write("};\n")
        f.write("const struct rt_seg { uint32_t addr, len; const uint8_t *data; } rt_segs[] = {\n")
        f.write("".join(f"    {{ 0x{b:x}u, {len(d)}u, seg{k} }},\n" for k, (b, d, _) in enumerate(img.segs)))
        f.write("};\nconst unsigned rt_nsegs = %d;\n" % len(img.segs))
    print(f"{len(bodies)} functions translated -> {outdir}/fw.c, image.c")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
