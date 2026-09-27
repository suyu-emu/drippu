"""Synthetic relocation checks; no emulator build or copyrighted game input."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import shutil
import subprocess
import json

spec = importlib.util.spec_from_file_location('kit_package', Path(__file__).resolve().parents[2] / 'tools/export_build_kit/package.py')
kit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kit)


class PackageTests(unittest.TestCase):
    def write_compiler_metadata(self, build):
        metadata = build / 'CMakeFiles/fixture/CMakeCXXCompiler.cmake'
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text('set(CMAKE_CXX_COMPILER_VERSION "19.44.35207")\n')

    @unittest.skipUnless(shutil.which('cl'), 'Run from an x64 Visual Studio developer environment')
    def test_real_windows_link_after_producer_tree_removed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, build, output = root / 'producer with spaces', root / 'build with spaces', root / 'kit [relocated] folder'
            source.mkdir()
            (source / 'host.c').write_text('extern int game(void); int main(void) { return game(); }')
            (source / 'registry_probe.c').write_text('int game(void) { return 1; }')
            (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(fixture C CXX)\nforeach(mode strict hybrid)\nadd_executable(suyu-export-host-${mode} host.c registry_probe.c)\nendforeach()\n')
            def run(*args):
                result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode:
                    print(result.stdout.decode(errors='replace'), flush=True)
                    raise subprocess.CalledProcessError(result.returncode, args, output=result.stdout)
            run('cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
            run('cmake', '--build', str(build))
            kit.package(build, output, kit.REVISION)
            self.assertEqual(json.loads((output / 'manifest.json').read_text())['producer_compiler_version'],
                             kit.producer_compiler_version(build))
            # Rename both producer locations: none of their original paths exist.
            source.rename(root / 'hidden-source')
            build.rename(root / 'hidden-build')
            export = root / 'export with spaces [hybrid]'
            (export / 'main').mkdir(parents=True)
            for header in ['recomp_abi_v4.h', 'recomp_guard_v2.h', 'recomp_fastmem_v1.h', 'recomp_features_v1.h', 'recomp_guard_gen_v1.h', 'recomp_fpx_v1.h']:
                (export / header).write_text('/* synthetic fixture */')
            (export / 'recomp_modules.cmake').write_text('set(SUYU_RECOMP_MODULES main)')
            (export / 'recomp_registration.c').write_text('extern int fixture_module(void); int game(void) { return fixture_module(); }')
            (export / 'main/recomp_runtime.h').write_text('#define RECOMP_IMAGE_ABI 6\n/* tpidrro_el0 */')
            (export / 'main/module.c').write_text('int fixture_module(void) { return 0; }')
            (export / 'main/CMakeLists.txt').write_text('add_library(recomp_static_main STATIC module.c)\ntarget_compile_options(recomp_static_main PRIVATE /O2)')
            for mode in ['strict', 'hybrid']:
                consumer = root / ('consumer-' + mode)
                run('cmake', '-S', str(output), '-B', str(consumer), '-G', 'Ninja', '-DSUYU_CMD_RECOMP_DIR=' + str(export), '-DSUYU_EXPORT_BUILD_KIT_REVISION=' + kit.REVISION, '-DSUYU_RECOMP_HYBRID=' + ('ON' if mode == 'hybrid' else 'OFF'))
                self.assertIn('CMAKE_BUILD_TYPE:STRING=Release', (consumer / 'CMakeCache.txt').read_text())
                run('cmake', '--build', str(consumer))
                run(str(consumer / 'bin/suyu-cmd-static.exe'))

            def assert_configure_rejected(name, diagnostic, configuration='Release'):
                consumer = root / ('rejected-' + name)
                result = subprocess.run(
                    ['cmake', '-S', str(output), '-B', str(consumer), '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=' + configuration, '-DSUYU_CMD_RECOMP_DIR=' + str(export),
                     '-DSUYU_EXPORT_BUILD_KIT_REVISION=' + kit.REVISION],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(diagnostic, ' '.join(result.stdout.split()))
                self.assertFalse((consumer / 'build.ninja').exists(), result.stdout)
                self.assertFalse((consumer / 'bin/suyu-cmd-static.exe').exists())

            assert_configure_rejected('debug', 'use Release instead of Debug', 'Debug')
            manifest_path = output / 'manifest.json'
            original_manifest = manifest_path.read_text()
            producer_manifest = json.loads(original_manifest)
            producer_manifest['producer_compiler_version'] = '99.0.0'
            manifest_path.write_text(json.dumps(producer_manifest))
            try:
                assert_configure_rejected('old-compiler', 'Update MSVC Build Tools:')
            finally:
                manifest_path.write_text(original_manifest)
            del producer_manifest['producer_compiler_version']
            manifest_path.write_text(json.dumps(producer_manifest))
            try:
                assert_configure_rejected('missing-compiler-version', 'Build kit is missing a valid producer compiler version')
            finally:
                manifest_path.write_text(original_manifest)

            for name in ['recomp_fastmem_v1.h', 'recomp_guard_gen_v1.h', 'recomp_fpx_v1.h']:
                with self.subTest(missing_handshake=name):
                    header = export / name
                    original = header.read_bytes()
                    header.unlink()
                    try:
                        assert_configure_rejected(name, 'Missing matched ABI 6 handshake: ' + name)
                    finally:
                        header.write_bytes(original)

            copied_input = next((output / 'inputs').iterdir())
            original = copied_input.read_bytes()
            copied_input.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            try:
                assert_configure_rejected('tampered-input', 'Corrupt or mixed build-kit input:')
            finally:
                copied_input.write_bytes(original)

    def test_relocation_excludes_probe_and_preserves_system_libraries(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / 'original-build'
            build.mkdir()
            self.write_compiler_metadata(build)
            for name in ['host.obj', 'registry_probe.c.obj', 'core.lib']:
                (build / name).write_bytes(name.encode())
            manifest = ''
            for mode in ['strict', 'hybrid']:
                manifest += ('build bin/suyu-export-host-' + mode + '.exe: CXX_EXECUTABLE_LINKER__Release host.obj registry_probe.c.obj | core.lib\n'
                             '  LINK_LIBRARIES = core.lib kernel32.lib\n  LINK_FLAGS = /machine:x64 /ENTRY:mainCRTStartup\n')
            (build / 'build.ninja').write_text(manifest)
            output = root / 'relocated-kit'
            kit.package(build, output, kit.REVISION)
            frozen = (output / 'strict.cmake').read_text()
            self.assertNotIn(str(build), frozen)
            self.assertNotIn('registry_probe', frozen)
            self.assertIn('kernel32.lib', frozen)
            self.assertIn('${CMAKE_CURRENT_LIST_DIR}/inputs/', frozen)
            self.assertEqual(len(list((output / 'inputs').iterdir())), 2)

    def test_missing_non_system_input_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.write_compiler_metadata(root)
            (root / 'build.ninja').write_text('build bin/suyu-export-host-strict.exe: CXX_EXECUTABLE_LINKER__Release missing.obj registry_probe.c.obj\n')
            with self.assertRaisesRegex(ValueError, 'Missing link input'):
                kit.package(root, root / 'kit', kit.REVISION)

    def test_revision_mismatch_fails_closed(self):
        with self.assertRaisesRegex(ValueError, 'revision'):
            kit.package('.', '.', 'old')

    def test_missing_compiler_metadata_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, 'producer C\\+\\+ compiler version'):
                kit.package(temp, Path(temp) / 'kit', kit.REVISION)

    def test_ninja_escaped_windows_paths(self):
        self.assertEqual(kit.ninja_words('G$:/some$ folder/host.obj other.obj'), ['G:/some folder/host.obj', 'other.obj'])


if __name__ == '__main__':
    unittest.main()
