"""Probe functions: each is a leaf PowerPC routine using the EABI (args r3.., f1..; result r3/f1)."""
import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppcasm import Asm
def sum_words(base):           # r3=ptr, r4=count -> r3 = sum of big-endian u32
    a=Asm(base); a.li(5,0); a.mtctr(4); a.addi(3,3,-4); a.L('l'); a.lwzu(6,4,3); a.add(5,5,6); a.bdnz('l'); a.addi(3,5,0); a.blr(); return a
def dot_f32(base):             # r3=x*, r4=y*, r5=n -> f1 = sum x[i]*y[i] (fmadds, single precision)
    a=Asm(base); a.lis(6,0); a.stwu(6,-16,1); a.lfs(1,8,1); a.addi(1,1,16)  # f1 = 0.0 via stack (stw 0 / lfs)
    a.mtctr(5); a.addi(3,3,-4); a.addi(4,4,-4); a.L('l'); a.lfsu(2,4,3); a.lfsu(3,4,4); a.fmadds(1,2,3,1); a.bdnz('l'); a.blr(); return a
def hash_loop(base):           # r3=seed, r4=iterations -> r3 = xorshift/multiply mix (integer ALU bound)
    a=Asm(base); a.lis(7,0x9E37); a.ori(7,7,0x79B9); a.mtctr(4); a.L('l')
    a.slwi(5,3,13); a.xor(3,3,5); a.srwi(5,3,17); a.xor(3,3,5); a.slwi(5,3,5); a.xor(3,3,5); a.mullw(3,3,7); a.bdnz('l'); a.blr(); return a
def div_edge(base):            # r3=a, r4=b -> r3 = a/b (divw: PowerPC leaves undefined results; Broadway gives defined values)
    a=Asm(base); a.divw(3,3,4); a.blr(); return a
def memcpy_words(base):        # r3=dst, r4=src, r5=count words
    a=Asm(base); a.mtctr(5); a.addi(3,3,-4); a.addi(4,4,-4); a.L('l'); a.lwzu(6,4,4); a.stwu(6,4,3); a.bdnz('l'); a.blr(); return a
def ps_loop(base):             # r3=x*, r4=y*, r5=n, f1/f3 accumulators preset by caller: paired-single madd/add/mul/sub
    a=Asm(base); a.mtctr(5); a.addi(3,3,-4); a.addi(4,4,-4); a.L('l'); a.lfsu(2,4,3); a.lfsu(4,4,4)
    a.ps_madd(1,2,4,1); a.ps_mul(5,2,4); a.ps_add(3,3,5); a.ps_sub(3,3,2); a.bdnz('l'); a.blr(); return a
def fctiwz_one(base):          # r3=&double, r4=&out(8 bytes): out = fctiwz(*r3) (result in the low word; stfiwx is not in the translator)
    a=Asm(base); a.lfd(1,0,3); a.fctiwz(2,1); a.stfd(2,0,4); a.blr(); return a
PROBES={'ps_loop':(0x80004500,ps_loop),'fctiwz_one':(0x80004600,fctiwz_one),'sum_words':(0x80004000,sum_words),'dot_f32':(0x80004100,dot_f32),'hash_loop':(0x80004200,hash_loop),'div_edge':(0x80004300,div_edge),'memcpy_words':(0x80004400,memcpy_words)}
if __name__=='__main__':
    out=sys.argv[1]
    for n,(base,f) in PROBES.items(): open(f'{out}/{n}.bin','wb').write(f(base).bytes()); print(n,hex(base))
