"""Load / unload stellaris_perf.dll and stellaris_bench.dll in the running game.

    python dllctl.py load   perf|bench|all
    python dllctl.py unload perf|bench|all
    python dllctl.py reload perf|bench|all
    python dllctl.py status

Loading is a remote LoadLibraryW. Unloading never calls FreeLibrary from outside (a game thread may
be inside a hook of the DLL): it sets the DLL's event Local\\stellaris_<name>_unload_<pid>, and the
DLL removes its hooks, waits for them to drain and unloads itself.
"""
import ctypes
import ctypes.wintypes as w
import os
import sys
import time

from benchlib import PERF_DIR, game_pid, kernel32, module_loaded

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _dll(name):
    """Next to bench/ in a release zip, else the CMake output of a source checkout."""
    for p in (os.path.join(ROOT, name), os.path.join(ROOT, "build", "Release", name)):
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "Release", name)


DLLS = {
    # the plugin is loaded from its own folder (benchlib.perf_dir): that is where it finds config\\ and writes logs\\
    "perf": ("stellaris_perf.dll", os.path.join(PERF_DIR, "stellaris_perf.dll")),
    "bench": ("stellaris_bench.dll", _dll("stellaris_bench.dll")),
}

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT_RESERVE, MEM_RELEASE, PAGE_READWRITE = 0x3000, 0x8000, 0x04
EVENT_MODIFY_STATE = 0x0002
kernel32.OpenProcess.restype = w.HANDLE
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualAllocEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD, w.DWORD]
kernel32.VirtualFreeEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD]
kernel32.WriteProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
kernel32.GetModuleHandleW.restype = w.HMODULE
kernel32.GetProcAddress.restype = ctypes.c_void_p
kernel32.GetProcAddress.argtypes = [w.HMODULE, ctypes.c_char_p]
kernel32.CreateRemoteThread.restype = w.HANDLE
kernel32.CreateRemoteThread.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                                        w.DWORD, ctypes.c_void_p]
kernel32.OpenEventW.restype = w.HANDLE
kernel32.OpenEventW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
kernel32.SetEvent.argtypes = [w.HANDLE]
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]


def load(pid, key):
    name, path = DLLS[key]
    if module_loaded(pid, name):
        print(f"{name}: already loaded")
        return True
    if not os.path.exists(path):
        print(f"{name}: {path} not built")
        return False
    proc = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    data = ctypes.create_unicode_buffer(path)
    size = ctypes.sizeof(data)
    mem = kernel32.VirtualAllocEx(proc, None, size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
    kernel32.WriteProcessMemory(proc, mem, data, size, None)
    load_library = kernel32.GetProcAddress(kernel32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    thread = kernel32.CreateRemoteThread(proc, None, 0, load_library, mem, 0, None)
    kernel32.WaitForSingleObject(thread, 10000)
    kernel32.CloseHandle(thread)
    kernel32.VirtualFreeEx(proc, mem, 0, MEM_RELEASE)
    kernel32.CloseHandle(proc)
    ok = module_loaded(pid, name)
    print(f"{name}: {'loaded' if ok else 'LOAD FAILED'}")
    return ok


def unload(pid, key, timeout=30.0):
    name, _ = DLLS[key]
    if not module_loaded(pid, name):
        print(f"{name}: not loaded")
        return True
    event = kernel32.OpenEventW(EVENT_MODIFY_STATE, False, f"Local\\{name[:-4]}_unload_{pid}")
    if not event:
        print(f"{name}: unload event not found (an old build?); restart the game instead of ejecting it")
        return False
    kernel32.SetEvent(event)
    kernel32.CloseHandle(event)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not module_loaded(pid, name):
            print(f"{name}: unloaded itself")
            return True
        time.sleep(0.2)
    print(f"{name}: still loaded after {timeout:.0f}s")
    return False


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    pid = game_pid()
    action = sys.argv[1]
    if action == "status":
        for key, (name, _) in DLLS.items():
            print(f"{name}: {'loaded' if module_loaded(pid, name) else 'not loaded'}")
        return 0
    keys = list(DLLS) if len(sys.argv) < 3 or sys.argv[2] == "all" else [sys.argv[2]]
    ok = True
    for key in keys:
        if action in ("unload", "reload"):
            ok = unload(pid, key) and ok
        if action in ("load", "reload"):
            ok = load(pid, key) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
