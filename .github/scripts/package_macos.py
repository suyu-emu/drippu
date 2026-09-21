#!/usr/bin/env python3
"""Build a portable, signed macOS release directory from CMake outputs."""

from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path


SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
FORBIDDEN_PREFIXES = ("/opt/homebrew/", "/usr/local/")


def run(*args: str | Path, capture: bool = False) -> str:
    command = [str(arg) for arg in args]
    if not capture:
        print("+", " ".join(command))
    result = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout if capture else ""


def is_macho(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    result = subprocess.run(
        ["file", "-b", str(path)], check=True, text=True, stdout=subprocess.PIPE
    )
    return "Mach-O" in result.stdout


def macho_files(root: Path) -> list[Path]:
    return sorted((path for path in root.rglob("*") if is_macho(path)), key=str)


def dependencies(binary: Path) -> list[str]:
    lines = run("otool", "-L", binary, capture=True).splitlines()[1:]
    return [line.strip().split(" (", 1)[0] for line in lines if line.strip()]


def rpaths(binary: Path) -> list[str]:
    lines = run("otool", "-l", binary, capture=True).splitlines()
    paths: list[str] = []
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH" and index + 2 < len(lines):
            value = lines[index + 2].strip()
            if value.startswith("path "):
                paths.append(value[5:].split(" (offset", 1)[0])
    return paths


def external_dependency(dependency: str) -> bool:
    return dependency.startswith("/") and not dependency.startswith(SYSTEM_PREFIXES)


def framework_parts(source: Path) -> tuple[Path, Path] | None:
    parts = source.parts
    for index, part in enumerate(parts):
        if part.endswith(".framework"):
            root = Path(*parts[: index + 1])
            relative_binary = Path(*parts[index:])
            return root, relative_binary
    return None


def copy_dependency(source: Path, frameworks: Path) -> Path:
    framework = framework_parts(source)
    if framework:
        framework_root, relative_binary = framework
        destination_root = frameworks / framework_root.name
        if not destination_root.exists():
            print(f"Bundling framework {framework_root}")
            shutil.copytree(framework_root, destination_root, symlinks=True)
        return frameworks / relative_binary

    destination = frameworks / source.name
    if not destination.exists():
        print(f"Bundling library {source}")
        shutil.copy2(source, destination)
        destination.chmod(destination.stat().st_mode | 0o200)
    return destination


def loader_reference(consumer: Path, bundled: Path) -> str:
    relative = os.path.relpath(bundled, consumer.parent)
    return "@loader_path/" + relative


def _executable_path_candidates(
    dependency: str, consumer: Path, app: Path, cli: Path, frameworks: Path
) -> list[Path]:
    """Candidate source files for an @executable_path/ dependency.

    A dylib inside Frameworks can be loaded by either the .app executable
    (Contents/MacOS) or the sibling CLI (output-dir/drippu-cmd), so
    @executable_path resolves differently per loader. The bundled copy in
    Frameworks may also not exist yet when its referrer is processed
    (order-dependent), so fall back to the Homebrew original by basename.
    """
    suffix = dependency.removeprefix("@executable_path/")
    candidates: list[Path] = []
    # Resolved against the .app executable (valid for in-bundle loads).
    candidates.append((app / "Contents" / "MacOS" / suffix).resolve())
    # Resolved against the sibling CLI (output-dir/<cli>).
    candidates.append((cli.parent / suffix).resolve())
    # The common macdeployqt leftover: @executable_path/../Frameworks/<lib>
    # inside a Frameworks dylib means "my sibling in Frameworks".
    if suffix.startswith("../Frameworks/"):
        candidates.append((frameworks / suffix.removeprefix("../Frameworks/")).resolve())
    # Fall back to the development install by basename so bundling does not
    # depend on processing order (the Frameworks copy may not exist yet).
    basename = Path(suffix).name
    for root in (Path("/opt/homebrew/lib"), Path("/usr/local/lib")):
        candidates.append(root / basename)
    return candidates


def dependency_source(
    dependency: str, consumer: Path, app: Path, cli: Path, frameworks: Path
) -> Path | None:
    if external_dependency(dependency):
        return Path(dependency)
    if dependency.startswith("@executable_path/"):
        for candidate in _executable_path_candidates(dependency, consumer, app, cli, frameworks):
            if candidate.exists() and candidate.is_file():
                return candidate
    if dependency.startswith("@loader_path/"):
        # Already loader-relative (correct for Frameworks-internal refs).
        # Resolve to verify it exists; if it does, nothing needs copying.
        candidate = (consumer.parent / dependency.removeprefix("@loader_path/")).resolve()
        if candidate.exists():
            return None
        # Broken @loader_path: try the bundled Frameworks copy / Homebrew by basename.
        basename = Path(dependency.removeprefix("@loader_path/")).name
        for fallback in (frameworks / basename, Path("/opt/homebrew/lib") / basename,
                         Path("/usr/local/lib") / basename):
            if fallback.exists():
                return fallback
        return None
    if dependency.startswith("@rpath/"):
        relative = Path(dependency.removeprefix("@rpath/"))
        for root in (frameworks, Path("/opt/homebrew/lib"), Path("/usr/local/lib")):
            candidate = root / relative
            if candidate.exists():
                return candidate
    return None


def bundle_non_system_libraries(app: Path, cli: Path) -> None:
    frameworks = app / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    queue = deque(macho_files(app) + [cli])
    processed: set[Path] = set()

    while queue:
        binary = queue.popleft()
        if binary in processed or not is_macho(binary):
            continue
        processed.add(binary)

        for dependency in dependencies(binary):
            source = dependency_source(dependency, binary, app, cli, frameworks)
            if source is None:
                continue
            if not source.exists():
                raise RuntimeError(f"missing dependency for {binary}: {dependency}")
            destination = copy_dependency(source, frameworks)
            replacement = loader_reference(binary, destination)
            run("install_name_tool", "-change", dependency, replacement, binary)
            if is_macho(destination):
                queue.append(destination)
            framework = framework_parts(destination)
            if framework:
                queue.extend(macho_files(frameworks / framework[0].name))

        if frameworks in binary.parents:
            install_ids = run("otool", "-D", binary, capture=True).splitlines()[1:]
            if any(external_dependency(install_id.strip()) for install_id in install_ids):
                run("install_name_tool", "-id", loader_reference(binary, binary), binary)

    # Second pass: macdeployqt (and some Homebrew builds) leave
    # @executable_path/../Frameworks/<lib> inside Frameworks dylibs. That
    # resolves for the .app executable but NOT for the sibling CLI
    # (output-dir/drippu-cmd), whose @executable_path is output-dir/ — this
    # caused dyld "Library not loaded: @executable_path/../Frameworks/..."
    # crashes at launch. Rewrite leftovers to @loader_path siblings, which
    # resolve identically no matter which executable loads them.
    for binary in macho_files(frameworks):
        for dependency in dependencies(binary):
            if not dependency.startswith("@executable_path/"):
                continue
            suffix = dependency.removeprefix("@executable_path/")
            target_name = Path(suffix).name
            sibling = (binary.parent / target_name).resolve()
            frameworks_root_hit = frameworks / target_name
            if sibling.exists() or frameworks_root_hit.exists():
                run("install_name_tool", "-change", dependency,
                    f"@loader_path/{target_name}", binary)
            else:
                # Try to bundle the missing sibling from Homebrew, then rewrite.
                for root in (Path("/opt/homebrew/lib"), Path("/usr/local/lib")):
                    source = root / target_name
                    if source.exists():
                        destination = copy_dependency(source, frameworks)
                        run("install_name_tool", "-change", dependency,
                            loader_reference(binary, destination), binary)
                        break

    for binary in macho_files(app) + [cli]:
        for path in rpaths(binary):
            if path.startswith(FORBIDDEN_PREFIXES):
                run("install_name_tool", "-delete_rpath", path, binary)


def reference_exists(dependency: str, consumer: Path, app: Path, cli: Path) -> bool:
    frameworks = app / "Contents" / "Frameworks"
    if dependency.startswith("@loader_path/"):
        return (consumer.parent / dependency.removeprefix("@loader_path/")).resolve().exists()
    if dependency.startswith("@executable_path/"):
        # Frameworks dylibs are loaded by BOTH the .app executable and the
        # sibling CLI, which have different @executable_path values. Require
        # the reference to resolve for both loaders; otherwise it is a latent
        # dyld crash for one of them (and should have been rewritten to
        # @loader_path by bundle_non_system_libraries).
        suffix = dependency.removeprefix("@executable_path/")
        app_hit = (app / "Contents" / "MacOS" / suffix).resolve().exists()
        cli_hit = (cli.parent / suffix).resolve().exists()
        if frameworks in consumer.parents:
            return app_hit and cli_hit
        executable_dir = cli.parent if consumer == cli else app / "Contents" / "MacOS"
        return (executable_dir / suffix).resolve().exists()
    if dependency.startswith("@rpath/"):
        relative = dependency.removeprefix("@rpath/")
        return (frameworks / relative).exists()
    return True


def validate_portability(app: Path, cli: Path, expected_arch: str) -> None:
    failures: list[str] = []
    for binary in macho_files(app) + [cli]:
        archs = run("lipo", "-archs", binary, capture=True).strip().split()
        if expected_arch and expected_arch not in archs:
            failures.append(f"{binary}: expected architecture {expected_arch}, got {' '.join(archs)}")
        for dependency in dependencies(binary):
            if external_dependency(dependency):
                failures.append(f"{binary}: external dependency {dependency}")
            elif dependency.startswith("@") and not reference_exists(dependency, binary, app, cli):
                failures.append(f"{binary}: unresolved bundled dependency {dependency}")
        for path in rpaths(binary):
            if path.startswith(FORBIDDEN_PREFIXES):
                failures.append(f"{binary}: development rpath {path}")
    if failures:
        raise RuntimeError("macOS package is not portable:\n" + "\n".join(failures))


def validate_bundle_metadata(app: Path) -> None:
    with (app / "Contents" / "Info.plist").open("rb") as plist_file:
        info = plistlib.load(plist_file)
    expected = {
        "CFBundleDisplayName": "drippu",
        "CFBundleExecutable": "suyu",
        "CFBundleIdentifier": "dev.drippu.emulator",
        "CFBundleName": "drippu",
    }
    failures = [
        f"{key}: expected {value!r}, got {info.get(key)!r}"
        for key, value in expected.items()
        if info.get(key) != value
    ]
    for key in ("CFBundleShortVersionString", "CFBundleVersion", "LSMinimumSystemVersion"):
        if not info.get(key):
            failures.append(f"{key}: missing or empty")
    if failures:
        raise RuntimeError("invalid macOS bundle metadata:\n" + "\n".join(failures))


def sign_path(path: Path, identity: str, entitlements: Path | None = None) -> None:
    args: list[str | Path] = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        args.extend(["--options", "runtime", "--timestamp"])
    if entitlements is not None:
        args.extend(["--entitlements", entitlements])
    args.append(path)
    run(*args)


def sign_package(app: Path, cli: Path, identity: str, entitlements: Path) -> None:
    main_executable = app / "Contents" / "MacOS" / "suyu"
    frameworks = sorted(app.rglob("*.framework"), key=lambda path: len(path.parts), reverse=True)
    framework_members = tuple(framework.resolve() for framework in frameworks)

    nested = []
    for binary in macho_files(app):
        if binary == main_executable:
            continue
        resolved = binary.resolve()
        if any(framework in resolved.parents for framework in framework_members):
            continue
        nested.append(binary)

    for binary in sorted(nested, key=lambda path: len(path.parts), reverse=True):
        sign_path(binary, identity)
    for framework in frameworks:
        sign_path(framework, identity)
    sign_path(main_executable, identity, entitlements)
    # Signing the bundle re-signs its main executable, so the final bundle
    # signature must carry the JIT entitlements as well.
    sign_path(app, identity, entitlements)
    sign_path(cli, identity, entitlements)

    run("codesign", "--verify", "--deep", "--strict", "--verbose=2", app)
    run("codesign", "--verify", "--strict", "--verbose=2", cli)
    for executable in (main_executable, cli):
        result = subprocess.run(
            ["codesign", "-d", "--entitlements", "-", str(executable)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        displayed = result.stdout + result.stderr
        for required in (
            "com.apple.security.cs.allow-jit",
            "com.apple.security.cs.allow-unsigned-executable-memory",
        ):
            if required not in displayed:
                raise RuntimeError(f"{executable}: signed without required entitlement {required}")


def locate_macdeployqt() -> str:
    executable = shutil.which("macdeployqt")
    if executable:
        return executable
    for candidate in (
        Path("/opt/homebrew/opt/qt/bin/macdeployqt"),
        Path("/usr/local/opt/qt/bin/macdeployqt"),
        Path("/opt/homebrew/opt/qt@6/bin/macdeployqt"),
        Path("/usr/local/opt/qt@6/bin/macdeployqt"),
    ):
        if candidate.exists():
            return str(candidate)
    raise RuntimeError("macdeployqt was not found; install Qt 6 or add it to PATH")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--expected-arch", default="arm64")
    parser.add_argument(
        "--entitlements",
        type=Path,
        default=Path("dist/macos/drippu.entitlements"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if sys.platform != "darwin":
        raise RuntimeError("package_macos.py must run on macOS")
    if not args.app.is_dir() or not args.cli.is_file():
        raise RuntimeError("both --app and --cli must point to built release outputs")

    if args.output_dir.exists():
        shutil.rmtree(args.output_dir)
    args.output_dir.mkdir(parents=True)
    app = args.output_dir / "drippu.app"
    cli = args.output_dir / "drippu-cmd"
    shutil.copytree(args.app, app, symlinks=True)
    shutil.copy2(args.cli, cli)

    smoke_binary = app / "Contents" / "MacOS" / "macos_moltenvk_smoke"
    smoke_binary.unlink(missing_ok=True)

    deploy_args: list[str | Path] = [
        locate_macdeployqt(),
        app,
        "-always-overwrite",
        "-no-strip",
        "-no-codesign",
    ]
    for library_root in (Path("/opt/homebrew/lib"), Path("/usr/local/lib")):
        if library_root.is_dir():
            deploy_args.append(f"-libpath={library_root}")
    run(*deploy_args)
    bundle_non_system_libraries(app, cli)
    validate_bundle_metadata(app)
    validate_portability(app, cli, args.expected_arch)

    identity = os.environ.get("MACOS_SIGNING_IDENTITY", "").strip() or "-"
    sign_package(app, cli, identity, args.entitlements)

    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        args.archive.unlink(missing_ok=True)
        run("tar", "-czf", args.archive, "-C", args.output_dir, ".")

    print(f"macOS release package ready at {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
