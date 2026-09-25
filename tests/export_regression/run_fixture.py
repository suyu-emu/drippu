#!/usr/bin/env python3
"""Exercise the exporter's real ExeFS helpers with small synthetic files."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from compile_object import ROOT, cache_value, msvc_environment, resolve_build

SOURCE = ROOT / "src/suyu/game_export.cpp"


def extract(text: str, start: str, end: str) -> str:
    return text[text.index(start):text.index(end, text.index(start))]


def resolve_qt_root(build: Path, value: str | None) -> Path:
    if value:
        root = Path(value).resolve()
    else:
        core_dir = cache_value(build, "Qt6Core_DIR")
        if not core_dir:
            raise SystemExit("Pass --qt-root or set QT_ROOT; Qt6Core_DIR is absent from CMakeCache.txt")
        root = Path(core_dir).resolve().parents[2]
    if not (root / "include/QtCore/QCoreApplication").is_file() or not (
        root / "lib/Qt6Core.lib"
    ).is_file() or not (root / "bin/Qt6Core.dll").is_file():
        raise SystemExit(f"Qt root lacks Qt6Core headers, import library, or DLL: {root}")
    return root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=os.environ.get("SUYU_EXPORT_TEST_BUILD"))
    parser.add_argument("--qt-root", default=os.environ.get("QT_ROOT"))
    parser.add_argument("--vsdevcmd", default=os.environ.get("VSDEVCMD"))
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("The fixture requires a configured Windows Qt/MSVC toolchain.")
    build = resolve_build(args.build)
    qt_root = resolve_qt_root(build, args.qt_root)
    source = SOURCE.read_text(encoding="utf-8")
    helpers = "\n".join((
        extract(source, "static bool CopyFileReplacingExisting(", "// True when both paths"),
        extract(source, "static bool CopyDeconstructedExeFs(", "// Fingerprint the effective files"),
        # Stops before SeedPortableConfig, which needs suyu's settings.
        extract(source, "static QString HashExeFsFiles(", "static bool SeedPortableConfig("),
        extract(source, "static bool HasUnpairedStandaloneNcaUpdate(",
                "static FileSys::VirtualFile ExtractRomFsFromRom("),
    ))
    pair_guard = extract(
        source, "const auto validate_base_fallback =", "const auto romfs_from_nsp ="
    )
    prefix = r'''
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#define LOG_ERROR(...) do {} while (false)
using u64 = std::uint64_t;
using u32 = std::uint32_t;
namespace FileSys {
struct VfsFile {
    std::string name;
    std::vector<unsigned char> contents;
    std::string GetName() const { return name; }
    u64 GetSize() const { return contents.size(); }
    std::vector<unsigned char> ReadBytes(u64 size, u64 offset) const {
        if (offset + size > contents.size()) return {};
        return {contents.begin() + offset, contents.begin() + offset + size};
    }
};
struct VfsDir {
    std::vector<std::shared_ptr<VfsFile>> files;
    std::vector<std::shared_ptr<VfsFile>> GetFiles() const { return files; }
};
using VirtualDir = std::shared_ptr<VfsDir>;
using VirtualFile = std::shared_ptr<VfsFile>;
}
'''
    body = r'''
static bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const QString root = QString::fromLocal8Bit(argv[1]);
    const QString src = root + "/source";
    const QString dst = root + "/export/exefs";
    if (!QDir().mkpath(src) || !writeFile(src + "/main", "code-v1") ||
        !writeFile(src + "/subsdk0", "old-module") ||
        !writeFile(src + "/romfs.bin", "synthetic-romfs")) return 3;
    const QString first = HashExeFsFiles({}, src);
    if (first.isEmpty() || !CopyDeconstructedExeFs(src, dst) ||
        !QFile::exists(dst + "/main") || !QFile::exists(dst + "/subsdk0") ||
        QFile::exists(dst + "/romfs.bin")) return 4;
    if (!writeFile(src + "/main", "code-v2") || !QFile::remove(src + "/subsdk0")) return 5;
    const QString second = HashExeFsFiles({}, src);
    if (second.isEmpty() || second == first || !CopyDeconstructedExeFs(src, dst) ||
        !QFile::exists(dst + "/main") || QFile::exists(dst + "/subsdk0")) return 6;
    QFile current(dst + "/main");
    if (!current.open(QIODevice::ReadOnly) || current.readAll() != "code-v2") return 7;
    if (HashExeFsFiles({}, src) != second) return 8;
    if (CopyDeconstructedExeFs(src, src) || !QFile::exists(src + "/main")) return 9;
    if (CopyDeconstructedExeFs(src.toUpper(), src) || !QFile::exists(src + "/main")) return 9;
    const QString pkg = root + "/export";
    const QString cache = pkg + "/aot_cache";
    if (!QDir().mkpath(cache + "/launcher") ||
        !writeFile(pkg + "/Synthetic.exe", "old-build") ||
        !writeFile(pkg + "/launch.bat", "old-launch") ||
        !writeFile(pkg + "/libcrypto-3-x64.dll", "old-dll") ||
        !writeFile(cache + "/launcher/static_launcher.exe", "old-static")) return 10;
    if (!PrepareAotSourcePackage(pkg, "Synthetic", cache) ||
        QFile::exists(pkg + "/Synthetic.exe") || QFile::exists(pkg + "/launch.bat") ||
        QFile::exists(pkg + "/libcrypto-3-x64.dll") || QDir(cache + "/launcher").exists()) return 11;
    QFile readme(pkg + "/README_NATIVE_EXPORT.txt");
    if (!readme.open(QIODevice::ReadOnly) ||
        !readme.readAll().contains("no compiled launcher is included")) return 12;
    auto base_dir = std::make_shared<FileSys::VfsDir>();
    auto current_dir = std::make_shared<FileSys::VfsDir>();
    auto base_main = std::make_shared<FileSys::VfsFile>();
    base_main->name = "main"; base_main->contents = {'b','a','s','e'};
    auto current_main = std::make_shared<FileSys::VfsFile>();
    current_main->name = "main"; current_main->contents = {'u','p','d','t'};
    base_dir->files.push_back(base_main);
    current_dir->files.push_back(current_main);
    FileSys::VirtualDir effective_exefs = base_dir;
'''+pair_guard+r'''
    auto base_romfs = std::make_shared<FileSys::VfsFile>();
    auto updated_romfs = std::make_shared<FileSys::VfsFile>();
    if (validate_base_fallback(base_dir, base_romfs, base_romfs) != base_romfs) return 13;
    effective_exefs = current_dir;
    if (validate_base_fallback(base_dir, base_romfs, base_romfs) != nullptr ||
        validate_base_fallback(current_dir, base_romfs, updated_romfs) != updated_romfs) return 13;
    if (HasUnpairedStandaloneNcaUpdate(base_romfs, base_romfs) ||
        !HasUnpairedStandaloneNcaUpdate(base_romfs, updated_romfs)) return 14;
    const QByteArray fallback_manifest = R"({"fallback_enabled":true,"fallback_modules":["sdk"]})";
    QStringList cached_modules;
    if (!ReadCachedFallbackPolicy(fallback_manifest, true, cached_modules) ||
        cached_modules != QStringList{QStringLiteral("sdk")} ||
        ReadCachedFallbackPolicy(fallback_manifest, false, cached_modules) ||
        ReadCachedFallbackPolicy(R"({"fallback_enabled":true,"fallback_modules":[5]})",
                                 true, cached_modules) ||
        ReadCachedFallbackPolicy(R"({"fallback_enabled":false,"fallback_modules":["sdk"]})",
                                 false, cached_modules)) return 15;
    const QString config_path = pkg + "/user/config/sdl2-config.ini";
    if (!WritePortableVersionOverride(config_path, 42, "1.0")) return 16;
    {
        QSettings ini(config_path, QSettings::IniFormat);
        if (ini.value("System/application_version_override").toUInt() != 42) return 17;
        ini.setValue("Other/retained", "yes");
        ini.sync();
    }
    if (!WritePortableVersionOverride(config_path, 0, {})) return 18;
    {
        QSettings ini(config_path, QSettings::IniFormat);
        if (ini.contains("System/application_version_override") ||
            ini.contains("System/application_display_version_override") ||
            ini.value("Other/retained").toString() != "yes") return 19;
    }
    return 0;
}
'''
    env = msvc_environment(build, args.vsdevcmd)
    with tempfile.TemporaryDirectory(prefix="suyu-export-fixture-") as temp:
        directory = Path(temp)
        cpp = directory / "fixture.cpp"
        exe = directory / "fixture.exe"
        cpp.write_text(prefix + helpers + body, encoding="utf-8")
        command = [
            str(Path(env["VCTOOLSINSTALLDIR"]) / "bin/Hostx64/x64/cl.exe"),
            "/nologo", "/EHsc", "/std:c++20", "/Zc:__cplusplus", "/utf-8", "/MD",
            f"/I{qt_root / 'include'}", f"/I{qt_root / 'include/QtCore'}",
            f"/I{qt_root / 'mkspecs/win32-msvc'}", str(cpp),
            "/link", f"/LIBPATH:{qt_root / 'lib'}", "Qt6Core.lib", f"/OUT:{exe}",
        ]
        subprocess.run(command, cwd=directory, env=env, check=True)
        env["PATH"] = str(qt_root / "bin") + os.pathsep + env.get("PATH", "")
        subprocess.run([exe, directory / "fixture"], env=env, check=True)
    print("PASS: ExeFS/cache, Source cleanup, NCA pairing, fallback policy, version reset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
