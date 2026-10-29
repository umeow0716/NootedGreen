# Rewrite IGA Gen11 assembly text into Gen12 syntax (send forms, removed options).
import re,sys
SFID={0x2:'smpl',0x3:'gtwy',0x7:'ts',0xA:'dc0',0xC:'dc1',0x5:'urb',0x4:'dc2',0x9:'dcro',0x6:'rc',0x8:'pixi',0x1:'null'}
out=[]
for l in open(sys.argv[1]):
    code,_,cmt=l.partition('//')
    m=re.match(r'(\s*(?:\([^)]*\)\s*)?)(sends?)(\s+\(\d+\|M\d+\))\s+(\S+)\s+(\S+)\s+(?:(\S+)\s+)?(0x[0-9A-Fa-f]+|a0\.\d)\s+(0x[0-9A-Fa-f]+|a0\.\d)\s*(\{[^}]*\})?',code)
    if m:
        pre,op,es,dst,s0,s1,ex,desc,opts=m.groups()
        if op=='send': s1,ex,desc=None,s1 if s1 else ex,desc
        if op=='send' and m.group(6): ex,desc=m.group(6),m.group(7)
        exv=int(ex,16); sf=SFID[exv&0xf]; eot=bool(exv&0x20); xl=(exv>>6)&0x1f
        strip=lambda r:r.split(':')[0]
        src1=strip(s1) if s1 else 'null'
        o=[x for x in (opts or '{}').strip('{}').split(',') if x.strip() and x.strip() not in ('NoDDClr','NoDDChk','NoPreempt','Compacted')]
        if eot and 'EOT' not in o: o.append('EOT')
        code=f'{pre}send.{sf}{es} {strip(dst)} {strip(s0)} {src1} {hex(exv&~0x3f)} {desc}' + (' {'+','.join(o)+'}' if o else '')
    else:
        code=re.sub(r'\{([^}]*)\}',lambda mm:('{'+','.join(x for x in mm.group(1).split(',') if x.strip() not in('NoDDClr','NoDDChk','NoPreempt','Compacted'))+'}') if any(x.strip() not in('NoDDClr','NoDDChk','NoPreempt','Compacted') for x in mm.group(1).split(',')) else '',code)
    out.append(code.rstrip()+'\n')
open(sys.argv[2],'w').writelines(out)

# Gen12 has no math.fdiv / math.iqot: expand them (temp r80, flag f1.0).
def operand(o):
    m=re.match(r'(r\d+\.\d+|\d+)(<[^>]*>)?(:\w+)',o)
    return m.groups() if m else None
lines=open(sys.argv[2]).read().splitlines(True); res=[]
for l in lines:
    m=re.match(r'(\s*(?:\(W\)\s*)?)math\.(fdiv|iqot)\s+\((\d+)\|M0\)\s+(\S+)\s+(\S+)\s+(\S+)',l)
    if not m: res.append(l); continue
    pre,op,es,d,s0,s1=m.groups()
    W='(W) ' if '(W)' in pre else ''
    E=f'({es}|M0)'; n=int(es)
    sreg=lambda o:o.split('<')[0].split(':')[0]
    if op=='fdiv':
        if sreg(d)!=sreg(s0):
            res+= [f'{W}math.inv {E} {d} {s1}\n', f'{W}mul {E} {d} {s0} {d.replace("<1>","<8;8,1>" if n>1 else "<0;1,0>")}\n']
        else:
            rg={1:'<0;1,0>',2:'<2;2,1>',4:'<4;4,1>'}.get(n,'<8;8,1>')
            res+= [f'{W}math.inv {E} r80.0<1>:f {s1}\n', f'{W}mul {E} {d} {s0} r80.0{rg}:f\n']
    else:
        rg={1:'<0;1,0>',2:'<2;2,1>',4:'<4;4,1>'}.get(n,'<8;8,1>')
        sty=s0.split(':')[-1]; t=lambda k,ty:f'r{80+k}.0<1>:{ty}'; ts=lambda k,ty:f'r{80+k}.0{rg}:{ty}'
        s1v = s1 if not re.match(r'^\d+:',s1) else s1
        res+=[f'{W}mov {E} {t(0,"f")} {s0}\n', f'{W}mov {E} {t(1,"f")} {s1v}\n',
              f'{W}math.inv {E} {t(1,"f")} {ts(1,"f")}\n', f'{W}mul {E} {t(0,"f")} {ts(0,"f")} {ts(1,"f")}\n',
              f'{W}mov {E} {t(2,"d")} {ts(0,"f")}\n', f'{W}mul {E} {t(3,"d")} {ts(2,"d")} {s1v.replace(":ud",":d")}\n',
              f'{W}add {E} {t(3,"d")} {s0.replace(":ud",":d")} -{ts(3,"d")}\n',
              f'{W}cmp {E} (lt)f1.0 null<1>:d {ts(3,"d")} 0:d\n', f'(f1.0) add {E} {t(2,"d")} {ts(2,"d")} -1:d\n' if not W else f'(W&f1.0) add {E} {t(2,"d")} {ts(2,"d")} -1:d\n',
              f'{W}cmp {E} (ge)f1.0 null<1>:d {ts(3,"d")} {s1v.replace(":ud",":d")}\n', f'(f1.0) add {E} {t(2,"d")} {ts(2,"d")} 1:d\n' if not W else f'(W&f1.0) add {E} {t(2,"d")} {ts(2,"d")} 1:d\n',
              f'{W}mov {E} {d} {ts(2,"d")}\n']
open(sys.argv[2],'w').writelines(res)
