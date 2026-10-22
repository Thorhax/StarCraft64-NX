"""Minimal StormLib ctypes wrapper: list / read files in an MPQ."""
import ctypes, os, sys
lib = ctypes.CDLL(os.environ.get('STORMLIB', 'libstorm.so'))
H = ctypes.c_void_p
class FIND(ctypes.Structure):
    _fields_ = [('cFileName', ctypes.c_char*1024), ('szPlainName', ctypes.c_char_p),
                ('dwHashIndex', ctypes.c_uint32), ('dwBlockIndex', ctypes.c_uint32),
                ('dwFileSize', ctypes.c_uint32), ('dwFileFlags', ctypes.c_uint32),
                ('dwCompSize', ctypes.c_uint32), ('dwFileTimeLo', ctypes.c_uint32),
                ('dwFileTimeHi', ctypes.c_uint32), ('lcLocale', ctypes.c_uint32)]
lib.SFileOpenArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(H)]
lib.SFileFindFirstFile.restype = H
lib.SFileFindFirstFile.argtypes = [H, ctypes.c_char_p, ctypes.POINTER(FIND), ctypes.c_char_p]
lib.SFileFindNextFile.argtypes = [H, ctypes.POINTER(FIND)]
lib.SFileFindClose.argtypes = [H]
lib.SFileOpenFileEx.argtypes = [H, ctypes.c_char_p, ctypes.c_uint32, ctypes.POINTER(H)]
lib.SFileGetFileSize.argtypes = [H, ctypes.POINTER(ctypes.c_uint32)]
lib.SFileReadFile.argtypes = [H, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
lib.SFileCloseFile.argtypes = [H]
lib.SFileCloseArchive.argtypes = [H]
STREAM_FLAG_READ_ONLY = 0x100

def open_archive(path):
    h = H()
    if not lib.SFileOpenArchive(str(path).encode(), 0, STREAM_FLAG_READ_ONLY, ctypes.byref(h)):
        raise OSError(f'cannot open {path}')
    return h

def names(h, listfile=None):
    f = FIND(); out = []
    s = lib.SFileFindFirstFile(h, b'*', ctypes.byref(f), listfile.encode() if listfile else None)
    if not s: return out
    while True:
        out.append((f.cFileName.decode('latin-1'), f.dwFileSize))
        if not lib.SFileFindNextFile(s, ctypes.byref(f)): break
    lib.SFileFindClose(s); return out

def read(h, name):
    fh = H()
    if not lib.SFileOpenFileEx(h, name.encode('latin-1'), 0, ctypes.byref(fh)): return None
    size = lib.SFileGetFileSize(fh, None); buf = ctypes.create_string_buffer(size); got = ctypes.c_uint32()
    ok = lib.SFileReadFile(fh, buf, size, ctypes.byref(got), None); lib.SFileCloseFile(fh)
    return buf.raw[:got.value] if ok or got.value == size else None

if __name__ == '__main__':
    for p in sys.argv[1:]:
        h = open_archive(p); n = names(h)
        print(f'== {p}: {len(n)} named files')
        for name, size in n: print(size, name)
