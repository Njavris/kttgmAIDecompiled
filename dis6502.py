#!/usr/bin/env python3
"""6502 recursive-descent disassembler for iNES ROMs (NROM focus)."""
import sys, json

# addressing modes
IMP, IMM, ZP, ZPX, ZPY, IZX, IZY, ABS, ABX, ABY, IND, REL, ACC = range(13)
MODELEN = {IMP:1, ACC:1, IMM:2, ZP:2, ZPX:2, ZPY:2, IZX:2, IZY:2, ABS:3, ABX:3, ABY:3, IND:3, REL:2}

# opcode table: op -> (mnemonic, mode)
OPS = {
0x00:("BRK",IMP), 0x01:("ORA",IZX), 0x05:("ORA",ZP),  0x06:("ASL",ZP),  0x08:("PHP",IMP),
0x09:("ORA",IMM), 0x0A:("ASL",ACC), 0x0D:("ORA",ABS), 0x0E:("ASL",ABS),
0x10:("BPL",REL), 0x11:("ORA",IZY), 0x15:("ORA",ZPX), 0x16:("ASL",ZPX), 0x18:("CLC",IMP),
0x19:("ORA",ABY), 0x1D:("ORA",ABX), 0x1E:("ASL",ABX),
0x20:("JSR",ABS), 0x21:("AND",IZX), 0x24:("BIT",ZP),  0x25:("AND",ZP),  0x26:("ROL",ZP),
0x28:("PLP",IMP), 0x29:("AND",IMM), 0x2A:("ROL",ACC), 0x2C:("BIT",ABS), 0x2D:("AND",ABS),
0x2E:("ROL",ABS),
0x30:("BMI",REL), 0x31:("AND",IZY), 0x35:("AND",ZPX), 0x36:("ROL",ZPX), 0x38:("SEC",IMP),
0x39:("AND",ABY), 0x3D:("AND",ABX), 0x3E:("ROL",ABX),
0x40:("RTI",IMP), 0x41:("EOR",IZX), 0x45:("EOR",ZP),  0x46:("LSR",ZP),  0x48:("PHA",IMP),
0x49:("EOR",IMM), 0x4A:("LSR",ACC), 0x4C:("JMP",ABS), 0x4D:("EOR",ABS), 0x4E:("LSR",ABS),
0x50:("BVC",REL), 0x51:("EOR",IZY), 0x55:("EOR",ZPX), 0x56:("LSR",ZPX), 0x58:("CLI",IMP),
0x59:("EOR",ABY), 0x5D:("EOR",ABX), 0x5E:("LSR",ABX),
0x60:("RTS",IMP), 0x61:("ADC",IZX), 0x65:("ADC",ZP),  0x66:("ROR",ZP),  0x68:("PLA",IMP),
0x69:("ADC",IMM), 0x6A:("ROR",ACC), 0x6C:("JMP",IND), 0x6D:("ADC",ABS), 0x6E:("ROR",ABS),
0x70:("BVS",REL), 0x71:("ADC",IZY), 0x75:("ADC",ZPX), 0x76:("ROR",ZPX), 0x78:("SEI",IMP),
0x79:("ADC",ABY), 0x7D:("ADC",ABX), 0x7E:("ROR",ABX),
0x81:("STA",IZX), 0x84:("STY",ZP),  0x85:("STA",ZP),  0x86:("STX",ZP),  0x88:("DEY",IMP),
0x8A:("TXA",IMP), 0x8C:("STY",ABS), 0x8D:("STA",ABS), 0x8E:("STX",ABS),
0x90:("BCC",REL), 0x91:("STA",IZY), 0x94:("STY",ZPX), 0x95:("STA",ZPX), 0x96:("STX",ZPY),
0x98:("TYA",IMP), 0x99:("STA",ABY), 0x9A:("TXS",IMP), 0x9D:("STA",ABX),
0xA0:("LDY",IMM), 0xA1:("LDA",IZX), 0xA2:("LDX",IMM), 0xA4:("LDY",ZP),  0xA5:("LDA",ZP),
0xA6:("LDX",ZP),  0xA8:("TAY",IMP), 0xA9:("LDA",IMM), 0xAA:("TAX",IMP), 0xAC:("LDY",ABS),
0xAD:("LDA",ABS), 0xAE:("LDX",ABS),
0xB0:("BCS",REL), 0xB1:("LDA",IZY), 0xB4:("LDY",ZPX), 0xB5:("LDA",ZPX), 0xB6:("LDX",ZPY),
0xB8:("CLV",IMP), 0xB9:("LDA",ABY), 0xBA:("TSX",IMP), 0xBC:("LDY",ABX), 0xBD:("LDA",ABX),
0xBE:("LDX",ABY),
0xC0:("CPY",IMM), 0xC1:("CMP",IZX), 0xC4:("CPY",ZP),  0xC5:("CMP",ZP),  0xC6:("DEC",ZP),
0xC8:("INY",IMP), 0xC9:("CMP",IMM), 0xCA:("DEX",IMP), 0xCC:("CPY",ABS), 0xCD:("CMP",ABS),
0xCE:("DEC",ABS),
0xD0:("BNE",REL), 0xD1:("CMP",IZY), 0xD5:("CMP",ZPX), 0xD6:("DEC",ZPX), 0xD8:("CLD",IMP),
0xD9:("CMP",ABY), 0xDD:("CMP",ABX), 0xDE:("DEC",ABX),
0xE0:("CPX",IMM), 0xE1:("SBC",IZX), 0xE4:("CPX",ZP),  0xE5:("SBC",ZP),  0xE6:("INC",ZP),
0xE8:("INX",IMP), 0xE9:("SBC",IMM), 0xEA:("NOP",IMP), 0xEC:("CPX",ABS), 0xED:("SBC",ABS),
0xEE:("INC",ABS),
0xF0:("BEQ",REL), 0xF1:("SBC",IZY), 0xF5:("SBC",ZPX), 0xF6:("INC",ZPX), 0xF8:("SED",IMP),
0xF9:("SBC",ABY), 0xFD:("SBC",ABX), 0xFE:("INC",ABX),
# unofficial / stable illegal opcodes
0x1A:("NOP",IMP), 0x3A:("NOP",IMP), 0x5A:("NOP",IMP), 0x7A:("NOP",IMP), 0xDA:("NOP",IMP), 0xFA:("NOP",IMP),
0x80:("NOP",IMM), 0x82:("NOP",IMM), 0x89:("NOP",IMM), 0xC2:("NOP",IMM), 0xE2:("NOP",IMM),
0x04:("NOP",ZP),  0x44:("NOP",ZP),  0x64:("NOP",ZP),
0x14:("NOP",ZPX), 0x34:("NOP",ZPX), 0x54:("NOP",ZPX), 0x74:("NOP",ZPX), 0xD4:("NOP",ZPX), 0xF4:("NOP",ZPX),
0x0C:("NOP",ABS), 0x1C:("NOP",ABX), 0x3C:("NOP",ABX), 0x5C:("NOP",ABX), 0x7C:("NOP",ABX), 0xDC:("NOP",ABX), 0xFC:("NOP",ABX),
0xA3:("LAX",IZX), 0xA7:("LAX",ZP),  0xAF:("LAX",ABS), 0xB3:("LAX",IZY), 0xB7:("LAX",ZPY), 0xBF:("LAX",ABY),
0x83:("SAX",IZX), 0x87:("SAX",ZP),  0x8F:("SAX",ABS), 0x97:("SAX",ZPY),
0xC7:("DCP",ZP),  0xCF:("DCP",ABS), 0xD7:("DCP",ZPX), 0xDB:("DCP",ABY), 0xDF:("DCP",ABX),
0xC3:("DCP",IZX), 0xD3:("DCP",IZY), 0xCB:("SBX",IMM),
0xE7:("ISB",ZP),  0xEF:("ISB",ABS), 0xF7:("ISB",ZPX), 0xFB:("ISB",ABY), 0xFF:("ISB",ABX),
0xE3:("ISB",IZX), 0xF3:("ISB",IZY), 0xEB:("USBC",IMM),
0x07:("SLO",ZP),  0x0F:("SLO",ABS), 0x17:("SLO",ZPX), 0x1B:("SLO",ABY), 0x1F:("SLO",ABX),
0x03:("SLO",IZX), 0x13:("SLO",IZY),
0x27:("RLA",ZP),  0x2F:("RLA",ABS), 0x37:("RLA",ZPX), 0x3B:("RLA",ABY), 0x3F:("RLA",ABX),
0x23:("RLA",IZX), 0x33:("RLA",IZY),
0x47:("SRE",ZP),  0x4F:("SRE",ABS), 0x57:("SRE",ZPX), 0x5B:("SRE",ABY), 0x5F:("SRE",ABX),
0x43:("SRE",IZX), 0x53:("SRE",IZY),
0x67:("RRA",ZP),  0x6F:("RRA",ABS), 0x77:("RRA",ZPX), 0x7B:("RRA",ABY), 0x7F:("RRA",ABX),
0x63:("RRA",IZX), 0x73:("RRA",IZY),
0x0B:("ANC",IMM), 0x2B:("ANC",IMM), 0x4B:("ALR",IMM), 0x6B:("ARR",IMM), 0xAB:("LXA",IMM), 0x8B:("ANE",IMM),
}

def decode(prg, pc, base=0):
    """Decode one instruction at address pc (prg[0] is at address `base`)."""
    off = pc - base
    op = prg[off]
    if op not in OPS: return None
    mn, mode = OPS[op]
    ln = MODELEN[mode]
    operand = None
    if ln >= 2: operand = prg[off+1]
    if ln == 3: operand |= prg[off+2] << 8
    tgt = None
    if mode == REL:
        off = operand if operand < 0x80 else operand - 0x100
        tgt = (pc + 2 + off) & 0xFFFF
    elif mode == ABS or mode == IND:
        tgt = operand
    return dict(pc=pc, op=op, mn=mn, mode=mode, len=ln, operand=operand, tgt=tgt)

def fmt(ins):
    mn, mode, opnd = ins['mn'], ins['mode'], ins['operand']
    if mode in (IMP, ACC): return mn
    if mode == IMM:  return f"{mn} #${opnd:02X}"
    if mode == ZP:   return f"{mn} ${opnd:02X}"
    if mode == ZPX:  return f"{mn} ${opnd:02X},X"
    if mode == ZPY:  return f"{mn} ${opnd:02X},Y"
    if mode == IZX:  return f"{mn} (${opnd:02X},X)"
    if mode == IZY:  return f"{mn} (${opnd:02X}),Y"
    if mode == ABS:  return f"{mn} ${opnd:04X}"
    if mode == ABX:  return f"{mn} ${opnd:04X},X"
    if mode == ABY:  return f"{mn} ${opnd:04X},Y"
    if mode == IND:  return f"{mn} (${opnd:04X})"
    if mode == REL:  return f"{mn} ${ins['tgt']:04X}"

class Disassembler:
    def __init__(self, prg, base):
        self.prg = prg; self.base = base
        self.top = base + len(prg)
        self.ins = {}      # pc -> ins
        self.code = set()  # marked code addresses
        self.refs = set()  # addresses referenced as branch/jump targets

    def at(self, pc):
        return decode(self.prg, pc, self.base)

    def mark(self, start):
        """Mark code reachable via straight-line flow from start."""
        work = [start]
        while work:
            pc = work.pop()
            while True:
                if not (self.base <= pc < self.top): break
                if pc in self.code: break
                ins = self.at(pc)
                if ins is None:
                    print(f"  ! unknown opcode ${self.prg[pc-self.base]:02X} at ${pc:04X}", file=sys.stderr)
                    self.code.add(pc)  # mark single byte as bad
                    break
                self.code.add(pc)
                self.ins[pc] = ins
                if ins['mode'] == REL:
                    self.refs.add(ins['tgt'])
                    work.append(ins['tgt'])
                    pc += ins['len']
                elif ins['mn'] in ('JMP','JSR') and ins['mode'] == ABS:
                    self.refs.add(ins['tgt'])
                    if ins['mn'] == 'JSR':
                        work.append(ins['tgt'])   # subroutine entry
                        pc += ins['len']           # execution continues after JSR
                    else:
                        work.append(ins['tgt'])
                        break   # stop straight-line
                elif ins['mode'] == IND:
                    # indirect jump: try to resolve pointer table
                    self.resolve_indirect(ins['operand'], work)
                    break
                elif ins['mn'] in ('RTS','RTI','BRK'):
                    break
                else:
                    pc += ins['len']
        # separate pass done

    def resolve_indirect(self, ptr_addr, work):
        """JMP ($ptr): if the pointer table lies in PRG, follow entries."""
        ptrs = []
        pc = ptr_addr
        while True:
            off = pc - self.base
            if off < 0 or off + 1 >= len(self.prg): break
            val = self.prg[off] | (self.prg[off+1] << 8)
            if not (self.base <= val < self.top): break
            ins0 = decode(self.prg, val, self.base)
            if ins0 is None: break
            ptrs.append(val)
            pc += 2
        if len(ptrs) >= 2:
            print(f"  ; JMP (${ptr_addr:04X}) -> {len(ptrs)} entries", file=sys.stderr)
            for p in ptrs:
                self.refs.add(p)
                work.append(p)
        else:
            print(f"  ! unresolved JMP (${ptr_addr:04X}) at ${pc:04X}", file=sys.stderr)

    def rts_trick_scan(self):
        """Find 'push address; RTS' dispatch patterns (PHA PHA RTS etc.)."""
        found = []
        pcs = sorted(self.ins)
        for i, pc in enumerate(pcs):
            ins = self.ins[pc]
            if ins['mn'] != 'RTS': continue
            # look back up to 8 instructions for LDA #hi ... PHA ... LDA #lo ... PHA
            seq = []
            j = i - 1
            cur = pc
            while j >= 0 and len(seq) < 10:
                p = pcs[j]
                if p + self.ins[p]['len'] != cur: break  # not contiguous
                seq.append(self.ins[p]); cur = p; j -= 1
            pushed = []
            k = 0
            while k + 1 < len(seq):
                a, b = seq[k], seq[k+1]
                if a['mn'] == 'LDA' and a['mode'] == IMM and b['mn'] == 'PHA':
                    pushed.append(a['operand']); k += 2
                elif a['mn'] == 'LDA' and a['mode'] == IMM and b['mn'] == 'TAX' and k+2 < len(seq) and seq[k+2]['mn'] == 'PHA':
                    pushed.append(a['operand']); k += 3
                elif a['mn'] == 'LDA' and a['mode'] == ABX and b['mn'] == 'PHA' and a['operand'] < 0xBF00:
                    pushed.append(('tab', a['operand'])); k += 2
                else:
                    break
            if len(pushed) >= 2:
                hi = pushed[-1]; lo = pushed[-2]
                if isinstance(hi, int) and isinstance(lo, int):
                    tgt = (hi << 8) | lo
                    if self.base <= tgt < self.top:
                        found.append((pc, tgt))
                        self.refs.add(tgt)
        return found

def main():
    rom = open(sys.argv[1], 'rb').read()
    assert rom[:4] == b'NES\x1a'
    prg_banks = rom[4]; chr_banks = rom[5]
    prg = rom[16:16+prg_banks*0x4000]
    chr_ = rom[16+prg_banks*0x4000:16+prg_banks*0x4000+chr_banks*0x2000]
    base = 0x8000 if prg_banks == 2 else 0xC000
    d = Disassembler(prg, base)
    nmi = prg[-6] | prg[-5]<<8
    rst = prg[-4] | prg[-3]<<8
    irq = prg[-2] | prg[-1]<<8
    print(f"NMI=${nmi:04X} RESET=${rst:04X} IRQ=${irq:04X}", file=sys.stderr)
    for name, v in (('reset', rst), ('nmi', nmi), ('irq', irq)):
        if base <= v < base+len(prg):
            d.mark(v)
    tricks = d.rts_trick_scan()
    # mark code reachable from trick targets
    for _, t in tricks: d.mark(t)
    # write listing
    out = []
    a = base
    while a < base + len(prg):
        if a in d.ins:
            ins = d.ins[a]
            label = ""
            if a in d.refs: label = f"L_{a:04X}:\n"
            out.append(f"{label}{'':8s}{a:04X}:  {ins['op']:02X} " +
                       " ".join(f"{b:02X}" for b in prg[a-base+1:a-base+ins['len']]).ljust(8) +
                       f"{fmt(ins):<32s}")
            a += ins['len']
        else:
            out.append(f"{'':9s}{a:04X}:  {prg[a-base]:02X}         .byte ${prg[a-base]:02X}")
            a += 1
    open(sys.argv[2] if len(sys.argv)>2 else 'out.asm','w').write("\n".join(out)+"\n")
    # dump info json
    info = dict(nmi=nmi, reset=rst, irq=irq, base=base, prg_len=len(prg), chr_len=len(chr_))
    open(sys.argv[3] if len(sys.argv)>3 else 'info.json','w').write(json.dumps(info))
    open('chr.bin','wb').write(chr_)
    print(f"code bytes: {len(d.ins)} / {len(prg)}", file=sys.stderr)
    print(f"routine entries (JSR/vector targets): see listing", file=sys.stderr)

if __name__ == '__main__':
    main()
