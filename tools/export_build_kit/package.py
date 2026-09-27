"""Freeze Ninja's resolved host link inputs; no source/build paths survive."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

REVISION = "suyu-aot-kit-abi6-fm1-gg1-fpx1-control-r2"


def ninja_words(value):
    # Ninja escapes spaces/colons/dollars independently of Windows quoting.
    return [re.sub(r"\$(.)", r"\1", word) for word in
            re.findall(r"(?:\$.|[^\s])+", value)]


def windows_words(value):
    return [re.sub(r"\$(.)", r"\1", word.strip('"')) for word in re.findall(r'"[^"]*"|[^\s]+', value)]


def link_edge(manifest, target):
    lines = manifest.replace("$\n", "").splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("build ") or target not in line:
            continue
        # Rule separator is an unescaped colon followed by whitespace.
        match = re.match(r"build (.*?)(?<!\$): (\S+) (.*)", line)
        if not match or "EXECUTABLE_LINKER" not in match[2]:
            continue
        values = {}
        for following in lines[index + 1:]:
            if not following.startswith("  "):
                break
            key, separator, value = following.strip().partition(" = ")
            if separator:
                values[key] = value
        objects = [word for word in ninja_words(match[3].split(" | ")[0])
                   if word.lower().endswith((".obj", ".res"))]
        return objects, windows_words(values.get("LINK_LIBRARIES", "")), windows_words(values.get("LINK_FLAGS", ""))
    raise ValueError("No resolved executable link edge for " + target)


def cmake_quote(value):
    if any(char in value for char in ';\n\r"'):
        raise ValueError("Unsupported CMake path or flag: " + value)
    return '"' + value.replace('\\', '/') + '"'


def package(build, output, revision):
    if revision != REVISION:
        raise ValueError("Unsupported build-kit revision")
    build, output = Path(build).resolve(), Path(output).resolve()
    manifest = (build / "build.ninja").read_text(encoding="utf-8")
    output.mkdir(parents=True, exist_ok=True)
    copied = {}
    def freeze(raw):
        path = Path(raw)
        if not path.is_absolute():
            path = build / path
        path = path.resolve()
        if not path.is_file():
            # Only bare names may be resolved by the installed Windows SDK.
            if '/' not in raw and '\\' not in raw and raw.lower().endswith('.lib'):
                return raw
            raise ValueError("Missing link input: " + raw)
        key = str(path)
        if key not in copied:
            relative = "inputs/" + hashlib.sha256(key.encode()).hexdigest()[:20] + '-' + path.name
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            copied[key] = relative
        return '${CMAKE_CURRENT_LIST_DIR}/' + copied[key]
    for mode in ('strict', 'hybrid'):
        objects, libraries, flags = link_edge(manifest, 'suyu-export-host-' + mode + '.exe')
        probes = [obj for obj in objects if 'registry_probe.c.obj' in obj]
        if len(probes) != 1:
            raise ValueError("Expected exactly one excluded registry probe object")
        objects = [freeze(obj) for obj in objects if obj not in probes]
        if not objects:
            raise ValueError("Host object list is empty")
        libraries = [freeze(lib) for lib in libraries]
        for flag in flags:
            if '\\' in flag or re.search(r'[A-Za-z]:[/\\]', flag) or '$' in flag:
                raise ValueError("Nonportable linker flag: " + flag)
        content = '\n'.join('set(KIT_' + key + '\n  ' + '\n  '.join(map(cmake_quote, vals)) + '\n)' for key, vals in
                            [('OBJECTS', objects), ('LIBRARIES', libraries), ('LINK_OPTIONS', flags)])
        (output / (mode + '.cmake')).write_text(content + '\n', encoding='utf-8')
    shutil.copy2(Path(__file__).with_name('CMakeLists.txt'), output / 'CMakeLists.txt')
    module_project = Path(__file__).resolve().parents[2] / 'src/suyu_cmd/recomp_modules/CMakeLists.txt'
    scaffold = output / 'src/suyu_cmd/recomp_modules'
    scaffold.mkdir(parents=True, exist_ok=True)
    shutil.copy2(module_project, scaffold / 'CMakeLists.txt')
    (output / 'revision.txt').write_text(REVISION + '\n', encoding='ascii')
    (output / 'manifest.json').write_text(json.dumps({'revision': REVISION, 'inputs':
        {relative: hashlib.sha256((output / relative).read_bytes()).hexdigest() for relative in copied.values()}}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--revision', default=REVISION)
    args = parser.parse_args()
    package(args.build, args.output, args.revision)
