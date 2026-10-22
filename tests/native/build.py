#!/usr/bin/env python3
"""Incremental build half of run.sh; suites arrive as one whitespace-separated line each."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from functools import cache
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


@cache
def digest(filename):
    try:
        return hashlib.sha256(Path(filename).read_bytes()).hexdigest()
    except FileNotFoundError:
        return None


def stamp(path):
    try:
        info = path.stat()
        return [info.st_size, info.st_mtime_ns]
    except FileNotFoundError:
        return None


def dependencies(filename, msvc):
    text = filename.read_text(encoding="utf-8-sig")
    if msvc:
        data = json.loads(text)["Data"]
        return [data["Source"], *data["Includes"]]
    # -MT fixes the target to 'object', avoiding drive-letter colons on Windows.
    text = re.sub(r"\\\r?\n", "", text).partition(":")[2]
    return [re.sub(r"\\([\s#:\\])", r"\1", word).replace("$$", "$")
            for word in re.findall(r"(?:\\.|[^\s])+", text)]


def current(entry, output):
    return bool(entry and entry["output"] is not None and entry["output"] == stamp(output)
                and all(digest(path) == value and value is not None
                        for path, value in entry["dependencies"].items()))


def save_state(path, state):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2), encoding="utf-8")
    temporary.replace(path)


def msvc_environment(root):
    script = Path(root) / "VC/Auxiliary/Build/vcvars64.bat"
    # cmd's `set` output preserves values containing '=' and avoids shell interpolation
    # of any compiler arguments; compilation itself uses argv/response files below.
    result = subprocess.run(
        f'cmd.exe /d /s /c "call "{script}" >nul && set"',
        check=True, stdout=subprocess.PIPE, text=True)
    return {key.upper(): value for line in result.stdout.splitlines()
            if "=" in line and not line.startswith("=")
            for key, value in [line.split("=", 1)]}


def build(args, suites, directory):
    msvc = args.msvc is not None
    environment = msvc_environment(args.msvc) if msvc else dict(os.environ)
    compiler = ["cl.exe"] if msvc else shlex.split(args.cxx)
    if not compiler:
        raise RuntimeError("CXX must name a compiler")
    resolved = shutil.which(compiler[0], path=environment.get("PATH"))
    if not resolved:
        raise RuntimeError(f"Compiler not found: {compiler[0]}")
    compiler[0] = resolved
    include_dirs = ["stubs", "../../src", "../../sdk", "../../tools/browser_shared", ".deps"]
    if msvc:
        flags = ["/nologo", "/std:c++latest", "/EHsc"]
        for path in include_dirs:
            flags.extend(["/I", str(Path(path).resolve())])
        flags.extend(["/FI", str(Path("stubs/pch.h").resolve())])
    else:
        flags = ["-std=c++2b", "-Wall", "-Wextra", "-g"]
        for path in include_dirs:
            flags.extend(["-I", path])
        flags.extend(["-include", "stubs/pch.h"])

    tools = [resolved]
    if msvc:
        tools.append(shutil.which("link.exe", path=environment.get("PATH")))
    else:
        tools.append(subprocess.check_output(
            [*compiler, "-print-prog-name=ld"], env=environment, text=True).strip())
    config = {
        "runner": digest(str(Path(__file__).resolve())),
        "compiler": compiler, "flags": flags, "cwd": str(Path.cwd()),
        "tools": {tool: stamp(Path(shutil.which(tool, path=environment.get("PATH")) or tool))
                  for tool in tools if tool},
        "environment": {key: environment.get(key) for key in (
            "PATH", "INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
            "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "SDKROOT")},
    }
    state_file = directory / "incremental.json"
    try:
        state = json.loads(state_file.read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError):
        state = {}
    if args.clean or state.get("config") != config:
        state = {"config": config, "objects": {}, "suites": {}}

    sources = list(dict.fromkeys(source for group in suites.values() for source in group))
    objects = {}
    depfiles = {}
    names = set()
    depdir = directory / "deps"
    depdir.mkdir(exist_ok=True)
    (directory / "obj").mkdir(exist_ok=True)
    for source in sources:
        # MSVC /MP emits objects using source basenames. Reject collisions on either host.
        name = Path(source).stem if msvc else hashlib.sha256(source.encode()).hexdigest()[:20]
        if name.casefold() in names:
            raise RuntimeError(f"Duplicate object name: {source}")
        names.add(name.casefold())
        objects[source] = directory / "obj" / (name + (".obj" if msvc else ".o"))
        depfiles[source] = depdir / (Path(source).name + ".json" if msvc else name + ".d")

    # Only runner-owned build products are pruned; no recursive delete of .build.
    outputs = {directory / (name + (".exe" if os.name == "nt" else "")) for name in suites}
    keep = set(objects.values()) | set(depfiles.values()) | outputs
    for path in [*(directory / "obj").glob("*.obj"), *(directory / "obj").glob("*.o"),
                 *depdir.glob("*.json"), *depdir.glob("*.d"),
                 *directory.glob("*_tests"), *directory.glob("*_tests.exe")]:
        if path not in keep and path.is_file():
            path.unlink()
    state["objects"] = {key: value for key, value in state["objects"].items() if key in sources}
    state["suites"] = {key: value for key, value in state["suites"].items() if key in suites}
    dirty = [source for source in sources if not current(state["objects"].get(source), objects[source])]
    print(f"== compiling {len(dirty)}/{len(sources)} translation units ({'MSVC' if msvc else 'C++'}) ==", flush=True)
    # Invalidate receipts before invoking tools. Failed/interrupted builds cannot reuse
    # an old receipt even if a compiler or linker leaves its previous output behind.
    for source in dirty:
        state["objects"].pop(source, None)
    save_state(state_file, state)
    if dirty:
        if msvc:
            response = directory / "compile.rsp"
            argv = [*flags, "/MP", "/c", "/sourceDependencies", str(depdir) + os.sep,
                    "/Fo" + str(directory / "obj") + os.sep,
                    *(str(Path(source).resolve()) for source in dirty)]
            response.write_text(subprocess.list2cmdline(argv), encoding="utf-8-sig")
            subprocess.run([*compiler, "@" + str(response)], env=environment, check=True)
        else:
            def compile_one(source):
                subprocess.run([*compiler, *flags, "-MD", "-MF", str(depfiles[source]),
                                "-MT", "object", "-c", source, "-o", str(objects[source])],
                               env=environment, check=True)
            with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
                list(pool.map(compile_one, dirty))
        digest.cache_clear()
        for source in dirty:
            inputs = {str(Path(path).resolve()) for path in dependencies(depfiles[source], msvc)}
            inputs.add(str(Path(source).resolve()))
            inputs.add(str(Path("stubs/pch.h").resolve()))
            state["objects"][source] = {
                "output": stamp(objects[source]),
                "dependencies": {path: digest(path) for path in sorted(inputs)},
            }
        save_state(state_file, state)

    pending = []
    for name, group in suites.items():
        output = directory / (name + (".exe" if os.name == "nt" else ""))
        inputs = {source: state["objects"][source]["output"] for source in group}
        entry = state["suites"].get(name)
        if (not entry or entry["sources"] != group or entry["inputs"] != inputs
                or entry["output"] != stamp(output) or any(source in dirty for source in group)):
            pending.append((name, group, output, inputs))
            state["suites"].pop(name, None)
    save_state(state_file, state)
    print(f"== linking {len(pending)}/{len(suites)} suites ==", flush=True)
    for name, group, output, inputs in pending:
        argv = [str(objects[source]) for source in group]
        argv = ["/nologo", *argv, "/Fe:" + str(output)] if msvc else [*flags, *argv, "-o", str(output)]
        subprocess.run([*compiler, *argv], env=environment, check=True)
        state["suites"][name] = {"sources": group, "inputs": inputs, "output": stamp(output)}
        save_state(state_file, state)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--msvc", help="Visual Studio root (uses vcvars64)")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--clean", action="store_true", help="rebuild all objects and suites")
    args = parser.parse_args()
    suites = {}
    for line in sys.stdin:
        name, *sources = line.split()
        if not re.fullmatch(r"[a-z0-9_]+_tests", name) or not sources or name in suites:
            parser.error(f"Invalid or duplicate suite: {name}")
        suites[name] = sources
    directory = Path(".build").resolve()
    directory.mkdir(exist_ok=True)
    # OS locks are released on process exit, including interrupted builds.
    with (directory / "build.lock").open("a+b") as lock:
        try:
            if os.name == "nt":
                import msvcrt
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise RuntimeError("Another native test build is using .build; wait for it to finish")
        build(args, suites, directory)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"native build failed: {error}", file=sys.stderr)
        sys.exit(1)
