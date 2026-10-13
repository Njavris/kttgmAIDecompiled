#!/usr/bin/env python3
"""
lift.py - static 6502 -> C lifter ("decompiler") for kttgm.nes

Translates the NES ROM's 6502 machine code into compilable C:
  * basic blocks are grouped into C functions: first by strongly
    connected components (so loops stay inside one function and
    branches become goto), then merged along fall-through/jump
    edges of the condensation DAG (so straight-line code doesn't
    explode into one function per block)
  * JSR -> C function call, RTS/RTI -> return
  * cross-region jumps become tail calls; the one 'leaky' jump back
    into the eternal main loop is lowered to setjmp/longjmp, which
    reproduces the 6502 hardware stack-wrap semantics safely
  * hardware spin loops (vblank poll, frame counter poll) are folded
    into runtime hooks
"""
import sys
import dis6502 as D

ROMFILE = sys.argv[1] if len(sys.argv) > 1 else 'kttgm.nes'
OUT     = sys.argv[2] if len(sys.argv) > 2 else 'game.c'
ROM = open(ROMFILE, 'rb').read()
assert ROM[:4] == b'NES\x1a'
PRG = bytearray(ROM[16:16+0x8000])
CHR = bytearray(ROM[16+0x8000:16+0x8000+0x2000])
BASE = 0x8000
NMI, RESET, IRQ = 0x8000, 0x8032, 0x8031

# ------------------------------------------------------------------ 1. disassemble
d = D.Disassembler(PRG, BASE)
for v in (RESET, NMI, IRQ):
    d.mark(v)
for _pc, t in d.rts_trick_scan():
    d.mark(t)
ins = d.ins
assert len(ins) > 1000, "disassembly looks incomplete"
print(f"[lift] {len(ins)} instructions decoded", file=sys.stderr)

# ------------------------------------------------------------------ 2. fold hw spin loops
units = {}          # pc -> unit
folded = []         # (start, length) folded ranges
pc = BASE
while pc < BASE + 0x8000:
    if pc not in ins:
        pc += 1
        continue
    i = ins[pc]
    # BIT $2002 ; BPL <bit>     -> wait for vblank
    if i['mn'] == 'BIT' and i['mode'] == D.ABS and i['operand'] == 0x2002:
        nxt = ins.get(pc + 3)
        if nxt and nxt['mn'] == 'BPL' and nxt['tgt'] == pc:
            units[pc] = dict(pc=pc, mn='WAIT_VBLANK', mode=None, len=5,
                             operand=None, tgt=None, note='BIT $2002 / BPL (vblank spin)')
            folded.append((pc, 5))
            pc += 5
            continue
    # CMP zp ; BEQ <cmp>        -> wait for NMI-modified variable to change
    if i['mn'] == 'CMP' and i['mode'] == D.ZP:
        nxt = ins.get(pc + 2)
        if nxt and nxt['mn'] == 'BEQ' and nxt['tgt'] == pc:
            units[pc] = dict(pc=pc, mn='WAIT_FRAME', mode=None, len=4,
                             operand=i['operand'], tgt=None, note='CMP zp / BEQ (frame spin)')
            folded.append((pc, 4))
            pc += 4
            continue
    units[pc] = dict(pc=pc, mn=i['mn'], mode=i['mode'], len=i['len'],
                     operand=i['operand'], tgt=i['tgt'], note=None)
    pc += i['len']
for f, l in folded:
    print(f"[lift] folded hw sync idiom at ${f:04X} ({l} bytes)", file=sys.stderr)
for u in units.values():
    t = u['tgt']
    if t is None:
        continue
    for f, l in folded:
        if f < t < f + l:
            sys.exit(f"[lift] FATAL: branch into folded idiom: ${u['pc']:04X} -> ${t:04X}")

# ------------------------------------------------------------------ 3. basic blocks
entries = {RESET, NMI, IRQ}          # routine entries (JSR targets + vectors)
for u in units.values():
    if u['mn'] == 'JSR':
        entries.add(u['tgt'])
leaders = set(entries)
for u in units.values():
    if u['mode'] == D.REL or (u['mn'] == 'JMP' and u['mode'] == D.ABS):
        leaders.add(u['tgt'])

blocks = []
cur = None
for a in sorted(units):
    if cur is None or a in leaders:
        cur = dict(start=a, units=[])
        blocks.append(cur)
    cur['units'].append(units[a])
    last = units[a]
    if last['mn'] in ('RTS', 'RTI') or last['mn'] == 'JMP' or last['mode'] == D.REL:
        cur = None
block_by_start = {b['start']: b for b in blocks}
print(f"[lift] {len(blocks)} basic blocks, {len(entries)} routine entries", file=sys.stderr)

def blk_end(b):
    return b['start'] + sum(u['len'] for u in b['units'])

flow = []            # (src, dst, kind): fall | branch | jmp   (region graph edges)
for b in blocks:
    last = b['units'][-1]
    end = blk_end(b)
    if last['mn'] in ('RTS', 'RTI'):
        continue
    if last['mn'] == 'JMP':
        flow.append((b['start'], block_by_start[last['tgt']]['start'], 'jmp'))
        continue
    if last['mode'] == D.REL:
        flow.append((b['start'], block_by_start[last['tgt']]['start'], 'branch'))
        if end in block_by_start:
            flow.append((b['start'], block_by_start[end]['start'], 'fall'))
        continue
    if end in block_by_start:
        flow.append((b['start'], block_by_start[end]['start'], 'fall'))
    # else: falls off end of code region (data follows; last code byte is RTS)

# ------------------------------------------------------------------ 4. Tarjan SCC (iterative)
succ = {}
for s, t, k in flow:
    succ.setdefault(s, []).append(t)
for b in blocks:
    succ.setdefault(b['start'], [])

idx = {}; low = {}; onstk = set(); stk = []; scc_of = {}; sccs = []
ctr = 0
for root in [b['start'] for b in blocks]:
    if root in idx:
        continue
    work = [(root, iter(succ.get(root, ())))]
    while work:
        v, it = work[-1]
        if v not in idx:
            idx[v] = low[v] = ctr; ctr += 1
            stk.append(v); onstk.add(v)
        advanced = False
        for w in it:
            if w not in idx:
                work.append((w, iter(succ.get(w, ()))))
                advanced = True
                break
            elif w in onstk:
                if idx[w] < low[v]:
                    low[v] = idx[w]
        if advanced:
            continue
        work.pop()
        if work:
            p = work[-1][0]
            if low[v] < low[p]:
                low[p] = low[v]
        if low[v] == idx[v]:
            comp = []
            while True:
                x = stk.pop(); onstk.discard(x); comp.append(x); scc_of[x] = len(sccs)
                if x == v:
                    break
            sccs.append(comp)

# ------------------------------------------------------------------ 5. returns analysis on condensation
def scc_has_rts(i):
    for a in sccs[i]:
        for u in block_by_start[a]['units']:
            if u['mn'] in ('RTS', 'RTI'):
                return True
    return False
has_rts = {i: scc_has_rts(i) for i in range(len(sccs))}

# edge X->Y, Y returns  =>  X returns (control tail-jumps into Y, Y's RTS unwinds X's C frame)
pred_scc = {}
for s, t, k in flow:
    if scc_of[s] != scc_of[t]:
        pred_scc.setdefault(scc_of[t], set()).add(scc_of[s])
returns_ = set(i for i in range(len(sccs)) if has_rts[i])
frontier = set(returns_)
while frontier:
    nf = set()
    for i in frontier:
        for p in pred_scc.get(i, ()):
            if p not in returns_:
                returns_.add(p); nf.add(p)
    frontier = nf
nr_scc = set(i for i in range(len(sccs)) if i not in returns_)
print(f"[lift] {len(sccs)} SCCs; never-returning: "
      f"{sorted('$%04X' % min(sccs[i]) for i in nr_scc)}", file=sys.stderr)

# ------------------------------------------------------------------ 6. merge SCCs into regions
# merge along fall/jmp cross edges whose target is not never-returning.
# merging along an edge of the condensation DAG cannot create a cycle.
uf = {}
def find(x):
    while uf.get(x, x) != x:
        uf[x] = uf.get(uf[x], uf[x])
        x = uf[x]
    return x
def union(a, b):
    ra, rb = find(a), find(b)
    if ra != rb:
        uf[ra] = rb

for s, t, k in flow:
    if scc_of[s] == scc_of[t]:
        continue
    if k in ('fall', 'jmp') and scc_of[t] not in nr_scc:
        union(scc_of[s], scc_of[t])

region_of_scc = {}
regions = []
for i in range(len(sccs)):
    r = find(i)
    if r not in region_of_scc:
        region_of_scc[r] = len(regions)
        regions.append([])
    reg = region_of_scc[r]
    regions[reg].append(i)
    region_of_scc[i] = reg

def region_blocks(ri):
    return sorted(min(sccs[i]) for i in regions[ri]) if False else \
           sorted(a for i in regions[ri] for a in sccs[i])
def region_of_block(a):
    return region_of_scc[scc_of[a]]
region_nr = {ri: all(i in nr_scc for i in regions[ri]) for ri in range(len(regions))}
region_returns = {ri: any(i in returns_ for i in regions[ri]) for ri in range(len(regions))}
assert all(region_nr[ri] == (not region_returns[ri]) for ri in range(len(regions)))

# inter-region edges
rflow = [(s, t, k) for (s, t, k) in flow if region_of_block(s) != region_of_block(t)]
region_entries = {ri: set() for ri in range(len(regions))}
for e in entries:
    region_entries[region_of_block(e)].add(e)
for s, t, k in rflow:
    region_entries[region_of_block(t)].add(t)
# NR regions entered via branch/jmp need a setjmp context so callers can
# longjmp in; NR regions entered only via fall are plain calls (the C frames
# get unwound by the eventual longjmp into the eternal loop anyway)
nr_with_entry = [ri for ri in range(len(regions))
                 if region_nr[ri] and any(region_of_block(t) == ri
                                          for (s, t, k) in rflow if k != 'fall')]
print(f"[lift] {len(regions)} C functions; longjmp entry regions: "
      f"{sorted('$%04X' % min(region_blocks(ri)) for ri in nr_with_entry)}", file=sys.stderr)

# JSR into a never-returning region would leak one C frame per call: warn
for u in units.values():
    if u['mn'] == 'JSR' and region_of_block(ins[u['pc']]['tgt']) in \
       [ri for ri in range(len(regions)) if region_nr[ri]]:
        print(f"[lift] WARNING: JSR ${ins[u['pc']]['tgt']:04X} never returns (C frame leaks per call)",
              file=sys.stderr)

# ------------------------------------------------------------------ 7. C emission
def fname(ri): return f"f_{min(region_blocks(ri)):04X}"
def multi(ri): return len(region_entries[ri]) > 1
def entry_index(ri, a): return sorted(region_entries[ri]).index(a)
BRCOND = {'BPL': '!fN', 'BMI': 'fN', 'BVC': '!fV', 'BVS': 'fV',
          'BCC': '!fC', 'BCS': 'fC', 'BNE': '!fZ', 'BEQ': 'fZ'}

def addr_expr(mode, o):
    if mode == D.ZP:  return f"0x{o:02X}"
    if mode == D.ZPX: return f"(uint8_t)(0x{o:02X} + X)"
    if mode == D.ZPY: return f"(uint8_t)(0x{o:02X} + Y)"
    if mode == D.IZX: return f"op_ad_izx(0x{o:02X})"
    if mode == D.IZY: return f"op_ad_izy(0x{o:02X})"
    if mode == D.ABS: return f"0x{o:04X}"
    if mode == D.ABX: return f"(uint16_t)(0x{o:04X} + X)"
    if mode == D.ABY: return f"(uint16_t)(0x{o:04X} + Y)"
    raise Exception("bad mode")

def emit_ins(u):
    mn, mode, o = u['mn'], u['mode'], u['operand']
    need_val = mode in (D.IMM, D.ZP, D.ZPX, D.ZPY, D.IZX, D.IZY, D.ABS, D.ABX, D.ABY)
    v = f"0x{o:02X}" if (need_val and mode == D.IMM) else (
        f"rd({addr_expr(mode, o)})" if need_val else None)
    a = addr_expr(mode, o) if mode in (D.ZP, D.ZPX, D.ZPY, D.IZX, D.IZY, D.ABS, D.ABX, D.ABY) else None
    if mn == 'LDA': return f"A = {v}; op_setNZ(A);"
    if mn == 'LDX': return f"X = {v}; op_setNZ(X);"
    if mn == 'LDY': return f"Y = {v}; op_setNZ(Y);"
    if mn == 'STA': return f"wr({a}, A);"
    if mn == 'STX': return f"wr({a}, X);"
    if mn == 'STY': return f"wr({a}, Y);"
    if mn == 'ORA': return f"A |= {v}; op_setNZ(A);"
    if mn == 'AND': return f"A &= {v}; op_setNZ(A);"
    if mn == 'EOR': return f"A ^= {v}; op_setNZ(A);"
    if mn == 'ADC': return f"op_adc({v});"
    if mn == 'SBC': return f"op_sbc({v});"
    if mn == 'CMP': return f"op_cmp(A, {v});"
    if mn == 'CPX': return f"op_cmp(X, {v});"
    if mn == 'CPY': return f"op_cmp(Y, {v});"
    if mn == 'BIT': return f"op_bit({v});"
    if mn == 'ASL': return ("op_asl_a();" if mode == D.ACC else f"op_asl({a});")
    if mn == 'LSR': return ("op_lsr_a();" if mode == D.ACC else f"op_lsr({a});")
    if mn == 'ROL': return ("op_rol_a();" if mode == D.ACC else f"op_rol({a});")
    if mn == 'ROR': return ("op_ror_a();" if mode == D.ACC else f"op_ror({a});")
    if mn == 'INC': return f"op_inc({a});"
    if mn == 'DEC': return f"op_dec({a});"
    if mn == 'INX': return "X++; op_setNZ(X);"
    if mn == 'INY': return "Y++; op_setNZ(Y);"
    if mn == 'DEX': return "X--; op_setNZ(X);"
    if mn == 'DEY': return "Y--; op_setNZ(Y);"
    if mn == 'TAX': return "X = A; op_setNZ(X);"
    if mn == 'TAY': return "Y = A; op_setNZ(Y);"
    if mn == 'TXA': return "A = X; op_setNZ(A);"
    if mn == 'TYA': return "A = Y; op_setNZ(A);"
    if mn == 'TSX': return "X = S; op_setNZ(X);"
    if mn == 'TXS': return "S = X;"
    if mn == 'PHA': return "op_pha();"
    if mn == 'PHP': return "op_php();"
    if mn == 'PLA': return "op_pla();"
    if mn == 'PLP': return "op_plp();"
    if mn == 'CLC': return "fC = 0;"
    if mn == 'SEC': return "fC = 1;"
    if mn == 'CLI': return "fI = 0;"
    if mn == 'SEI': return "fI = 1;"
    if mn == 'CLD': return "fD = 0;"
    if mn == 'SED': return "fD = 1;"
    if mn == 'CLV': return "fV = 0;"
    if mn == 'NOP': return "/* nop */"
    if mn == 'WAIT_VBLANK': return "hook_wait_vblank();"
    if mn == 'WAIT_FRAME':  return f"hook_wait_frame(0x{o:02X});"
    if mn == 'RTS': return "return;"
    if mn == 'RTI': return "return;"
    if mn == 'JSR':
        tgt = ins[u['pc']]['tgt']
        ti = region_of_block(tgt)
        return f"{fname(ti)}({entry_index(ti, tgt)});" if multi(ti) else f"{fname(ti)}();"
    raise Exception(f"unhandled {mn}")

def transfer(src_ri, dst_addr):
    """Code that leaves the current C function for control at dst_addr."""
    ti = region_of_block(dst_addr)
    if ti == src_ri:
        return [f"goto L_{dst_addr:04X};"], True
    k = entry_index(ti, dst_addr)
    arg = f"{k}" if multi(ti) else ""
    if ti in nrmap:
        j = nrmap[ti]
        return [f"if (!g_nr_init[{j}]) {fname(ti)}({arg});",
                f"g_nr_entry[{j}] = {k};",
                f"longjmp(g_nr_jmp[{j}], 1);"], False
    return [f"{fname(ti)}({arg});", "return;"], False

def asm_note(u):
    if u['note']:
        return u['note']
    return D.fmt(ins[u['pc']])

out = []
w = out.append
w("/* ------------------------------------------------------------------")
w(" * game.c - 6502 machine code of kttgm.nes statically lifted to C")
w(" * generated by lift.py - DO NOT EDIT (regenerate instead)")
w(" *")
w(" * One C statement per original 6502 instruction (see the comment on")
w(" * each line for the original code).  A/X/Y/S and the flags are CPU")
w(" * globals; every memory access goes through rd()/wr(), which")
w(" * implement the NES bus: RAM, PPU registers, APU registers and ROM")
w(" * data.  Hardware spin loops were folded into hook_wait_vblank()/")
w(" * hook_wait_frame(), and the jump back into the main loop uses")
w(" * setjmp/longjmp to reproduce the 6502 stack-wrap behaviour.")
w(" * ------------------------------------------------------------------ */")
w("")
w('#include "runtime.h"')
w('#include <setjmp.h>')
w("")
w("const uint8_t PRG_ROM[0x8000] = { /* original 32 KiB PRG bank */")
for off in range(0, 0x8000, 16):
    w("    " + " ".join(f"0x{b:02X}," for b in PRG[off:off+16]) + f" /* ${BASE+off:04X} */")
w("};")
w("const uint8_t CHR_ROM[0x2000] = { /* original 8 KiB CHR bank */")
for off in range(0, 0x2000, 16):
    w("    " + " ".join(f"0x{b:02X}," for b in CHR[off:off+16]) + f" /* ${off:04X} */")
w("};")
w("")
if nr_with_entry:
    w("/* never-returning loop contexts: the 6502 just abandons the return")
    w(" * address (hardware stack wraps); in C we longjmp back in */")
    w(f"static jmp_buf g_nr_jmp[{len(nr_with_entry)}];")
    w(f"static unsigned g_nr_entry[{len(nr_with_entry)}];")
    w(f"static uint8_t g_nr_init[{len(nr_with_entry)}];")
    w("")
nrmap = {ri: n for n, ri in enumerate(nr_with_entry)}
w("/* lifted subroutines */")
for ri in sorted(range(len(regions)), key=lambda ri: min(region_blocks(ri))):
    w(f"static void {fname(ri)}({'unsigned entry' if multi(ri) else 'void'});")
w("")

for ri in sorted(range(len(regions)), key=lambda ri: min(region_blocks(ri))):
    rblocks = [block_by_start[a] for a in region_blocks(ri)]
    m = multi(ri)
    is_entry_routine = bool(entries & set(region_blocks(ri)))
    hdr = (f"/* {'subroutine' if is_entry_routine else 'code region'} "
           f"${region_blocks(ri)[0]:04X}-${blk_end(rblocks[-1])-1:04X}")
    if m:
        hdr += ", entries: " + " ".join(f"${e:04X}" for e in sorted(region_entries[ri]))
    hdr += " */"
    w(hdr)
    w(f"static void {fname(ri)}({'unsigned entry' if m else 'void'}) {{")
    if ri in nrmap:
        j = nrmap[ri]
        w(f"    g_nr_init[{j}] = 1;")
        if m:
            w(f"    if (setjmp(g_nr_jmp[{j}]) != 0) entry = g_nr_entry[{j}];")
        else:
            e0 = min(region_entries[ri]) if region_entries[ri] else None
            if e0 is not None and e0 != region_blocks(ri)[0]:
                w(f"    if (setjmp(g_nr_jmp[{j}]) != 0) goto L_{e0:04X};")
            else:
                w(f"    (void)setjmp(g_nr_jmp[{j}]);")
    if m:
        w("    switch (entry) {")
        for k, e in enumerate(sorted(region_entries[ri])):
            w(f"    case {k}: goto L_{e:04X};")
        w("    }")
    else:
        e0 = min(region_entries[ri]) if region_entries[ri] else None
        if e0 is not None and e0 != region_blocks(ri)[0] and ri not in nrmap:
            w(f"    goto L_{e0:04X};")
    need_label = set(region_entries[ri])
    for s, t, kind in flow:
        if region_of_block(s) == ri and region_of_block(t) == ri and kind in ('branch', 'jmp'):
            need_label.add(t)
    for b in rblocks:
        if b['start'] in need_label:
            w(f"  L_{b['start']:04X}:")
        for u in b['units']:
            if u['mode'] == D.REL or u['mn'] == 'JMP':
                continue          # block terminator, emitted by the block-end logic below
            code = emit_ins(u)
            w(f"    {code:<52s} /* {asm_note(u)} */")
        last = b['units'][-1]
        if last['mn'] in ('RTS', 'RTI', 'WAIT_VBLANK', 'WAIT_FRAME'):
            continue
        end = blk_end(b)
        if last['mn'] == 'JMP':
            lines, _ = transfer(ri, ins[last['pc']]['tgt'])
            for L in lines:
                w(f"    {L}")
            continue
        if last['mode'] == D.REL:
            cond = BRCOND[last['mn']]
            tgt = ins[last['pc']]['tgt']
            tl, internal = transfer(ri, tgt)
            if internal:
                w(f"    if ({cond}) {tl[0]}")
            else:
                w(f"    if ({cond}) {{")
                for L in tl:
                    w(f"        {L}")
                w("    }")
            fb = block_by_start.get(end)
            if fb is not None and region_of_block(end) != ri:
                fl, _ = transfer(ri, end)
                for L in fl:
                    w(f"    {L}")
            continue
        fb = block_by_start.get(end)
        if fb is None:
            w("    return; /* fell off end of code region */")
        elif region_of_block(end) != ri:
            fl, _ = transfer(ri, end)
            for L in fl:
                w(f"    {L}")
    w("}")
    w("")

w("/* interrupt vectors (NMI=$8000 RESET=$8032 IRQ=$8031) */")
for nm, vec in (("nes_nmi", NMI), ("nes_reset", RESET), ("nes_irq", IRQ)):
    ri = region_of_block(vec)
    k = entry_index(ri, vec)
    arg = f"{k}" if multi(ri) else ""
    w(f"void {nm}(void) {{ {fname(ri)}({arg}); }}")
w("")
open(OUT, "w").write("\n".join(out) + "\n")
print(f"[lift] wrote {OUT}: {len(out)} lines, {len(regions)} functions", file=sys.stderr)
