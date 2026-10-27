# Replays a Gyro4All motion-trace.csv through a Python copy of the 0.6.17 pacer (jitter buffer) and compares
# smoothness (speed CV over 12 ms slots) with the raw and recorded output. Usage: python -I tools/sim_pacer.py <trace>
import csv, math, statistics, sys
from collections import deque
S=[];F=[]
for r in csv.reader(open(sys.argv[1])):
    if r[0]=="S": S.append(math.sqrt(sum(float(x)**2 for x in r[3:6]))+1e-3)
    elif r[0]=="F": F.append((int(r[1]),int(r[2]),float(r[3]),float(r[5])))
def jitter(max_delay=0.05, k=0.5):
    q=deque(); used=0.0; rate=0; rs=0; rc=0; gm=0; gv=0; fm=0; lf=0; la=0; si=0; out=[]
    for now,n,raw,_ in F:
        dt=(now-lf)/1e9 if lf else 0; lf=now
        if 0<dt<0.2: fm = fm+(dt-fm)*0.05 if fm else dt
        w=S[si:si+n]; si+=n
        if n:
            if la:
                g=(now-la)/1e9
                if g<0.2:
                    if not gm: gm=g
                    d=g-gm; gm+=d*0.05; gv+=(d*d-gv)*0.05
            la=now
            if not rs: rs=now
            rc+=n; span=(now-rs)/1e9
            if span>=1: r=rc/span; rate = rate+(r-rate)*0.3 if rate else r; rs=now; rc=0
        target=min(gm+k*math.sqrt(gv),max_delay)
        active= rate>0 and dt>0 and fm>0 and gm>fm*1.5
        o=0.0
        def take(c):
            nonlocal used,o
            while c>1e-9 and q:
                t=min(c,1-used); o+=q[0]*t; used+=t; c-=t
                if used>=1-1e-9: q.popleft(); used=0
        if not active:
            take(len(q)+1); q.clear(); used=0; o+=raw; out.append((now,o)); continue
        if n:
            s=sum(w); [q.append(raw*x/s) for x in w]
        else: o+=raw
        want=target*rate; depth=len(q)-used
        sp=max(0.5,min(2.0,1+0.5*(depth-want)/max(want,1)))
        take(rate*dt*sp)
        lim=max_delay*rate*1.5
        if len(q)-used>lim: take(len(q)-used-lim)
        out.append((now,o)); DELAY.append((len(q)-used)/rate*1000 if active and rate else 0)
    return out
def metrics(series):
    sp=[]
    for (t0,_),(t1,v) in zip(series,series[1:]):
        dt=(t1-t0)/1e9
        if 0<dt<0.1: sp.append((t1,abs(v)/dt))
    # per render-frame-ish: aggregate into 12 ms bins to avoid the double calls skewing
    bins={}
    for t,v in sp: bins.setdefault(t//12_000_000,[]).append(v)
    bs=[statistics.mean(v) for _,v in sorted(bins.items())]
    cvs=[]
    for i in range(0,len(bs)-8,8):
        w=bs[i:i+8]; m=statistics.mean(w)
        if m>10: cvs.append(statistics.pstdev(w)/m)
    zero=sum(1 for v in bs if v==0)/len(bs)
    return statistics.mean(cvs), zero
DELAY=[]
raw=[(t,r) for t,_,r,_ in F]; old=[(t,a) for t,_,_,a in F]; new=jitter()
for name,s in (("raw (no pacing)",raw),("0.6.16 pacing (recorded)",old),("0.6.17 jitter buffer (simulated)",new)):
    cv,z=metrics(s); print(f"{name:34s} speed CV {cv:.2f}  empty 12ms-slots {z:.0%}")
d=[x for x in DELAY if x>0]
if d: print(f"0.6.17 queue delay: median {statistics.median(d):.1f} ms, p90 {sorted(d)[int(.9*len(d))]:.1f} ms")
