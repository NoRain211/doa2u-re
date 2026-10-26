# Exports Ghidra function names and tags into tools/doa2u/function_status.json.
# Run from Ghidra's Script Manager against the DOA2.xbe game program. Arg0 is
# optional; without it the repository is found from the open executable.
#
# @category DOA2U
import json
import os
import re
from collections import OrderedDict


NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
DEFAULT = os.path.join("tools", "doa2u", "function_status.json")


def ghidra_global(name):
    scope = globals()
    value = getattr(scope, name, None)
    if value is not None:
        return value
    getter = getattr(scope, "get", None)
    return getter(name) if getter else None


def script_args():
    get_args = ghidra_global("getScriptArgs")
    return get_args() if get_args else []


def current_program():
    program = ghidra_global("currentProgram")
    if program is None:
        raise RuntimeError("run this script inside Ghidra")
    return program


def executable_path():
    path = current_program().getExecutablePath()
    # Ghidra reports Windows paths as /C:/...; strip the leading slash.
    if path.startswith("/") and len(path) > 2 and path[2] == ":":
        path = path[1:]
    return path


def find_repo(start):
    if not start:
        return None
    current = os.path.abspath(start)
    while True:
        shards = os.path.join(current, "tools", "doa2u", "functions")
        if os.path.isdir(shards):
            return current
        parent = os.path.dirname(current)
        if parent == current:
            return None
        current = parent


def load(path):
    if not path or not os.path.isfile(path):
        return {}
    stream = open(path, "rb")
    try:
        entries = json.loads(stream.read().decode("utf-8"))
    finally:
        stream.close()
    return dict((int(entry["start"], 16), entry) for entry in entries)


def load_starts(repo):
    folder = os.path.join(repo, "tools", "doa2u", "functions")
    if not os.path.isdir(folder):
        raise RuntimeError("cannot find %s" % folder)
    starts = set()
    for name in os.listdir(folder):
        if not name.endswith(".json"):
            continue
        stream = open(os.path.join(folder, name), "rb")
        try:
            entries = json.loads(stream.read().decode("utf-8"))
        finally:
            stream.close()
        for entry in entries:
            starts.add(int(entry["start"], 16))
    return starts


def write(path, entries):
    ordered = []
    for address in sorted(entries):
        entry = entries[address]
        value = OrderedDict()
        value["start"] = "0x%08X" % address
        value["name"] = entry["name"]
        for key in ("tag", "status", "source", "symbol"):
            if entry.get(key):
                value[key] = entry[key]
        ordered.append(value)
    stream = open(path, "wb")
    try:
        stream.write((json.dumps(ordered, indent=2) + "\n").encode("utf-8"))
    finally:
        stream.close()
    return ordered


def run():
    executable = executable_path()
    if os.path.basename(executable).lower() != "doa2.xbe":
        raise RuntimeError("open the DOA2.xbe game program, not %s" % executable)

    repo = find_repo(os.path.dirname(executable))
    if not repo:
        repo = find_repo(os.path.dirname(os.path.abspath(__file__)))
    if not repo:
        raise RuntimeError("cannot find tools/doa2u/functions")

    args = script_args()
    path = args[0] if args else os.path.join(repo, DEFAULT)

    entries = load(path)
    starts = load_starts(repo)
    defined = set()
    exported = set()
    outside = set()
    for function in current_program().getFunctionManager().getFunctions(True):
        address = int(function.getEntryPoint().getOffset())
        defined.add(address)
        name = function.getName()
        if name.startswith(("FUN_", "thunk_FUN_", "LAB_", "SUB_")):
            continue
        if not NAME.match(name):
            continue
        if address not in starts:
            outside.add(address)
            continue

        exported.add(address)
        old = entries.get(address, {})
        entry = {"name": name}
        tags = sorted(tag.getName() for tag in function.getTags())
        if tags:
            entry["tag"] = tags[0]
        if old.get("status"):
            entry["status"] = old["status"]
        elif function.getComment():
            entry["status"] = "decompiled"
        for key in ("source", "symbol"):
            if old.get(key):
                entry[key] = old[key]
        entries[address] = entry

    # A default name clears a stale custom name, but a manual binding keeps
    # its descriptive name because its source/symbol is still live.
    for address in defined - exported:
        entry = entries.get(address)
        if entry is not None and not entry.get("source"):
            entries.pop(address)

    ordered = write(path, entries)
    counts = {}
    for entry in ordered:
        status = entry.get("status", "named")
        counts[status] = counts.get(status, 0) + 1
    print("wrote %d functions to %s" % (len(ordered), path))
    print("status: " + ", ".join("%s=%d" % item for item in sorted(counts.items())))
    stale = len(set(entries) - defined)
    if stale:
        print("kept %d entries not defined in Ghidra" % stale)
    if outside:
        print("skipped %d names outside tools/doa2u/functions" % len(outside))


run()
