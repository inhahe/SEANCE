import struct, ctypes
from ctypes import wintypes
from minidump.minidumpfile import MinidumpFile
import capstone

DUMP = r'C:\Users\inhah\AppData\Local\CrashDumps\SEANCE.exe.92400.dmp'
EXE  = r'cpp/build/SEANCE_artefacts/Release/SEANCE.exe'

mf = MinidumpFile.parse(DUMP)
rdr = mf.get_reader()
base = 0x7ff7fdeb0000
mods=sorted((m.baseaddress,m.baseaddress+m.size,m.name.split('\\')[-1]) for m in mf.modules.modules)
def who(a):
    for b,e,n in mods:
        if b<=a<e: return f'{n}+0x{a-b:x}'
    return ''

exe = open(EXE,'rb').read()
pe = struct.unpack_from('<I',exe,0x3c)[0]
nsec = struct.unpack_from('<H',exe,pe+6)[0]
optsz = struct.unpack_from('<H',exe,pe+20)[0]
so = pe+24+optsz
sects=[]
for i in range(nsec):
    off=so+i*40
    vsz,va,rsz,rraw=struct.unpack_from('<IIII',exe,off+8)
    sects.append((va,vsz,rraw,rsz))
def file_to_rva(fo):
    for va,vsz,rr,rs in sects:
        if rr<=fo<rr+rs: return va+(fo-rr)
    return None

# dbghelp
dbghelp = ctypes.WinDLL(r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll')
dbghelp.SymInitialize.argtypes=[wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
dbghelp.SymLoadModuleExW.restype=ctypes.c_uint64
dbghelp.SymLoadModuleExW.argtypes=[wintypes.HANDLE,wintypes.HANDLE,wintypes.LPCWSTR,wintypes.LPCWSTR,ctypes.c_uint64,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD]
dbghelp.SymFromAddrW.argtypes=[wintypes.HANDLE,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint64),ctypes.c_void_p]
dbghelp.SymSetOptions(0x10|0x4)
h=ctypes.c_void_p(0x1234)
dbghelp.SymInitialize(h,None,False)
LOADBASE=0x10000000
dbghelp.SymLoadModuleExW(h,None,EXE,None,LOADBASE,len(exe),None,0)
class SI(ctypes.Structure):
    _fields_=[('SizeOfStruct',wintypes.ULONG),('TypeIndex',wintypes.ULONG),
      ('Reserved',ctypes.c_uint64*2),('Index',wintypes.ULONG),('Size',wintypes.ULONG),
      ('ModBase',ctypes.c_uint64),('Flags',wintypes.ULONG),('Value',ctypes.c_uint64),
      ('Address',ctypes.c_uint64),('Register',wintypes.ULONG),('Scope',wintypes.ULONG),
      ('Tag',wintypes.ULONG),('NameLen',wintypes.ULONG),('MaxNameLen',wintypes.ULONG),
      ('Name',wintypes.WCHAR*2048)]
def sym_rva(rva):
    s=SI(); s.SizeOfStruct=ctypes.sizeof(SI)-2048*2; s.MaxNameLen=2048
    d=ctypes.c_uint64(0)
    if dbghelp.SymFromAddrW(h,LOADBASE+rva,ctypes.byref(d),ctypes.byref(s)):
        return f'{s.Name}+0x{d.value:x}'
    return None

md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)

def match_retsite(va):
    # try several windows of code following the return address
    for start in (0,4,8,2,6,12,16):
        for length in (24,20,16,32):
            try: win=rdr.read(va+start,length)
            except: continue
            idx=exe.find(win)
            if idx>=0 and exe.find(win,idx+1)==-1:
                return file_to_rva(idx)-start
    return None

def find_func_start_and_sym(va):
    # scan backward in dump for 0xCC padding (function boundary), then match prologue
    try: blob=rdr.read(va-0x600, 0x600)
    except: return None
    # find last run of >=2 0xCC before end
    fstart=None
    for off in range(len(blob)-2, 0, -1):
        if blob[off]==0xCC and blob[off-1]==0xCC:
            fstart=va-0x600+off+1
            break
    if fstart is None: return None
    # match 32 bytes of prologue
    try: win=rdr.read(fstart,32)
    except: return None
    idx=exe.find(win)
    if idx>=0 and exe.find(win,idx+1)==-1:
        rva=file_to_rva(idx)
        s=sym_rva(rva)
        return (fstart-base, s)
    return None

def call_target(va):
    # disasm bytes before return addr to find the call ending exactly at va
    try: blob=rdr.read(va-16,16)
    except: return None
    for st in range(16):
        for ins in md.disasm(blob[st:], va-16+st):
            if ins.address+ins.size==va and ins.mnemonic=='call':
                op=ins.op_str
                if op.startswith('0x'):
                    return int(op,16)
                return op  # register/indirect
            if ins.address+ins.size>va: break
    return None

frames=[0x7ff7fe0fef1d,0x7ff7fe0fac08,0x7ff7fe1abf8d,0x7ff7fe4efa50,
        0x7ff7fe1b7e04,0x7ff7fe1ae81f,0x7ff7fe21ec57,0x7ff7fe21ede5,
        0x7ff7fe21e877,0x7ff7fe21129b,0x7ff7fe208722,0x7ff7fe1cd5bf,
        0x7ff7fe3297a8,0x7ff7fe22d0e2]

print('=== innermost-first, with retsite match + func-start fallback + callee ===')
for va in frames:
    rva=va-base
    cur=match_retsite(va)
    name=sym_rva(cur) if cur is not None else None
    line=f'SEANCE+{hex(rva):>9}  '
    if name:
        line+=name
    else:
        fs=find_func_start_and_sym(va)
        if fs: line+=f'[start {hex(fs[0])}] {fs[1]}'
        else:  line+='<unresolved>'
    # callee
    tgt=call_target(va)
    if isinstance(tgt,int):
        w=who(tgt)
        cn=None
        if w.startswith('SEANCE'):
            ct=match_retsite(tgt) if False else None
            # symbolize target by func-start match
            try:
                win=rdr.read(tgt,32); idx=exe.find(win)
                if idx>=0 and exe.find(win,idx+1)==-1: cn=sym_rva(file_to_rva(idx))
            except: pass
        line+=f'   --calls--> {w or hex(tgt)}'+(f' = {cn}' if cn else '')
    elif tgt:
        line+=f'   --calls--> {tgt}'
    print(line)
