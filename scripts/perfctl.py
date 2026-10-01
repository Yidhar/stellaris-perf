"""Load or unload stellaris_perf.dll in a running Stellaris (Windows, Python 3.8+, no extra packages).

    python perfctl.py load [--wait] [--dll PATH]    inject the DLL (--wait: wait for stellaris.exe to start)
    python perfctl.py unload                        ask the DLL to remove its hooks and unload itself
    python perfctl.py reload [--dll PATH]           unload, then load again
    python perfctl.py status                        loaded or not

Loading is a remote LoadLibraryW. Unloading never calls FreeLibrary from outside: a game thread may be
inside one of the DLL's detours. Instead this sets the DLL's own event (Local\\stellaris_perf_unload_<pid>);
the DLL removes its hooks, waits for in-flight calls to finish and unloads itself.

The injection lasts for that game session only. Nothing is written to the game's files except
stellaris_perf.ini and stellaris_perf.log next to stellaris.exe.
"""
import argparse
import ctypes
import ctypes.wintypes as w
import os
import sys
import time

MODULE = "stellaris_perf.dll"
HERE = os.path.dirname(os.path.abspath(__file__))
# next to the scripts folder in a release zip, or the CMake output in a source checkout
DLL_CANDIDATES = [
    os.path.join(HERE, "..", MODULE),
    os.path.join(HERE, "..", "build", "Release", MODULE),
]

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x2, 0x8, 0x10
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT_RESERVE, MEM_RELEASE, PAGE_READWRITE = 0x3000, 0x8000, 0x04
EVENT_MODIFY_STATE = 0x0002


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
                ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", w.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("th32ModuleID", w.DWORD), ("th32ProcessID", w.DWORD),
                ("GlblcntUsage", w.DWORD), ("ProccntUsage", w.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", w.DWORD), ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


kernel32.CreateToolhelp32Snapshot.restype = w.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
kernel32.Process32FirstW.argtypes = kernel32.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Module32FirstW.argtypes = kernel32.Module32NextW.argtypes = [w.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.OpenProcess.restype = w.HANDLE
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualAllocEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD, w.DWORD]
kernel32.VirtualFreeEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD]
kernel32.WriteProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
kernel32.GetModuleHandleW.restype = w.HMODULE
kernel32.GetModuleHandleW.argtypes = [w.LPCWSTR]
kernel32.GetProcAddress.restype = ctypes.c_void_p
kernel32.GetProcAddress.argtypes = [w.HMODULE, ctypes.c_char_p]
kernel32.CreateRemoteThread.restype = w.HANDLE
kernel32.CreateRemoteThread.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                                        w.DWORD, ctypes.c_void_p]
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.OpenEventW.restype = w.HANDLE
kernel32.OpenEventW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
kernel32.SetEvent.argtypes = [w.HANDLE]


def game_pids():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
    pids = []
    ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == "stellaris.exe":
            pids.append(entry.th32ProcessID)
        ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return pids


def module_loaded(pid, name):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap in (None, -1, w.HANDLE(-1).value):
        return False
    entry = MODULEENTRY32W(dwSize=ctypes.sizeof(MODULEENTRY32W))
    found = False
    ok = kernel32.Module32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szModule.lower() == name.lower():
            found = True
            break
        ok = kernel32.Module32NextW(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return found


def find_game(wait):
    deadline = time.time() + (3600 if wait else 0)
    while True:
        pids = game_pids()
        if len(pids) == 1:
            return pids[0]
        if len(pids) > 1:
            raise SystemExit(f"found {len(pids)} stellaris.exe processes {pids}; close the extra ones first")
        if time.time() >= deadline:
            raise SystemExit("stellaris.exe is not running (use --wait to wait for it)")
        time.sleep(0.5)


def find_dll(explicit):
    paths = [explicit] if explicit else DLL_CANDIDATES
    for p in paths:
        p = os.path.abspath(p)
        if os.path.exists(p):
            return p
    raise SystemExit(f"{MODULE} not found (looked in: {', '.join(os.path.abspath(p) for p in paths)}); use --dll")


def load(pid, dll_path):
    if module_loaded(pid, MODULE):
        print(f"{MODULE}: already loaded")
        return True
    proc = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not proc:
        print(f"cannot open the game process (error {ctypes.get_last_error()}); run from the same user, "
              "elevated if the game runs elevated")
        return False
    data = ctypes.create_unicode_buffer(dll_path)
    size = ctypes.sizeof(data)
    mem = kernel32.VirtualAllocEx(proc, None, size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
    kernel32.WriteProcessMemory(proc, mem, data, size, None)
    load_library = kernel32.GetProcAddress(kernel32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    thread = kernel32.CreateRemoteThread(proc, None, 0, load_library, mem, 0, None)
    kernel32.WaitForSingleObject(thread, 10000)
    kernel32.CloseHandle(thread)
    kernel32.VirtualFreeEx(proc, mem, 0, MEM_RELEASE)
    kernel32.CloseHandle(proc)
    ok = module_loaded(pid, MODULE)
    print(f"{MODULE}: {'loaded' if ok else 'LOAD FAILED'}")
    return ok


def unload(pid, timeout=30.0):
    if not module_loaded(pid, MODULE):
        print(f"{MODULE}: not loaded")
        return True
    event = kernel32.OpenEventW(EVENT_MODIFY_STATE, False, f"Local\\stellaris_perf_unload_{pid}")
    if not event:
        print(f"{MODULE}: unload event not found; restart the game instead of ejecting it")
        return False
    kernel32.SetEvent(event)
    kernel32.CloseHandle(event)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not module_loaded(pid, MODULE):
            print(f"{MODULE}: unloaded itself")
            return True
        time.sleep(0.2)
    print(f"{MODULE}: still loaded after {timeout:.0f}s")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("action", choices=["load", "unload", "reload", "status"])
    ap.add_argument("--wait", action="store_true", help="load: wait until stellaris.exe is running")
    ap.add_argument("--dll", help=f"path of {MODULE} (default: next to scripts/, or build/Release)")
    args = ap.parse_args()

    pid = find_game(args.wait and args.action in ("load", "reload"))
    if args.action == "status":
        print(f"{MODULE}: {'loaded' if module_loaded(pid, MODULE) else 'not loaded'} (stellaris.exe pid {pid})")
        return 0
    ok = True
    if args.action in ("unload", "reload"):
        ok = unload(pid)
    if ok and args.action in ("load", "reload"):
        ok = load(pid, find_dll(args.dll))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
