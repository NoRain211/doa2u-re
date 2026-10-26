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


def script_args():
    try:
        return getScriptArgs() or []
    except NameError:
        return []


def executable_path():
    path = currentProgram.getExecutablePath()
    # Ghidra reports Windows paths as /C:/...; strip the leading slash.
    if path.startswith("/") and len(path) > 2 and path[2] == ":":
        path = path[1:]
    return path


def find_registry(start):
    if not start:
        return None
    current = os.path.abspath(start)
    while True:
        candidate = os.path.join(current, DEFAULT)
        shards = os.path.join(current, "tools", "doa2u", "functions")
        if os.path.isfile(candidate) or os.path.isdir(shards):
            return candidate
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

    args = script_args()
    path = args[0] if args else find_registry(os.path.dirname(executable))
    if not path:
        path = find_registry(os.path.dirname(os.path.abspath(__file__)))
    if not path:
        raise RuntimeError("pass tools/doa2u/function_status.json as argument 0")

    entries = load(path)
    found = set()
    for function in currentProgram.getFunctionManager().getFunctions(True):
        name = function.getName()
        if name.startswith(("FUN_", "thunk_FUN_", "LAB_", "SUB_")):
            continue
        if not NAME.match(name):
            continue

        address = int(function.getEntryPoint().getOffset())
        found.add(address)
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

    ordered = write(path, entries)
    counts = {}
    for entry in ordered:
        status = entry.get("status", "named")
        counts[status] = counts.get(status, 0) + 1
    print("wrote %d functions to %s" % (len(ordered), path))
    print("status: " + ", ".join("%s=%d" % item for item in sorted(counts.items())))
    stale = len(set(entries) - found)
    if stale:
        print("kept %d entries not defined in Ghidra" % stale)


run()
