# SPDX-License-Identifier: MIT
"""Real FIFO texture reads and EFB writes across separate consumer banks.

The default exercises Wii MEM2. Pass --gamecube for the single-bank build.
No firmware or game assets required.
The test is hidden, does not activate a desktop window, and has no audio output.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import re
import struct

BP=c.CFUNCTYPE(None,c.c_void_p,c.c_uint32,c.c_uint32)
WRITE=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_uint32,c.POINTER(c.c_uint8),*[c.c_uint32]*5)
EPOCH=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_uint32,c.c_uint32,c.POINTER(c.c_uint64))
class Range(c.Structure):
    _fields_=[('bytes',c.POINTER(c.c_uint8)),('address',c.c_uint32),('available',c.c_uint32)]
RESOLVE=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_uint32,c.c_uint32,c.POINTER(Range))
LOCATE=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_void_p,c.c_uint32,c.POINTER(c.c_uint32))
class Config(c.Structure):
    _fields_=[('abi',c.c_uint32),('title',c.c_char_p),('data',c.c_char_p),('cache',c.c_char_p),
        ('hidden',c.c_uint32),('mem1',c.c_void_p),('mem1_size',c.c_uint32),('user',c.c_void_p),
        ('bp',BP),('write',WRITE),('epoch',EPOCH),('texture_epoch',EPOCH),
        ('own',c.c_void_p),('pending',c.c_void_p),('prepare',c.c_void_p),('retire',c.c_void_p),
        ('resolve',RESOLVE),('locate',LOCATE)]
START=c.CFUNCTYPE(c.c_int,c.POINTER(Config))
FIFO=c.CFUNCTYPE(c.c_int,c.POINTER(c.c_uint8),c.c_uint32)
DRAIN=c.CFUNCTYPE(c.c_int)
PEEK=c.CFUNCTYPE(c.c_int,c.c_uint32,c.c_uint16,c.c_uint16,c.POINTER(c.c_uint32))
class Api(c.Structure):
    _fields_=[('abi',c.c_uint32),('start',START),('fifo',FIFO),('drain',DRAIN),
              ('status',c.c_void_p),('peek',PEEK),('stop',c.CFUNCTYPE(None))]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library',type=Path,required=True)
    parser.add_argument('--work-root',type=Path,required=True)
    parser.add_argument('--gamecube',action='store_true')
    args=parser.parse_args()
    work=args.work_root.resolve();work.mkdir(parents=True,exist_ok=True)
    (work/'cache').mkdir(exist_ok=True)
    os.environ.update(SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0',SDL_AUDIODRIVER='dummy',GCN_AUDIO='0')
    header=(Path(__file__).resolve().parents[2]/'include/gcnllr/native_gx.h').read_text()
    abi=int(re.search(r'^#define GCNLLR_NATIVE_GX_ABI (\d+)u',header,re.M)[1])
    directory=os.add_dll_directory(str(args.library.resolve().parent)) if os.name=='nt' else None
    dll=c.CDLL(str(args.library.resolve()))
    dll.gcnllr_native_gx_get_api.argtypes=[c.c_uint32]
    dll.gcnllr_native_gx_get_api.restype=c.POINTER(Api)
    pointer=dll.gcnllr_native_gx_get_api(abi)
    if not pointer:raise RuntimeError('GPU ABI mismatch')
    api=pointer.contents
    mem1=(c.c_uint8*(24*1024*1024))()
    base=0x01000000 if args.gamecube else 0x10000000
    region_type=c.c_uint8*(1024*1024)
    region=region_type.from_buffer(mem1,base) if args.gamecube else region_type()
    banks=((0,mem1),) if args.gamecube else ((0,mem1),(base,region))
    reads=[];writes=[]
    def mapping(address,size):
        address&=0x1fffffff
        for base,bank in banks:
            offset=address-base
            if 0<=offset<len(bank) and size<=len(bank)-offset:return base,bank,offset
        return None
    @RESOLVE
    def resolve(user,address,size,out):
        mapped=mapping(address,size)
        if not mapped:return 0
        base,bank,offset=mapped;reads.append(base+offset)
        out[0]=Range(c.cast(c.addressof(bank)+offset,c.POINTER(c.c_uint8)),base+offset,len(bank)-offset)
        return 1
    @LOCATE
    def locate(user,data,size,address):
        for base,bank in banks:
            offset=data-c.addressof(bank)
            if 0<=offset<len(bank) and size<=len(bank)-offset:address[0]=base+offset;return 1
        return 0
    @WRITE
    def write(user,address,source,size,flags,width,height,stride):
        mapped=mapping(address,size)
        if not mapped:return 0
        base,bank,offset=mapped
        c.memmove(c.addressof(bank)+offset,source,size);writes.append(address);return 1
    config=Config(abi,b'Wii GPU memory contract',str(work).encode(),str(work/'cache').encode(),1,
                  None,0,None,BP(lambda user,reg,value:None),write,EPOCH(),EPOCH(),None,None,None,None,resolve,locate)
    def send(raw):
        raw+=bytes((-len(raw))%32)
        source=(c.c_uint8*len(raw)).from_buffer_copy(raw)
        if not api.fifo(source,len(raw)) or not api.drain():raise RuntimeError('GPU FIFO failed')
    def bp(values):return b''.join(b'\x61'+struct.pack('>I',reg<<24|value) for reg,value in values)
    def cp(reg,value):return b'\x08'+bytes([reg])+struct.pack('>I',value)
    def floats(values):return struct.pack('>'+len(values)*'f',*values)
    def xf(reg,values):return b'\x10'+struct.pack('>I',((len(values)//4-1)<<16)|reg)+values
    if not api.start(c.byref(config)):raise RuntimeError('GPU start failed')
    try:
        tile=base+0x100;destination=base+0x10000
        c.memset(c.addressof(mem1)+0x10000,0xa5,640*480*2)
        c.memmove(c.addressof(region)+0x100,bytes([0]*4+[255]*4)*4,32)
        raw=cp(0x50,(1<<9)|(1<<13))+cp(0x60,0)+cp(0x70,1|(4<<1)|(5<<14))+cp(0x80,0)+cp(0x90,0)
        raw+=xf(0,floats([1,0,0,0,0,1,0,0,0,0,1,0]))
        raw+=xf(0x1018,struct.pack('>I',30<<6))+xf(0x101a,floats([320,-240,16777215,662,582,16777215]))
        raw+=xf(0x1020,floats([1,0,1,0,-1,0])+struct.pack('>I',1))
        raw+=xf(0x1009,struct.pack('>I',1))+xf(0x100e,struct.pack('>I',1))+xf(0x1010,struct.pack('>I',1))
        raw+=xf(120,floats([0,0,0,.75,0,0,0,.5]))+xf(0x103f,struct.pack('>I',1))+xf(0x1040,struct.pack('>I',4))
        raw+=bp([(0,0x11),(0x20,(342<<12)|342),(0x21,(981<<12)|821),(0x59,171|(171<<10)),
            (0x28,1<<6),(0x30,7),(0x31,3),(0x40,0),(0x41,0x18),(0x43,0),
            (0xf3,0x3f0000),(0xf6,4),(0xf7,14),
            (0x80,0),(0x84,0),(0x88,(1<<20)|(3<<10)|7),(0x94,tile>>5),
            (0xc0,8|(15<<4)|(15<<8)|(15<<12)|(1<<19)),(0xc1,(4<<4)|(7<<7)|(7<<10)|(7<<13)|(1<<19)),
            (0x49,0),(0x4a,(479<<10)|639),(0x4b,destination>>5),(0x4d,40),(0x4e,256),
            (0x53,(21<<12)|(22<<18)),(0x54,21)])
        send(raw)
        quad=b'\x80\0\4'+b''.join(floats([x,y,.5])+bytes([255]*4) for x,y in [(-1,-1),(1,-1),(1,1),(-1,1)])
        for black,expected in ((False,bytes([235,128,235,128])),(True,bytes([16,128,16,128]))):
            if black:c.memset(c.addressof(region)+0x100,0,32)
            send(quad+bp([(0x52,1<<14),(0x45,2)]))
            at=0x10000+240*1280+640
            actual=bytes(region[at:at+4])
            if any(abs(a-b)>3 for a,b in zip(actual,expected)):
                raise RuntimeError(f'GPU texture/copy mismatch: {actual.hex()} != {expected.hex()}')
        if tile not in reads or writes!=[destination,destination]:raise RuntimeError('Physical bank identities were lost')
        if bytes(mem1[0x10000:0x10000+640*480*2])!=bytes([0xa5])*(640*480*2):
            raise RuntimeError('GPU copy overwrote low MEM1')
        # Indexed arrays retain their address while the CPU changes vertex
        # colors. A missing epoch callback must hash the real array bytes.
        positions=base+0x200;colors=base+0x400
        c.memmove(c.addressof(region)+0x100,bytes([0]*4+[255]*4)*4,32)
        vertices=[(-1,-1),(1,-1),(1,1),(-1,1)]
        c.memmove(c.addressof(region)+0x200,b''.join(floats([x,y,.5]) for x,y in vertices),48)
        c.memmove(c.addressof(region)+0x400,bytes([255,255,255,0])*4,16)
        send(cp(0x50,(2<<9)|(2<<13))+cp(0xa0,positions)+cp(0xb0,12)+cp(0xa2,colors)+cp(0xb2,4)+
             bp([(0x41,1|0x18|(4<<8)|(5<<5))]))
        indexed=b'\x80\0\4'+bytes([0,0,1,1,2,2,3,3])
        send(indexed+bp([(0x52,1<<14),(0x45,2)]))
        c.memmove(c.addressof(region)+0x400,bytes([255])*16,16)
        send(indexed+bp([(0x52,1<<14),(0x45,2)]))
        actual=bytes(region[at:at+4]);expected=bytes([235,128,235,128])
        if any(abs(a-b)>3 for a,b in zip(actual,expected)):
            raise RuntimeError(f'GPU indexed vertex rewrite mismatch: {actual.hex()} != {expected.hex()}')
        # 2D menus put XY geometry exactly on an orthographic near plane.
        # Compare explicit XYZ(z=0) and the hardware's implicit XY z=0.
        for xy in (False,True):
            send(bp([(0x41,0x18),(0x4f,0),(0x50,0),(0x51,0xffffff),(0x52,(1<<14)|(1<<11))]))
            send(cp(0x50,(1<<9)|(1<<13))+cp(0x70,(0 if xy else 1)|(4<<1)|(5<<14))+
                 xf(0x1020,floats([1,0,1,0,-.002,-1])+struct.pack('>I',1)))
            flat=b'\x80\0\4'+b''.join(floats([x,y] if xy else [x,y,0])+bytes([255]*4) for x,y in vertices)
            send(flat+bp([(0x52,1<<14),(0x45,2)]))
            actual=bytes(region[at:at+4])
            if any(abs(a-b)>3 for a,b in zip(actual,expected)):
                raise RuntimeError(f'Orthographic near-plane {"XY" if xy else "XYZ"} mismatch: {actual.hex()} != {expected.hex()}')
        # Ten-bit texture dimensions encode size minus one. Maximum-size I4
        # atlases must still resolve and sample real MEM2 data in either axis.
        large_tile=base+0xc0000
        for width,height in ((1024,8),(8,1024),(128,1024)):
            c.memset(c.addressof(region)+0xc0000,0xff,width*height//2)
            send(bp([(0x4f,0),(0x50,0),(0x51,0xffffff),(0x52,(1<<14)|(1<<11))]))
            send(cp(0x50,(1<<9)|(1<<13))+cp(0x70,1|(4<<1)|(5<<14))+
                 xf(0x1020,floats([1,0,1,0,-1,0])+struct.pack('>I',1))+
                 bp([(0x88,((height-1)<<10)|(width-1)),(0x94,large_tile>>5),
                     (0x30,width-1),(0x31,height-1)]))
            send(quad+bp([(0x52,1<<14),(0x45,2)]))
            actual=bytes(region[at:at+4])
            if any(abs(a-b)>3 for a,b in zip(actual,expected)):
                raise RuntimeError(f'Maximum I4 texture {width}x{height}: {actual.hex()} != {expected.hex()}')
        if large_tile not in reads:raise RuntimeError('Maximum I4 texture was never resolved from RAM')
        # XF can select a texture coordinate absent from the draw's CP vertex
        # layout. The implicit (0, 0) must sample the left half of the tile.
        send(xf(120,floats([1,0,0,0,0,1,0,0]))+
             xf(0x1040,struct.pack('>I',(5<<7)|(1<<2)))+
             bp([(0x88,(1<<20)|(3<<10)|7),(0x94,tile>>5),(0x30,7),(0x31,3)]))
        textured_quad=b'\x80\0\4'+b''.join(
            floats([x,y,.5])+bytes([255]*4)+floats([.75,.5]) for x,y in vertices)
        for has_uv,luma in ((False,16),(True,235),(False,16)):
            send(cp(0x60,int(has_uv))+cp(0x70,1|(4<<1)|(5<<14)|(1<<21)|(4<<22))+
                 (textured_quad if has_uv else quad)+bp([(0x52,1<<14),(0x45,2)]))
            actual=bytes(region[at:at+4]);expected=bytes([luma,128,luma,128])
            if any(abs(a-b)>3 for a,b in zip(actual,expected)):
                raise RuntimeError(f'Texgen source with UV enabled={has_uv}: {actual.hex()} != {expected.hex()}')
        send(xf(0x1040,struct.pack('>I',4)))
        # Legal disabled-texture TEV input still selects TEXC/TEXA. It must
        # supply a constant, without registering NULL as texture sampler255.
        for texgens,luma in ((1,235),(0,16),(1,235)):
            before=len(reads)
            # Actual failing menu equation: ZERO/RASC/TEXC/ZERO.
            send(xf(0x103f,struct.pack('>I',texgens))+
                 bp([(0,0x10|texgens),(0x28,0),
                     (0xc0,15|(8<<4)|(10<<8)|(15<<12)|(1<<19))])+
                 quad+bp([(0x52,1<<14),(0x45,2)]))
            actual=bytes(region[at:at+4]);expected=bytes([luma,128,luma,128])
            if any(abs(a-b)>3 for a,b in zip(actual,expected)):
                raise RuntimeError(f'Disabled texture ({texgens} texgens): {actual.hex()} != {expected.hex()}')
            if large_tile in reads[before:]:raise RuntimeError('Disabled texture sampled RAM')
        result=dict(result='passed',library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest(),
                    abi=abi,physical_base=hex(base),texture=True,display_copy=True,cpu_rewrite=True,mem1_preserved=True,
                    indexed_vertices=True,cpu_vertex_rewrite=True,near_plane_xy=True,max_texture_dimensions=True,
                    disabled_texture_tev=True,disabled_texture_texgen_key=True,missing_vertex_uv=True)
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result))
    finally:
        api.stop()
        if directory:directory.close()
if __name__=='__main__':main()
