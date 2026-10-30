import re,struct,glob
d=open('game/default.xbe','rb').read()
def rd(va):
    off=va-0x12000+0x2000
    return struct.unpack('<I',d[off:off+4])[0]
known=set(int(m,16) for m in re.findall(r'\{ 0x([0-9A-F]{8})u, \(recomp_func_t\)',open('port/src/recomp/gen/recomp_dispatch.c').read()))
missing={}
for f in glob.glob('port/src/recomp/gen/recomp_*.c'):
    src=open(f).read()
    for m in re.finditer(r'RECOMP_ITAIL\(MEM32\((\w+) \* 4 \+ 0x([0-9A-F]+)\)\)',src):
        tbl=int(m.group(2),16)
        if not (0x12000<=tbl<0x1D1B80): continue
        # function containing this
        fn=src.rfind('void sub_',0,m.start()); name=src[fn+5:fn+17]
        i=0
        while True:
            t=rd(tbl+4*i)
            if not (abs(t-tbl)<0x4000 and 0x12000<=t<0x1D1B80): break
            if t not in known: missing.setdefault(t,set()).add(name)
            i+=1
print(len(missing),'missing targets')
for t in sorted(missing): print(hex(t),sorted(missing[t]))
