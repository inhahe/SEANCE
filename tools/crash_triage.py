import struct, ctypes
from ctypes import wintypes
from minidump.minidumpfile import MinidumpFile

DUMP = r'C:\Users\inhah\AppData\Local\CrashDumps\SEANCE.exe.92400.dmp'
EXE  = r'cpp/build/SEANCE_artefacts/Release/SEANCE.exe'

mf = MinidumpFile.parse(DUMP)
rdr = mf.get_reader()
base = 0x7ff7fdeb0000

# Innermost SEANCE return addresses (VA), innermost -> outer.
rets = [0x7ff7fe0fef1d,0x7ff7fe0fac08,0x7ff7fe1abf8d,0x7ff7fe4efa50,
        0x7ff7fe1b7e04,0x7ff7fe1ae81f,0x7ff7fe21ec57,0x7ff7fe21ede5,
        0x7ff7fe21e877,0x7ff7fe21129b,0x7ff7fe208722,0x7ff7fe1cd5bf,
        0x7ff7fe3297a8,0x7ff7fe22d0e2]

exe = open(EXE,'rb').read()
pe = struct.unpack_from('<I',exe,0x3c)[0]
nsec = struct.unpack_from('<H',exe,pe+6)[0]
optsz = struct.unpack_from('<H',exe,pe+20)[0]
so = pe+24+optsz
sects=[]
for i in range(nsec):
    off=so+i*40
    name=exe[off:off+8].rstrip(b'\x00').decode('latin1')
    vsz,va,rsz,rraw=struct.unpack_from('<IIII',exe,off+8)
    sects.append((name,va,vsz,rraw,rsz))

def file_to_rva(fo):
    for n,va,vsz,rr,rs in sects:
        if rr<=fo<rr+rs: return va+(fo-rr)
    return None

# Recover current-binary RVA for each return site by byte-matching the 32
# bytes of caller code that *follow* the return address (continuation of the
# caller). Unchanged functions are byte-identical across the two builds.
recovered=[]
for r in rets:
    dump_rva=r-base
    win=rdr.read(r,32)
    idx=exe.find(win)
    uniq = idx>=0 and exe.find(win,idx+1)==-1
    cur_rva=file_to_rva(idx) if idx>=0 else None
    recovered.append((dump_rva,cur_rva,uniq))
    print(f'dumpRVA {hex(dump_rva):>10}  match={"UNIQUE" if uniq else ("MULTI" if idx>=0 else "NOMATCH"):8} curRVA {hex(cur_rva) if cur_rva else None}')

# ---- symbolize current RVAs via dbghelp.dll ----
dbghelp = ctypes.WinDLL(r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll')
SymSetOptions = dbghelp.SymSetOptions
SymInitialize = dbghelp.SymInitialize
SymInitialize.argtypes=[wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
SymLoadModuleExW = dbghelp.SymLoadModuleExW
SymLoadModuleExW.restype=ctypes.c_uint64
SymLoadModuleExW.argtypes=[wintypes.HANDLE,wintypes.HANDLE,wintypes.LPCWSTR,wintypes.LPCWSTR,ctypes.c_uint64,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD]
SymFromAddrW = dbghelp.SymFromAddrW
SymFromAddrW.argtypes=[wintypes.HANDLE,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint64),ctypes.c_void_p]

SYMOPT_LOAD_LINES=0x10
SymSetOptions(SYMOPT_LOAD_LINES|0x00000004)  # +UNDNAME
h=ctypes.c_void_p(0x1000)
SymInitialize(h,None,False)
LOADBASE=0x10000000
m=SymLoadModuleExW(h,None,EXE,None,LOADBASE,len(exe),None,0)

class SYMBOL_INFOW(ctypes.Structure):
    _fields_=[('SizeOfStruct',wintypes.ULONG),('TypeIndex',wintypes.ULONG),
        ('Reserved',ctypes.c_uint64*2),('Index',wintypes.ULONG),('Size',wintypes.ULONG),
        ('ModBase',ctypes.c_uint64),('Flags',wintypes.ULONG),('Value',ctypes.c_uint64),
        ('Address',ctypes.c_uint64),('Register',wintypes.ULONG),('Scope',wintypes.ULONG),
        ('Tag',wintypes.ULONG),('NameLen',wintypes.ULONG),('MaxNameLen',wintypes.ULONG),
        ('Name',wintypes.WCHAR*1024)]
def sym(rva):
    if rva is None: return None
    s=SYMBOL_INFOW(); s.SizeOfStruct=ctypes.sizeof(SYMBOL_INFOW)-1024*2; s.MaxNameLen=1024
    disp=ctypes.c_uint64(0)
    if SymFromAddrW(h,LOADBASE+rva,ctypes.byref(disp),ctypes.byref(s)):
        return f'{s.Name}+0x{disp.value:x}'
    return f'<no sym @rva {hex(rva)}>'

print('\n=== innermost-first call stack (current-build symbols) ===')
for dump_rva,cur_rva,uniq in recovered:
    print(f'  SEANCE+{hex(dump_rva):>10}  {sym(cur_rva) if uniq else "(no unique match - changed TU?)"}')
