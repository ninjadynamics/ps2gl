#!/usr/bin/env python3
"""Compare complete scheduled X2/X2F packets in a float32 dependency model.

Models simultaneous upper/lower reads, masked register fields, triangle order,
clip/fan output, both XTOP halves and output arena capacity. Not a native VU
arithmetic/timing or GS pixel emulator. Culling is OFF, as required by callers.
"""
import json
import re
from pathlib import Path
import numpy as np
ROOT = Path(__file__).resolve().parents[1]
F = np.float32
LANES = 'xyzw'

def reg(s):
    assert re.search(r'\d+', s), repr(s)
    return int(re.search(r'\d+', s)[0])

def signed(v):
    return v - 65536 if v & 32768 else v

class Program:
    def __init__(self, path):
        self.code, self.labels = [], {}
        for raw in path.read_text().splitlines():
            line = raw.split(';', 1)[0]
            if not line.strip(): continue
            if line.strip().endswith(':'):
                self.labels[line.strip()[:-1]] = len(self.code)
            elif not line.lstrip().startswith('.'):
                pair = []
                for text in (line[:67].strip(), line[67:].strip()):
                    words = text.split(None, 1)
                    op = words[0].split('.')
                    pair.append((op[0].lower(), op[1] if len(op)>1 else LANES,
                        words[1].split(',') if len(words)>1 else []))
                upper, lower = pair
                if lower[0] in ('lq','move','mfir','mr32') and upper[0] != 'clipw' and upper[2] and upper[2][0].startswith('VF'):
                    assert upper[2][0] != lower[2][0], ('same-pair VF writers', len(self.code), pair)
                self.code.append(pair)

    def run(self, memory, top, forbid_inside=False, wide=False):
        vf = np.zeros((32,4),dtype=F);vf[0,3]=1
        vi = np.zeros(16,dtype=np.int64)
        acc=np.zeros(4,dtype=F);imm=F(0);q=F(0);clip=0
        pc=0;pending=None;end_delay=None;stops=0;steps=0;packets=[];stores={}
        in_flight=None;self.kicks=0;self.max_packet=0;self.max_arenas=[0,0]
        while steps<150000:
            if forbid_inside:
                assert pc != self.labels.get("xf_inside_triangle_lid"), "cull-on skipped triangle classification"
            ov=vf.copy(); oi=vi.copy(); oa=acc.copy(); oq=q; oldimm=imm
            writes=[]; branch=None
            for op,mask,args in self.code[pc]:
                if '[e]' in op:
                    op=op.replace('[e]','');end_delay=pc+1
                m=[LANES.index(c) for c in mask]
                if op in ('nop','waitq'):continue
                if op in ('lq','sq','ilw','isw'):
                    off,idx=re.fullmatch(r'(-?\d+)\(VI(\d+)\)',args[1]).groups()
                    address=int(off)+int(oi[int(idx)]);assert 0<=address<1024,(pc,address)
                    if op in ('sq','isw') and in_flight:
                        assert not in_flight[0]<=address<in_flight[1], ('GIF-owned overwrite',pc,address,in_flight)
                    if op=='lq':
                        for lane in m:
                            if (address,lane) in stores:
                                assert steps-stores[address,lane]>=4,('SQ/LQ seam',pc,address,lane)
                        writes.append((reg(args[0]),m,memory[address,m].copy()))
                    elif op=='sq':
                        memory[address,m]=ov[reg(args[0]),m]
                        for lane in m:stores[address,lane]=steps
                    elif op=='ilw':vi[reg(args[0])]=int(memory[address].view(np.uint32)[m[0]])&65535
                    else:memory[address].view(np.uint32)[m]=oi[reg(args[0])]
                elif op=='xtop':vi[reg(args[0])]=top
                elif op=='loi':imm=np.array(int(args[0],16),dtype=np.uint32).view(F)[()]
                elif op=='fcset':clip=int(args[0],0)
                elif op=='clipw':
                    v=ov[reg(args[0])];w=abs(ov[reg(args[1]),3])
                    bits=sum((int(v[k]>w)|(int(v[k]<-w)<<1))<<(k*2) for k in range(3))
                    clip=((clip<<6)|bits)&0xffffff
                elif op=='fcand':vi[reg(args[0])]=int(bool(clip&int(args[1],0)))
                elif op=='fmand':vi[reg(args[0])]=0 # culling mask is zero in admitted game state
                elif op=='div':q=F(ov[reg(args[1]),LANES.index(args[1][-1])]/ov[reg(args[2]),LANES.index(args[2][-1])])
                elif op=='mtir':vi[reg(args[0])]=int(ov[reg(args[1])].view(np.uint32)[LANES.index(args[1][-1])])&65535
                elif op=='mfir':writes.append((reg(args[0]),m,np.full(len(m),np.array(signed(int(oi[reg(args[1])])),dtype=np.int32).view(F),dtype=F)))
                elif op in ('iadd','isub','iaddiu','isubiu','ior','iand'):
                    a=int(oi[reg(args[1])]);b=int(args[2],0) if op.endswith('iu') else int(oi[reg(args[2])])
                    v=a-b if op.startswith('isub') else a|b if op=='ior' else a&b if op=='iand' else a+b
                    vi[reg(args[0])]=v&65535
                elif op=='b':branch=self.labels[args[0]]
                elif op.startswith('ib'):
                    a=signed(int(oi[reg(args[0])]))
                    if op in ('ibeq','ibne'):
                        equal=a==signed(int(oi[reg(args[1])]))
                        hit=equal if op=='ibeq' else not equal
                    else:hit={'ibgtz':a>0,'ibltz':a<0,'iblez':a<=0,'ibgez':a>=0}[op]
                    if hit:branch=self.labels[args[-1]]
                elif op=='xgkick':
                    address=int(oi[reg(args[0])]);count=int(memory[address].view(np.uint32)[0])&0x7fff
                    assert address in (top+180,top+362)
                    assert count<= ((60 if address==top+180 else 36) if wide else (30 if address==top+180 else 18))
                    # Issuing a new PATH1 kick waits for its predecessor. The
                    # newly kicked bytes remain GIF-owned until the next kick.
                    in_flight=(address,address+1+count*3)
                    self.kicks+=1;self.max_packet=max(self.max_packet,count)
                    arena=int(address==top+362);self.max_arenas[arena]=max(self.max_arenas[arena],count)
                    packets.append(memory[address+1:address+1+count*3].copy())
                else:
                    a=ov[reg(args[1])].copy() if len(args)>1 and args[1].startswith('VF') else oa.copy()
                    if op=='move':v=a
                    elif op=='mr32':v=np.roll(a,-1)
                    elif op=='abs':v=np.abs(a)
                    elif op.startswith('ftoi'):
                        scale=16 if op=='ftoi4' else 1
                        v=np.clip(np.trunc(a.astype(np.float64)*scale),-2147483648,2147483647).astype(np.int32).view(F)
                    elif op in ('opmula','opmsub'):
                        b=ov[reg(args[2])]
                        if op=='opmula':v=F(a[[1,2,0,3]]*b[[2,0,1,3]])
                        else:v=F(oa-F(a[[1,2,0,3]]*b[[2,0,1,3]]))
                    else:
                        source=args[2]
                        if source=='I':b=np.full(4,oldimm,dtype=F)
                        elif source=='Q':b=np.full(4,oq,dtype=F)
                        else:
                            b=ov[reg(source)].copy()
                            if source[-1] in LANES:b=np.full(4,b[LANES.index(source[-1])],dtype=F)
                        if op.startswith('madd'):v=F(oa+F(a*b))
                        elif op.startswith('mul'):v=F(a*b)
                        elif op.startswith('add'):v=F(a+b)
                        elif op.startswith('sub'):v=F(a-b)
                        elif op.startswith('max'):v=np.maximum(a,b)
                        elif op.startswith('min'):v=np.minimum(a,b)
                        else:raise AssertionError((op,args))
                    if args[0].upper().startswith('ACC'):acc[m]=v[m]
                    else:writes.append((reg(args[0]),m,v[m].copy()))
            for dst,m,v in writes:
                if dst: vf[dst,m]=v
            next_pc=pending if pending is not None else pc+1
            assert pending is None or branch is None
            pending=branch
            if end_delay==pc:
                stops+=1;end_delay=None
                if stops==2:return np.concatenate(packets),steps
            pc=next_pc;steps+=1
        raise AssertionError('program did not finish')

def check_source_contract():
    reference=(ROOT/'vu1/general_clip_tri_x2.vcl').read_text()
    candidate=(ROOT/'vu1/general_clip_quad_x2f.vcl').read_text()
    for name in ('pd_plane','pd_sign','cp_edge','clip_pass','emit_mvert','x2_kick_chunk'):
        expression=rf'\.macro\s+{name}\s.*?\.endm'
        assert re.search(expression,reference,re.S)[0]==re.search(expression,candidate,re.S)[0],name
    # Four clip histories at shifts 18/12/6/0: preserve the authored ABC/ACD
    # triangles and ignore the Z bits just as legacy fcand(0xf3cf) does.
    assert sum(15<<shift for shift in (18,12,6))==0x3cf3c0
    assert sum(15<<shift for shift in (18,6,0))==0x3c03cf
    cpp=(ROOT/'src/x2f_renderer.cpp').read_text()
    assert 'const float outputOptions[3]' in cpp
    assert 'PGL_CITY_SOURCE_CLIP_DISPATCH ? 1.0f : 0.0f' in cpp
    assert 'PGL_CITY_SOURCE_CLIP_DISPATCH ? 4u : 0u' in cpp

def check_dispatch_boundaries(old,new):
    # Each fixture contains one actual authored quad, independently varied
    # UV/RGBA, no source transformation shortcuts in the reference.
    depths=[F(0),F(-0.),F(1),F(.9375)]
    for center in (.9375,1.0):
        depths.extend((np.nextafter(F(center),F(-np.inf)),np.nextafter(F(center),F(np.inf))))
    shapes=[]
    for w in depths:
        shapes.append(np.array([[-.2,-.2,2],[.2,-.2,2],[.2,.2,2],[-.2,.2,w]],dtype=F))
        shapes.append(np.array([[-.2,-.2,w],[.2,-.2,2],[.2,.2,2],[-.2,.2,2]],dtype=F))
    for axis,scale in ((0,180),(1,150)):
        for sign in (-1,1):
            edge=F(F(sign*2048*2)/F(scale))
            for x in (edge,np.nextafter(edge,F(-np.inf)),np.nextafter(edge,F(np.inf))):
                quad=np.array([[-.2,-.2,2],[.2,-.2,2],[.2,.2,2],[-.2,.2,2]],dtype=F)
                quad[3,axis]=x
                shapes.append(quad)
    cases=0
    for top in (79,551):
        for shape in shapes:
            results=[]
            for program,wide,dispatch in ((old,False,False),(new,False,False),(new,True,False),(new,False,True),(new,True,True)):
                memory=np.zeros((1024,4),dtype=F)
                memory[57,3]=32767
                memory[62:66]=[[180,0,0,0],[0,150,0,0],[0,0,-32767,1],[0,0,32767,0]]
                memory[75].view(np.uint32)[0]=0x8000
                memory[76]=[1,1,1,0];memory[77,0]=1
                memory[top+178]=[-1,60 if wide else 30,36 if wide else 18,int(dispatch)]
                indices=[0,1,2,0,2,3] if program is old else [0,1,2,3]
                stride=4 if program is old else 3
                memory[top].view(np.uint32)[0]=len(indices)
                for vertex,corner in enumerate(indices):
                    q=top+5+vertex*stride
                    memory[q,:3]=shape[corner]
                    memory[q+stride-2]=[corner*.37,1-corner*.19,1,0]
                    memory[q+stride-1]=[corner*.13,.7-corner*.11,.3+corner*.17,corner*.29]
                output,_=program.run(memory,top,wide=wide)
                output[0::3,3]=0
                results.append(output)
            assert all(output.tobytes()==results[0].tobytes() for output in results),('dispatch boundary',cases)
            cases+=1
    return dict(cases=cases,planes='near epsilon, W zero, exact side planes and adjacent float32',result='all gates match ordered reference payload')

def check_output_boundaries(program):
    # Fill A60 and B36 exactly, then force A->B->A->B reuse. Authored
    # corners/triangles remain intact; only packet boundaries may differ.
    shapes=[
        [[-300,-300,20],[300,-300,20],[300,300,20],[-300,300,20]],
        [[-500,-400,20],[500,-400,20],[0,400,20],[-500,400,20]],
        [[-500,-400,20],[500,-400,20],[0,400,20],[500,-400,20]],
    ]
    cases=0;peak_kicks=0;peaks=[0,0]
    for top in (79,551):
        for shape in shapes:
            outputs=[]
            for wide,dispatch in ((False,False),(True,False),(False,True),(True,True)):
                memory=np.zeros((1024,4),dtype=F)
                memory[57,3]=32767
                memory[62:66]=[[180,0,0,0],[0,150,0,0],[0,0,-32767,1],[0,0,32767,0]]
                memory[75].view(np.uint32)[0]=0x8000
                memory[76]=[1,1,1,0];memory[77,0]=1
                memory[top+178]=[-1,60 if wide else 30,36 if wide else 18,int(dispatch)]
                memory[top].view(np.uint32)[0]=32
                for vertex in range(32):
                    corner=vertex%4;q=top+5+vertex*3
                    memory[q,:3]=shape[corner]
                    memory[q+1]=[corner*.25,.5,1,0]
                    memory[q+2]=[.2+vertex/40,.7,.9,.5]
                output,_=program.run(memory,top,wide=wide)
                output[0::3,3]=0
                outputs.append(output)
                if wide:
                    peak_kicks=max(peak_kicks,program.kicks)
                    peaks=[max(a,b) for a,b in zip(peaks,program.max_arenas)]
            assert all(output.tobytes()==outputs[0].tobytes() for output in outputs), 'boundary packet mismatch'
            cases+=1
    assert peaks==[60,36] and peak_kicks>=4,(peaks,peak_kicks)
    return dict(cases=cases,full_arenas=peaks,maximum_kicks=peak_kicks,
        ownership='no store into current GIF-owned packet')

def check():
    check_source_contract()
    old=Program(ROOT/'vu1/general_clip_tri_x2_vcl.vsm')
    new=Program(ROOT/'vu1/general_clip_quad_x2f_vcl.vsm')
    rng=np.random.default_rng(0xF20)
    columns=['X2','X2F narrow dispatch OFF','X2F wide dispatch OFF','X2F narrow dispatch ON','X2F wide dispatch ON']
    width=len(columns)
    steps=[0]*width;cases=0;kicks=[0]*width;peak=[0]*width;arena_peaks=[[0,0] for _ in columns];groups={key:[0]*(width+1) for key in ('ordinary','clipping_stress','wholly_inside')}
    for sample in range(300):
        top=79 if sample&1 else 551
        count=1+sample%8
        matrix=np.array([[180,0,0,0],[0,150 if sample&1 else 75,0,0],[0,0,-32767,1],[0,0,32767,0]],dtype=F)
        if sample%3==0:matrix[0,0]=320;matrix[1,1]*=2
        positions=rng.uniform(-100,100,(count,4,3)).astype(F)
        positions[:,:,2]+=105
        if sample%4==0:positions[:,:,2]-=110
        if sample%7==0:positions[:,:,2]=rng.uniform(.2,3,(count,4)).astype(F)
        if sample%9==0:positions[0,:,0]=F(-0.)
        if sample%11==0:positions[:,:,:2]*=1000
        if sample>=200:
            # Bound absolute XY even when the earlier stress mutation made
            # them huge: the old *.01 left several nominal inside cases out.
            positions[:,:,:2]=np.tanh(positions[:,:,:2])
            positions[:,:,2]=rng.uniform(20,200,(count,4)).astype(F)
        uv=rng.uniform(-3,3,(count,4,2)).astype(F)
        color=rng.uniform(0,1,(count,4,4)).astype(F)
        group=groups['wholly_inside' if sample>=200 else 'ordinary' if sample%4 and sample%7 and sample%11 else 'clipping_stress']
        group[-1]+=1
        results=[]
        configurations=[(old,[0,1,2,0,2,3],False,False)]
        configurations += [(new,[0,1,2,3],wide,dispatch) for dispatch in (False,True) for wide in (False,True)]
        for column,(program,indices,wide,dispatch) in enumerate(configurations):
            limit=5 if program is old else 8
            pieces=[]
            for first in range(0,count,limit):
                memory=np.zeros((1024,4),dtype=F)
                memory[57,3]=32767;memory[62:66]=matrix
                memory[75].view(np.uint32)[0]=0x8000
                memory[76]=[1,1,1,0];memory[77,0]=1
                memory[top+178,0]=-1
                if program is new:
                    memory[top+178,1:3]=[60,36] if wide else [30,18]
                    memory[top+178,3]=int(dispatch)
                vertex=0
                for quad in range(first,min(count,first+limit)):
                    for corner in indices:
                        stride=4 if program is old else 3
                        q=top+5+stride*vertex
                        memory[q,:3]=positions[quad,corner]
                        memory[q+stride-2]=[*uv[quad,corner],1,0]
                        memory[q+stride-1]=color[quad,corner]
                        vertex+=1
                memory[top].view(np.uint32)[0]=vertex
                actual,cycles=program.run(memory,top,wide=wide)
                pieces.append(actual);steps[column]+=cycles;group[column]+=cycles
                kicks[column]+=program.kicks;peak[column]=max(peak[column],program.max_packet)
                arena_peaks[column]=[max(a,b) for a,b in zip(arena_peaks[column],program.max_arenas)]
            results.append(np.concatenate(pieces))
        # STQ.w is unwritten by both packet emitters and not consumed by GS.
        for result in results:result[0::3,3]=0
        assert all(result.tobytes()==results[0].tobytes() for result in results),('packet mismatch',sample,[r.shape for r in results])
        cases+=1
    # Exercise cull-enabled dispatch on a fully-inside quad. MAC arithmetic
    # is deliberately not claimed by this model, only exact fallback routing.
    memory=np.zeros((1024,4),dtype=F)
    memory[0].view(np.uint32)[3]=32
    memory[57,3]=32767;memory[62:66]=matrix
    memory[75].view(np.uint32)[0]=0x8000
    memory[76]=[1,1,1,0];memory[77,0]=1
    top=79;memory[top+178,0]=-1
    memory[top+178,1:3]=[30,18]
    for corner in range(4):
        q=top+5+3*corner
        memory[q,:3]=positions[0,corner]
        memory[q+1]=[*uv[0,corner],1,0]
        memory[q+2]=color[0,corner]
    memory[top].view(np.uint32)[0]=4
    for wide,dispatch in ((False,False),(True,False),(False,True),(True,True)):
        memory[top+178,1:]=[60 if wide else 30,36 if wide else 18,int(dispatch)]
        new.run(memory.copy(),top,forbid_inside=True,wide=wide)
    return dict(cases=cases,cull_enabled_routing='original triangle path', columns=columns,issue_pairs=steps,kicks=kicks,largest_packet_vertices=peak,largest_arena_vertices=arena_peaks,
        result='bitwise full packet payloads match',groups=groups,output_boundaries=check_output_boundaries(new),dispatch_boundaries=check_dispatch_boundaries(old,new),hardware='pending')
if __name__=='__main__':
    with np.errstate(all='ignore'):print(json.dumps(check(),indent=2))
