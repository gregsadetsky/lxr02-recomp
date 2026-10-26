"""list every (mnemonic, operand shape) in the elf with one example, to drive the backend."""
import sys, collections
import frontend
from capstone import arm as A
T = {A.ARM_OP_REG: "r", A.ARM_OP_IMM: "i", A.ARM_OP_MEM: "m", A.ARM_OP_FP: "f", A.ARM_OP_SYSREG: "sys", A.ARM_OP_CIMM: "ci", A.ARM_OP_PIMM: "pi"}
img = frontend.load(sys.argv[1])
seen = collections.OrderedDict()
for fn in img.funcs.values():
    for i in fn.insns.values():
        sh = []
        for o in i.operands:
            t = T.get(o.type, str(o.type))
            if o.type == A.ARM_OP_MEM:
                t += "[" + ("b" if o.mem.base else "") + ("x" if o.mem.index else "") + ("d" if o.mem.disp else "") + ("<<%d" % o.mem.lshift if o.mem.lshift else "") + "]"
            if o.shift.type: t += "s%d:%d" % (o.shift.type, o.shift.value)
            if o.subtracted: t += "-"
            sh.append(t)
        key = (i.mnemonic, " ".join(sh), "wb" if i.writeback else "", "S" if i.update_flags else "")
        if key not in seen: seen[key] = (i, fn.name, 0)
        x = seen[key]; seen[key] = (x[0], x[1], x[2] + 1)
for (m, sh, wb, s), (i, fn, n) in sorted(seen.items()):
    print(f"{m:14} {sh:28} {wb:2} {s:1} {n:5}  {i.address:#07x} {i.mnemonic} {i.op_str}")
