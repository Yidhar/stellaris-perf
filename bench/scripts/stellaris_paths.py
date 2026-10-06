"""Where things are on this machine, found rather than assumed. Shared by the bench scripts and the stress-mod generator.

    documents_folder()     Documents from the shell (it can be moved, e.g. into OneDrive)
    stellaris_data_dir()   <Documents>\\Paradox Interactive\\Stellaris (mods, dlc_load.json, continue_game.json, plugins)
    game_dir()             the folder of stellaris.exe: STELLARIS_DIR if set, else the Steam library that has the game; None if not found
    require_game_dir()     the same, or an exit that says to set STELLARIS_DIR
"""
import ctypes
import os
import re

FOLDERID_DOCUMENTS = "D0 9A D3 FD 8F 23 AF 46 AD B4 6C 85 48 03 69 C7"


def documents_folder():
    buf = ctypes.c_wchar_p()
    folder_id = (ctypes.c_byte * 16)(*bytes.fromhex(FOLDERID_DOCUMENTS))
    try:
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(folder_id), 0, None, ctypes.byref(buf)) == 0:
            return buf.value
    except (AttributeError, OSError):
        pass
    return os.path.join(os.path.expanduser("~"), "Documents")


def stellaris_data_dir():
    return os.path.join(documents_folder(), "Paradox Interactive", "Stellaris")


def _steam_root():
    try:
        import winreg
        for hive, sub in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, sub) as k:
                    for name in ("SteamPath", "InstallPath"):
                        try:
                            value = winreg.QueryValueEx(k, name)[0]
                            if value:
                                return os.path.normpath(value)
                        except OSError:
                            pass
            except OSError:
                pass
    except ImportError:
        pass
    for var in ("ProgramFiles(x86)", "ProgramFiles"):
        base = os.environ.get(var)
        if base and os.path.isdir(os.path.join(base, "Steam")):
            return os.path.join(base, "Steam")
    return None


def game_dir():
    env = os.environ.get("STELLARIS_DIR")
    if env:
        return env
    root = _steam_root()
    if not root:
        return None
    libraries = [root]
    try:
        with open(os.path.join(root, "steamapps", "libraryfolders.vdf"), encoding="utf-8", errors="replace") as f:
            libraries += [os.path.normpath(p.replace("\\\\", "\\")) for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
    except OSError:
        pass
    for lib in libraries:
        candidate = os.path.join(lib, "steamapps", "common", "Stellaris")
        if os.path.isfile(os.path.join(candidate, "stellaris.exe")):
            return candidate
    return None


def require_game_dir():
    d = game_dir()
    if not d:
        raise SystemExit("cannot find Stellaris (stellaris.exe) in the Steam libraries; set STELLARIS_DIR to its folder")
    return d
