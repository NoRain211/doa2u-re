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
GRID_START = "<!-- function-status-grid:start -->"
GRID_END = "<!-- function-status-grid:end -->"
AREA_LABELS = {
    "GAME": "Game",
    "CRI_MIDDLEWARE": "CRI middleware",
    "SDK_XAPI_CRT": "SDK / CRT",
    "UNTAGGED": "Untagged",
}
AREA_ORDER = ("GAME", "CRI_MIDDLEWARE", "SDK_XAPI_CRT")
ICONS = {
    "named": u"\u2b1c",
    "decompiled": u"\U0001f7e8",
    "bound": u"\U0001f7e6",
    "tested": u"\U0001f7e9",
}
STATUS_ORDER = ("named", "decompiled", "bound", "tested")
SEPARATOR = u" \u00b7 "
DASH = u" \u2014 "
GRID_WIDTH = 40


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


def thousands(value):
    return u"{:,}".format(value)


def percentage(value, total):
    if not total:
        return 0
    return (value * 100 + total // 2) // total


def summary(row):
    total = sum(row.values())
    if not total:
        return u"0 tracked"
    parts = [
        u"%s tracked" % thousands(total),
        u"%s named" % thousands(row.get("named", 0)),
    ]
    for status in ("decompiled", "bound", "tested"):
        if row.get(status):
            parts.append(u"%s %s" % (thousands(row[status]), status))
    ported = row.get("decompiled", 0) + row.get("bound", 0) + row.get("tested", 0)
    parts.append(u"%d%% decompiled or ported" % percentage(ported, total))
    return SEPARATOR.join(parts)


def status_grid(entries):
    counts = {}
    for entry in entries:
        tag = entry.get("tag", "UNTAGGED")
        status = entry.get("status", "named")
        if status not in ICONS:
            status = "named"
        row = counts.setdefault(tag, {})
        row[status] = row.get(status, 0) + 1

    tags = list(AREA_ORDER)
    for tag in sorted(counts):
        if tag not in tags and tag != "UNTAGGED":
            tags.append(tag)
    tags.append("UNTAGGED")

    lines = [
        u"Each square is one tracked function. " + SEPARATOR.join(
            u"%s %s" % (ICONS[status], status) for status in STATUS_ORDER
        ) + u".",
        u"",
    ]
    total = {status: 0 for status in STATUS_ORDER}
    for tag in tags:
        row = counts.get(tag, {})
        for status in STATUS_ORDER:
            total[status] += row.get(status, 0)
        label = AREA_LABELS.get(tag, tag.replace("_", " ").title())
        lines.append(u"**%s**%s%s" % (label, DASH, summary(row)))
        squares = u"".join(
            ICONS[status] * row.get(status, 0) for status in STATUS_ORDER
        )
        for index in range(0, len(squares), GRID_WIDTH):
            lines.append(squares[index:index + GRID_WIDTH] + u"<br>")
        lines.append(u"")
    lines.append(u"**Total**%s%s" % (DASH, summary(total)))
    return u"\n".join(lines).rstrip()


def write_readme(repo, entries):
    path = os.path.join(repo, "README.md")
    if not os.path.isfile(path):
        print("README grid skipped: README.md not found")
        return
    stream = open(path, "rb")
    try:
        text = stream.read().decode("utf-8")
    finally:
        stream.close()
    if GRID_START not in text or GRID_END not in text:
        print("README grid skipped: markers not found")
        return
    start = text.index(GRID_START) + len(GRID_START)
    end = text.index(GRID_END, start)
    updated = text[:start] + "\n" + status_grid(entries) + "\n" + text[end:]
    if updated == text:
        return
    stream = open(path, "wb")
    try:
        stream.write(updated.encode("utf-8"))
    finally:
        stream.close()
    print("updated README grid")


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
    write_readme(repo, ordered)
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
