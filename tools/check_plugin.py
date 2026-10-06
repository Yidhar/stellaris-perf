"""Checks the plugin folder against the launcher's plugin spec (schema 2) and against this repository.

    python tools/check_plugin.py [--dir FOLDER] [--tag vX.Y.Z] [--release]

Without --dir the sources are checked (plugin/ and sdk/); with --dir a folder as it is installed: a build\\plugin\\stellaris-perf
or an unpacked release zip, where the DLL has to be there too. Checks:
  * stl-plugin.json is schema 2, has a valid id, a dll path inside the folder, and the config entries point at files
    that exist in the folder, one per name, without sub-folders;
  * game.exe_timestamps contains sdk::kExeTimestamp of sdk/stellaris_sdk.hpp (the plugin locates addresses in the exe,
    so the launcher must only load it into the build the SDK was made from);
  * the version is x.y.z, and with --tag the tag is v<version>;
  * no file for the game folder: nothing but the manifest, the DLL, defaults/, config/, data/, logs/ and docs;
  * with --release (the folder that becomes the release zip) also no config/ and no logs/: those are the user's.
Exit code 1 and a message per problem when something is wrong.
"""
import argparse
import fnmatch
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", help="an assembled plugin folder (default: the sources, plugin/)")
    ap.add_argument("--tag", help="a release tag that has to match the manifest version (vX.Y.Z)")
    ap.add_argument("--release", action="store_true", help="the folder is the content of a release zip: no config/ or logs/")
    args = ap.parse_args()

    folder = os.path.abspath(args.dir) if args.dir else os.path.join(ROOT, "plugin")
    problems = []

    def bad(msg):
        problems.append(msg)

    manifest_path = os.path.join(folder, "stl-plugin.json")
    if not os.path.isfile(manifest_path):
        print(f"{manifest_path}: not found", file=sys.stderr)
        return 1
    with open(manifest_path, encoding="utf-8-sig") as f:
        m = json.load(f)

    if m.get("schema") != 2:
        bad(f"schema must be 2, is {m.get('schema')!r}")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", str(m.get("id", ""))):
        bad(f"id {m.get('id')!r}: letters, digits, '-', '_', '.'")
    for key in ("name", "version", "description", "dll"):
        if not m.get(key):
            bad(f"{key} is missing")
    if not re.fullmatch(r"\d+\.\d+\.\d+", str(m.get("version", ""))):
        bad(f"version {m.get('version')!r} is not x.y.z")
    if args.tag and args.tag != "v" + str(m.get("version")):
        bad(f"tag {args.tag} does not match version {m.get('version')} (expected v{m.get('version')})")

    dll = str(m.get("dll", ""))
    if os.path.isabs(dll) or ".." in dll.replace("\\", "/").split("/"):
        bad(f"dll {dll!r} must be a path inside the plugin folder")
    if args.dir and not os.path.isfile(os.path.join(folder, dll)):
        bad(f"the DLL {dll} is not in {folder}")

    names = set()
    for c in m.get("config", []):
        name = c.get("file", "")
        if not name or "/" in name or "\\" in name:
            bad(f"config file {name!r}: a name in config/, no sub-folders")
        if name in names:
            bad(f"config file {name!r} is listed twice")
        names.add(name)
        default = c.get("default")
        if default and not os.path.isfile(os.path.join(folder, default)):
            bad(f"config default {default!r} does not exist in the plugin folder")
        if not c.get("title"):
            bad(f"config {name}: title is missing")
    # the launcher updates the plugin from the latest GitHub release: the asset pattern has to pick the plugin zip and
    # not the benchmark zip (stellaris-perf-bench-v<version>.zip), which starts with the same words
    upd = m.get("update")
    if upd:
        if not re.fullmatch(r"[\w.-]+/[\w.-]+", str(upd.get("github", ""))):
            bad(f"update.github {upd.get('github')!r} is not owner/repo")
        pattern = str(upd.get("asset", "*.zip"))
        plugin_zip = f"stellaris-perf-v{m.get('version')}.zip"
        if not fnmatch.fnmatch(plugin_zip, pattern):
            bad(f"update.asset {pattern!r} does not match the plugin zip {plugin_zip}")
        if fnmatch.fnmatch(f"stellaris-perf-bench-v{m.get('version')}.zip", pattern):
            bad(f"update.asset {pattern!r} also matches the benchmark zip")
    if m.get("homepage") and not str(m["homepage"]).startswith(("https://", "http://")):
        bad("homepage must be an http(s) URL")
    if "seed_files" in m:
        bad("seed_files is schema 1 (it writes into the game folder); use config")

    # the exe build the SDK was made from
    sdk_header = os.path.join(ROOT, "sdk", "stellaris_sdk.hpp")
    with open(sdk_header, encoding="utf-8") as f:
        mt = re.search(r"kExeTimestamp = (0x[0-9A-Fa-f]+)", f.read())
    if not mt:
        bad("kExeTimestamp not found in sdk/stellaris_sdk.hpp")
    else:
        listed = [str(t).lower() for t in m.get("game", {}).get("exe_timestamps", [])]
        if mt.group(1).lower() not in listed:
            bad(f"game.exe_timestamps {listed} lacks the SDK's {mt.group(1)}")

    # nothing for the game folder, no loader or stand-in DLL
    allowed = {"stl-plugin.json", dll, "readme.md", "readme.zh-cn.md", "license"}
    for entry in sorted(os.listdir(folder)):
        low = entry.lower()
        if args.release and low in ("config", "logs"):
            bad(f"{entry} must not be in a release zip (it is the user's)")
            continue
        if low in allowed or low in ("defaults", "config", "data", "logs"):
            continue
        bad(f"unexpected file or folder in the plugin folder: {entry}")

    if problems:
        print("plugin check FAILED:", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1
    print(f"plugin ok: {m['id']} {m['version']}, schema {m['schema']}, exe {', '.join(m['game']['exe_timestamps'])}, "
          f"{len(m.get('config', []))} settings file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
