#!/usr/bin/env python3
"""Package-policy validation of public release archives (stdlib only).

Scans the members of an archive without extracting anything to disk and reports
which policy rules (tools/package_policy/policy.json) they violate. Findings
carry a stable rule id, the member path and a short reason; file contents and
matched key text are never printed or stored.

This is a distribution control for the project's own release assets. Passing it
is not a legal clearance, and the hashes checked for the export build kit
identify and protect packaged inputs; they do not grant permission.

Exit status: 0 clean, 1 findings, 2 usage error.
"""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
import stat
import sys
import tarfile
import tempfile
import zipfile

POLICY_PATH = Path(__file__).with_name('policy.json')
KINDS = ('windows', 'linux', 'macos', 'android-apk', 'libretro-linux', 'libretro-windows',
         'libretro-macos', 'libretro-android', 'source')
HEAD_BYTES = 16384 + 8
WHOLE_FILE_LIMIT = 1 << 20
NESTED_MAX_BYTES = 256 << 20
NESTED_MAX_DEPTH = 2
MANIFEST_MAX_BYTES = 8 << 20
SPOOL_BYTES = 32 << 20
ZIP_EXTENSIONS = ('.zip', '.apk', '.jar')
TAR_EXTENSIONS = ('.tar.gz', '.tgz', '.tar')
OPAQUE_EXTENSIONS = ('.7z', '.rar', '.xz', '.zst', '.cab', '.msi', '.lz4', '.bz2', '.gz', '.iso')
KIT_PREFIX = 'export-build-kit/'
SHA256_RE = re.compile(r'^[0-9a-f]{64}$')


def load_policy(path=POLICY_PATH):
    return json.loads(Path(path).read_text(encoding='utf-8'))


class Rules:
    def __init__(self, policy):
        self.policy = policy
        self.version = policy['policy_version']
        self.key_names = [re.compile(p, re.I) for p in policy['key_file_name_patterns']]
        self.key_text = [re.compile(p) for p in policy['key_text_patterns']]
        self.firmware_names = [re.compile(p, re.I) for p in policy['firmware_nand_name_patterns']]
        self.firmware_paths = [re.compile(p, re.I) for p in policy['firmware_nand_path_patterns']]
        self.containers = tuple(e.lower() for e in policy['game_container_extensions'])
        self.markers = {n.lower() for n in policy['game_export_marker_names']}
        self.user_data = [re.compile(p) for p in policy['user_data_path_patterns']]
        self.signatures = [(s['name'], s['offset'], bytes.fromhex(s['hex'])) for s in policy['content_signatures']]
        self.text_limit = int(policy['key_text_scan_limit_bytes'])
        self.layouts = {k: [re.compile(p) for p in v] for k, v in policy.get('release_layouts', {}).items()}
        self.exceptions = [
            {'kinds': set(e.get('kinds', [])), 'path': re.compile(e['path_regex']),
             'rules': set(e.get('rules', []))}
            for e in policy.get('approved_exceptions', [])]

    def excused(self, kind, rule, member):
        return any(kind in e['kinds'] and rule in e['rules'] and e['path'].fullmatch(member)
                   for e in self.exceptions)


def clean_label(text):
    return ''.join(c if c.isprintable() else '?' for c in text)


def is_plain_text(prefix):
    """True when bytes are only printable ASCII and whitespace.

    A container magic that sits at a fixed offset follows binary header data (keys,
    hashes, code), so ordinary text that merely has the same four letters at that
    offset is not a container.
    """
    return all(b in (9, 10, 13) or 32 <= b < 127 for b in prefix)


def base_name(path):
    return posixpath.basename(path).rstrip('. ').lower()


def normalize(raw, strip_dot_slash):
    """Return (normalized path, is_dir, list of path-unsafe reasons)."""
    problems = []
    name = raw
    if '\x00' in name:
        problems.append('NUL in name')
        name = name.replace('\x00', '')
    if '\\' in name:
        problems.append('backslash in name')
        name = name.replace('\\', '/')
    if strip_dot_slash:
        while name.startswith('./'):
            name = name[2:]
    is_dir = name.endswith('/')
    stripped = name.rstrip('/')
    if not raw:
        problems.append('empty name')
    if stripped.startswith('/'):
        problems.append('absolute path')
    elif re.match(r'^[A-Za-z]:', stripped):
        problems.append('drive letter')
    parts = stripped.split('/')
    if '..' in parts:
        problems.append('parent-directory component')
    if stripped and '' in parts[1 if stripped.startswith('/') else 0:]:
        problems.append('empty path component')
    kept = [p for p in parts if p not in ('', '.', '..')]
    return '/'.join(kept), is_dir, problems


class Entry:
    __slots__ = ('raw', 'is_dir', 'link', 'link_target', 'size', 'opener')

    def __init__(self, raw, is_dir, link, link_target, size, opener):
        self.raw, self.is_dir, self.link, self.link_target = raw, is_dir, link, link_target
        self.size, self.opener = size, opener


def zip_entries(archive):
    for info in archive.infolist():
        is_dir = info.filename.endswith('/')
        link = target = None
        if not is_dir and stat.S_ISLNK(info.external_attr >> 16):
            link = 'symlink'
            if info.file_size <= 4096:
                try:
                    target = archive.read(info).decode('utf-8', 'replace')
                except Exception:
                    target = None
        yield Entry(info.filename, is_dir, link, target, info.file_size,
                    lambda info=info: archive.open(info))


def tar_entries(archive):
    for member in archive:
        link = target = None
        if member.issym():
            link, target = 'symlink', member.linkname
        elif member.islnk():
            link, target = 'hardlink', member.linkname
        elif not (member.isfile() or member.isdir()):
            link = 'special file'
        yield Entry(member.name, member.isdir(), link, target, member.size,
                    lambda member=member: archive.extractfile(member))


def archive_format(name):
    lower = name.lower()
    if lower.endswith(ZIP_EXTENSIONS):
        return 'zip'
    if lower.endswith(TAR_EXTENSIONS):
        return 'tar'
    return None


class Scanner:
    def __init__(self, rules, kind):
        self.rules, self.kind = rules, kind
        self.findings = []
        self.members = 0
        self.kit = {}
        self.kit_manifest = None
        self.source_top = None

    def add(self, rule, member, reason):
        self.findings.append({'rule': rule, 'member': clean_label(member), 'reason': reason})

    # ---- archive level -------------------------------------------------
    def scan(self, fileobj, fmt, prefix, depth):
        try:
            if fmt == 'zip':
                with zipfile.ZipFile(fileobj) as archive:
                    self.scan_entries(zip_entries(archive), False, prefix, depth)
            else:
                with tarfile.open(fileobj=fileobj, mode='r:*') as archive:
                    self.scan_entries(tar_entries(archive), True, prefix, depth)
        except (zipfile.BadZipFile, tarfile.TarError, EOFError, OSError, RuntimeError, ValueError) as error:
            self.add('unreadable', prefix.rstrip('!') or '(archive)', 'archive could not be read: ' + type(error).__name__)

    def scan_entries(self, entries, is_tar, prefix, depth):
        top = depth == 0
        seen = {}
        names, dirs, links = set(), set(), []
        for entry in entries:
            path, is_dir, problems = normalize(entry.raw, is_tar)
            if is_tar and not path and not problems and entry.is_dir:
                continue  # "./" root entry of "tar -C dir ."
            is_dir = is_dir or entry.is_dir
            self.members += 1
            label = prefix + (path or entry.raw)
            for problem in problems:
                self.add('path-unsafe', prefix + entry.raw, problem)
            if path:
                key = path.lower()
                if key in seen and not (is_dir and seen[key]):
                    self.add('path-unsafe', label, 'duplicate member name')
                seen[key] = is_dir and seen.get(key, True)
            if not path:
                continue
            for i in range(1, len(path.split('/'))):
                dirs.add('/'.join(path.split('/')[:i]))
            if is_dir or entry.link:
                if is_dir:
                    dirs.add(path)
            else:
                names.add(path)
            if entry.link:
                links.append((path, label, entry))
                continue
            if is_dir:
                continue
            if top:
                self.check_layout(path, label)
            self.scan_file(path, label, entry, prefix, depth)
        self.check_links(links, names, dirs, top)

    # ---- links -----------------------------------------------------------
    def check_links(self, links, names, dirs, top):
        link_map = {path: entry.link_target for path, _, entry in links if entry.link == 'symlink'}
        for path, label, entry in links:
            reason = self.link_problem(path, entry, link_map, names, dirs, top)
            if reason:
                self.add('link', label, reason)

    def link_problem(self, path, entry, link_map, names, dirs, top):
        if not (top and self.kind == 'macos'):
            return entry.link + ' entries are not allowed'
        if entry.link != 'symlink':
            return entry.link + ' entries are not allowed'
        match = re.match(r'^(suyu\.app/.*?[^/]+\.framework)/', path + '/')
        if not match:
            return 'symlink outside a suyu.app framework directory'
        framework = match[1]
        if path == framework:
            return 'symlink replaces a framework directory'
        target = entry.link_target
        if not target or '\x00' in target or '\\' in target or target.startswith('/') or re.match(r'^[A-Za-z]:', target):
            return 'symlink target is empty or not a relative path'
        resolved = self.resolve(posixpath.dirname(path), target, link_map)
        if resolved is None:
            return 'symlink target cannot be resolved inside the archive'
        if not resolved.startswith(framework + '/') or resolved == framework:
            return 'symlink target leaves its framework directory'
        if resolved not in names and resolved not in dirs:
            return 'symlink target is not a member of the archive'
        return None

    @staticmethod
    def resolve(base, target, link_map):
        pending = [p for p in (base + '/' + target).split('/') if p not in ('', '.')]
        current, hops = [], 0
        while pending:
            part = pending.pop(0)
            if part == '..':
                if not current:
                    return None
                current.pop()
                continue
            current.append(part)
            joined = '/'.join(current)
            if joined in link_map:
                hops += 1
                if hops > 40 or not link_map[joined]:
                    return None
                current.pop()
                pending = [p for p in link_map[joined].split('/') if p not in ('', '.')] + pending
        return '/'.join(current)

    # ---- layout ----------------------------------------------------------
    def check_layout(self, path, label):
        if self.kind == 'source':
            top_dir = path.split('/')[0]
            if self.source_top is None:
                self.source_top = top_dir
            elif top_dir != self.source_top:
                self.add('unexpected', label, 'more than one top-level directory')
                return
        if not any(p.fullmatch(path) for p in self.rules.layouts.get(self.kind, [])):
            self.add('unexpected', label, 'not part of the expected ' + self.kind + ' release layout')

    # ---- file content ----------------------------------------------------
    def scan_name(self, path, label):
        rules = self.rules
        base = base_name(path)
        if any(p.search(base) for p in rules.key_names):
            self.add('key-name', label, 'key file name')
        if any(p.search(base) for p in rules.firmware_names):
            self.add('firmware', label, 'firmware/NAND file name')
        lower = path.lower()
        if any(p.search(lower) for p in rules.firmware_paths):
            self.add('firmware', label, 'firmware/NAND path')
        if base.endswith(rules.containers):
            self.add('game-container', label, 'game container file extension')
        if base in rules.markers:
            self.add('export-marker', label, 'game export marker file name')
        if any(p.search(lower) for p in rules.user_data):
            self.add('user-data', label, 'user data path')
        if base.endswith(OPAQUE_EXTENSIONS) and not archive_format(base):
            self.add('nested-archive', label, 'opaque archive format cannot be inspected')

    def scan_file(self, path, label, entry, prefix, depth):
        rules = self.rules
        self.scan_name(path, label)
        fmt = archive_format(path)
        kit_rel = None
        if depth == 0 and self.kind == 'windows' and path.startswith(KIT_PREFIX):
            kit_rel = path[len(KIT_PREFIX):]
        size = entry.size
        text_limit = size if size <= WHOLE_FILE_LIMIT else rules.text_limit
        first = max(text_limit, HEAD_BYTES)
        nested_ok = fmt and depth < NESTED_MAX_DEPTH and size <= NESTED_MAX_BYTES
        if fmt and not nested_ok:
            self.add('nested-archive', label, 'nested archive too deep or too large to inspect')
        try:
            handle = entry.opener()
            if handle is None:
                raise OSError('no data')
            with handle:
                data = handle.read(first)
                for name, offset, magic in rules.signatures:
                    if data[offset:offset + len(magic)] == magic and not (offset and is_plain_text(data[:offset])):
                        self.add('signature', label, 'content signature: ' + name)
                text = data[:text_limit].decode('latin-1')
                if any(p.search(text) for p in rules.key_text):
                    self.add('key-text', label, 'content matches a key text pattern')
                digest = hashlib.sha256(data) if kit_rel is not None else None
                spool = None
                if nested_ok:
                    spool = tempfile.SpooledTemporaryFile(max_size=SPOOL_BYTES)
                    spool.write(data)
                manifest = bytearray(data) if kit_rel == 'manifest.json' else None
                total = len(data)
                if digest is not None or spool is not None:
                    while True:
                        chunk = handle.read(1 << 20)
                        if not chunk:
                            break
                        total += len(chunk)
                        if digest is not None:
                            digest.update(chunk)
                        if manifest is not None and len(manifest) <= MANIFEST_MAX_BYTES:
                            manifest.extend(chunk)
                        if spool is not None:
                            if total > NESTED_MAX_BYTES:
                                spool.close()
                                spool = None
                                self.add('nested-archive', label, 'nested archive too large to inspect')
                                if digest is None:
                                    break
                            else:
                                spool.write(chunk)
        except Exception as error:
            self.add('unreadable', label, 'member could not be read: ' + type(error).__name__)
            return
        if digest is not None:
            self.kit[kit_rel] = digest.hexdigest()
            if manifest is not None:
                self.kit_manifest = bytes(manifest)
        if spool is not None:
            try:
                spool.seek(0)
                self.scan(spool, fmt, label + '!', depth + 1)
            finally:
                spool.close()

    # ---- export build kit ------------------------------------------------
    def check_kit(self):
        member = KIT_PREFIX + 'manifest.json'
        if self.kit_manifest is None:
            self.add('kit', member, 'export build kit manifest.json is missing')
            return
        try:
            manifest = json.loads(self.kit_manifest.decode('utf-8'))
            if not isinstance(manifest, dict):
                raise ValueError
        except ValueError:
            self.add('kit', member, 'manifest.json is not a valid JSON object')
            return
        if manifest.get('policy_version') != self.rules.version:
            self.add('kit', member, 'manifest policy_version does not match ' + self.rules.version)
        revision = manifest.get('producer_source_revision')
        if not isinstance(revision, str) or not revision.strip():
            self.add('kit', member, 'manifest producer_source_revision is missing or empty')
        files = manifest.get('files')
        if not isinstance(files, dict) or not files:
            self.add('kit', member, 'manifest files table is missing or empty')
            return
        for rel, digest in sorted(self.kit.items()):
            if rel == 'manifest.json':
                continue
            listed = files.get(rel)
            if listed is None:
                self.add('kit', KIT_PREFIX + rel, 'file is not listed in the kit manifest')
            elif not isinstance(listed, str) or not SHA256_RE.match(listed.lower()) or listed.lower() != digest:
                self.add('kit', KIT_PREFIX + rel, 'sha256 does not match the kit manifest')
        for rel in sorted(files):
            if rel != 'manifest.json' and rel not in self.kit:
                self.add('kit', KIT_PREFIX + rel, 'file listed in the kit manifest is missing')


def scan_archive(path, kind, rules):
    scanner = Scanner(rules, kind)
    path = Path(path)
    fmt = archive_format(path.name)
    if fmt is None:
        scanner.add('unreadable', path.name, 'unsupported archive type')
    else:
        with open(path, 'rb') as handle:
            scanner.scan(handle, fmt, '', 0)
        if kind == 'windows':
            scanner.check_kit()
    findings = [f for f in scanner.findings if not rules.excused(kind, f['rule'], f['member'])]
    findings.sort(key=lambda f: (f['member'], f['rule'], f['reason']))
    return {'path': path.name, 'kind': kind, 'members_scanned': scanner.members, 'findings': findings}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--kind', required=True, choices=KINDS)
    parser.add_argument('--report', help='write a JSON report (archive base names only)')
    parser.add_argument('--policy', default=str(POLICY_PATH), help=argparse.SUPPRESS)
    parser.add_argument('archives', nargs='+', metavar='ARCHIVE')
    args = parser.parse_args(argv)
    sys.stdout.reconfigure(errors='backslashreplace')
    for archive in args.archives:
        if not Path(archive).is_file():
            parser.error('not a file: ' + archive)
    rules = Rules(load_policy(args.policy))
    results = [scan_archive(a, args.kind, rules) for a in args.archives]
    if args.report:
        Path(args.report).write_text(json.dumps(
            {'policy_version': rules.version, 'archives': results}, indent=2) + '\n', encoding='utf-8')
    failed = False
    for result in results:
        count = len(result['findings'])
        print(f"{result['path']} [{result['kind']}]: {result['members_scanned']} members, "
              f"{count} finding(s)")
        for finding in result['findings']:
            print(f"  {finding['rule']}: {finding['member']}: {finding['reason']}")
        failed = failed or bool(count)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
