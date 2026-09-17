#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_YML = REPO_ROOT / ".github" / "workflows" / "release.yml"
PR_CI_YML = REPO_ROOT / ".github" / "workflows" / "exporter-smoke.yml"
PUBLISH_JOB_ID = "release"
ROLLING_TAG = "v0.04-latest"
STATUS_FUNCS = ("always", "success", "failure", "cancelled")


@dataclass
class JobSpec:
    job_id: str
    name: str
    needs: list[str]
    raw_if: str | None
    continue_on_error: bool
    steps: list[dict[str, Any]]
    uses: str | None


@dataclass
class GhEvent:
    kind: str
    args: list[str]


@dataclass
class PublishTrace:
    job_ran: bool
    events: list[GhEvent] = field(default_factory=list)
    stdout: str = ""
    stderr: str = ""
    exit_code: int | None = None
    created_tag: str | None = None
    notes: str = ""
    uploaded: list[str] = field(default_factory=list)
    checksum_text: str = ""


def load_workflow(path: Path) -> dict[str, Any]:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def parse_jobs(doc: dict[str, Any]) -> dict[str, JobSpec]:
    jobs: dict[str, JobSpec] = {}
    for job_id, raw in (doc.get("jobs") or {}).items():
        needs = raw.get("needs") or []
        if isinstance(needs, str):
            needs = [needs]
        jobs[job_id] = JobSpec(
            job_id=job_id,
            name=str(raw.get("name") or job_id),
            needs=list(needs),
            raw_if=raw.get("if"),
            continue_on_error=bool(raw.get("continue-on-error", False)),
            steps=list(raw.get("steps") or []),
            uses=raw.get("uses"),
        )
    return jobs


def unwrap_expression(expr: str) -> str:
    expr = expr.strip()
    if expr.startswith("${{") and expr.endswith("}}"):
        return expr[3:-2].strip()
    return expr


def uses_status_check(expr: str) -> bool:
    return any(re.search(rf"\b{name}\s*\(", expr) for name in STATUS_FUNCS)


def apply_github_default_success(expr: str) -> str:
    if uses_status_check(expr):
        return expr
    return f"success() && ({expr})"


def tokenize(expr: str) -> list[str]:
    spec = r"""
        \s+
        | '([^']*)'
        | "([^"]*)"
        | [A-Za-z_][A-Za-z0-9_-]*
        | && | \|\|
        | == | !=
        | [()!.]
    """
    tokens: list[str] = []
    for match in re.finditer(spec, expr, re.VERBOSE):
        tok = match.group(0)
        if tok.isspace():
            continue
        tokens.append(tok)
    return tokens


class ExprEval:
    def __init__(self, tokens: list[str], ctx: dict[str, Any]):
        self.tokens = tokens
        self.i = 0
        self.ctx = ctx

    def peek(self) -> str | None:
        return self.tokens[self.i] if self.i < len(self.tokens) else None

    def eat(self, expected: str | None = None) -> str:
        if self.i >= len(self.tokens):
            raise ValueError("unexpected end of expression")
        tok = self.tokens[self.i]
        if expected is not None and tok != expected:
            raise ValueError(f"expected {expected!r}, got {tok!r}")
        self.i += 1
        return tok

    def parse(self) -> Any:
        value = self.parse_or()
        if self.peek() is not None:
            raise ValueError(f"trailing token {self.peek()!r}")
        return value

    def parse_or(self) -> bool:
        left = self.parse_and()
        while self.peek() == "||":
            self.eat("||")
            right = self.parse_and()
            left = bool(left) or bool(right)
        return bool(left)

    def parse_and(self) -> bool:
        left = self.parse_eq()
        while self.peek() == "&&":
            self.eat("&&")
            right = self.parse_eq()
            left = bool(left) and bool(right)
        return bool(left)

    def parse_eq(self) -> Any:
        left = self.parse_unary()
        while self.peek() in ("==", "!="):
            op = self.eat()
            right = self.parse_unary()
            if op == "==":
                left = _gha_equal(left, right)
            else:
                left = not _gha_equal(left, right)
        return left

    def parse_unary(self) -> Any:
        if self.peek() == "!":
            self.eat("!")
            return not bool(self.parse_unary())
        return self.parse_primary()

    def parse_primary(self) -> Any:
        tok = self.peek()
        if tok is None:
            raise ValueError("expected value")
        if tok == "(":
            self.eat("(")
            value = self.parse_or()
            self.eat(")")
            return value
        if tok.startswith("'") or tok.startswith('"'):
            self.eat()
            return tok[1:-1]
        if tok in STATUS_FUNCS:
            name = self.eat()
            self.eat("(")
            self.eat(")")
            return self.ctx["status"][name]()
        if re.match(r"^[A-Za-z_]", tok):
            name = self.eat()
            if name == "needs":
                while self.peek() == ".":
                    self.eat(".")
                    name += "." + self.eat()
                return self.lookup_needs(name)
            return name
        raise ValueError(f"bad token {tok!r}")

    def lookup_needs(self, path: str) -> str:
        parts = path.split(".")
        if len(parts) != 3 or parts[0] != "needs" or parts[2] != "result":
            raise ValueError(f"unsupported needs path {path}")
        job_id = parts[1]
        results = self.ctx["needs_results"]
        if job_id not in results:
            raise ValueError(f"unknown needed job {job_id}")
        return results[job_id]


def _gha_equal(left: Any, right: Any) -> bool:
    if isinstance(left, str) and isinstance(right, str):
        return left.lower() == right.lower()
    return left == right


def evaluate_if(
    raw_if: str | None,
    needs: list[str],
    needs_results: dict[str, str],
    cancelled: bool = False,
) -> bool:
    needed = {job: needs_results[job] for job in needs}

    def success() -> bool:
        return all(result == "success" for result in needed.values()) and not cancelled

    def failure() -> bool:
        return any(result == "failure" for result in needed.values())

    ctx = {
        "needs_results": needs_results,
        "status": {
            "always": lambda: True,
            "success": success,
            "failure": failure,
            "cancelled": lambda: cancelled,
        },
    }
    if raw_if is None:
        return success()
    expr = apply_github_default_success(unwrap_expression(str(raw_if)))
    value = ExprEval(tokenize(expr), ctx).parse()
    return bool(value)


def required_job_ids(jobs: dict[str, JobSpec]) -> list[str]:
    return [
        job_id
        for job_id, spec in jobs.items()
        if job_id != PUBLISH_JOB_ID and not spec.continue_on_error
    ]


def optional_job_ids(jobs: dict[str, JobSpec]) -> list[str]:
    return [
        job_id
        for job_id, spec in jobs.items()
        if job_id != PUBLISH_JOB_ID and spec.continue_on_error
    ]


def step_run_script(step: dict[str, Any]) -> str | None:
    run = step.get("run")
    return run if isinstance(run, str) else None


def publish_scripts(spec: JobSpec) -> list[tuple[str, str]]:
    scripts: list[tuple[str, str]] = []
    for step in spec.steps:
        run = step_run_script(step)
        if run is None:
            continue
        scripts.append((str(step.get("name") or "unnamed"), run))
    return scripts


def write_executable(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def run_publish_shell(
    spec: JobSpec,
    artifacts: dict[str, bytes],
    repo: str = "SourCreamCulture/suyu-v0.0.4",
    commit: str = "deadbeefcafebabe0123456789abcdef01234567",
) -> PublishTrace:
    scripts = publish_scripts(spec)
    with tempfile.TemporaryDirectory(prefix="suyu-release-gate-") as tmp:
        tmp_path = Path(tmp)
        artifacts_dir = tmp_path / "artifacts"
        bin_dir = tmp_path / "bin"
        log_path = tmp_path / "stub.log"
        notes_path = tmp_path / "created-notes.md"
        checksum_path = tmp_path / "created-checksums"
        uploaded_dir = tmp_path / "uploaded"
        artifacts_dir.mkdir()
        bin_dir.mkdir()
        uploaded_dir.mkdir()
        log_path.write_text("", encoding="utf-8")
        scripts_src = REPO_ROOT / ".github" / "scripts"
        if scripts_src.is_dir():
            shutil.copytree(scripts_src, tmp_path / ".github" / "scripts")
        for relative, data in artifacts.items():
            dest = artifacts_dir / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)

        gh_stub = """#!/bin/bash
set -euo pipefail
LOG=__LOG__
echo "GH $*" >> "$LOG"
if [ "${1:-}" = "release" ] && [ "${2:-}" = "delete" ]; then
  echo "DELETED ${3:-}" >> "$LOG"
  exit 0
fi
if [ "${1:-}" = "release" ] && [ "${2:-}" = "create" ]; then
  tag="$3"
  notes=""
  files=()
  prerelease=0
  latest=0
  shift 3
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --repo|--title|--notes|--notes-file)
        key="$1"
        val="$2"
        shift 2
        if [ "$key" = "--notes" ]; then notes="$val"; fi
        if [ "$key" = "--notes-file" ]; then notes="$(cat "$val")"; fi
        ;;
      --repo=*|--title=*) ;;
      --notes=*) notes="${1#--notes=}" ;;
      --notes-file=*) notes="$(cat "${1#--notes-file=}")" ;;
      --prerelease|--prerelease=true) prerelease=1; shift ;;
      --prerelease=false) prerelease=0; shift ;;
      --latest|--latest=true) latest=1; shift ;;
      --latest=false) latest=0; shift ;;
      --draft) shift ;;
      --*) shift ;;
      *)
        files+=("$1")
        shift
        ;;
    esac
  done
  if [ "$prerelease" -eq 1 ] && [ "$latest" -eq 1 ]; then
    echo "HTTP 422: Validation Failed (https://api.github.com/repos/${GITHUB_REPOSITORY:-}/releases)" >&2
    echo "Latest release cannot be draft or prerelease." >&2
    echo "CLEANUP_DRAFT" >> "$LOG"
    exit 1
  fi
  echo "CREATED $tag" >> "$LOG"
  printf '%s\\n' "$notes" > __NOTES__
  : > __CHECKSUMS__
  for f in "${files[@]+"${files[@]}"}"; do
    echo "UPLOAD $f" >> "$LOG"
    cp -a "$f" __UPLOADED__/ 2>/dev/null || true
    base="$(basename "$f")"
    case "$base" in
      *SHA256*|*sha256*|*CHECKSUM*|*checksum*)
        cat "$f" >> __CHECKSUMS__ || true
        ;;
    esac
  done
  echo "$notes" >> __CHECKSUMS__
  exit 0
fi
exit 0
"""
        write_executable(
            bin_dir / "gh",
            gh_stub.replace("__LOG__", str(log_path))
            .replace("__NOTES__", str(notes_path))
            .replace("__CHECKSUMS__", str(checksum_path))
            .replace("__UPLOADED__", str(uploaded_dir)),
        )
        git_stub = """#!/bin/bash
LOG=__LOG__
if [ "${1:-}" = "push" ] && [ "${2:-}" = "--delete" ]; then
  echo "GIT_PUSH_DELETE $*" >> "$LOG"
  exit 0
fi
exec /usr/bin/git "$@"
"""
        write_executable(bin_dir / "git", git_stub.replace("__LOG__", str(log_path)))

        env = os.environ.copy()
        env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
        env["GH_TOKEN"] = "stub"
        env["GITHUB_REPOSITORY"] = repo
        env["GITHUB_SHA"] = commit
        env["GITHUB_RUN_ID"] = "1"
        env["GITHUB_SERVER_URL"] = "https://github.com"
        env["GITHUB_WORKSPACE"] = str(tmp_path)
        stdout_parts: list[str] = []
        stderr_parts: list[str] = []
        exit_code = 0
        for name, script in scripts:
            rendered = (
                script.replace("${{ github.repository }}", repo)
                .replace("${{ github.sha }}", commit)
                .replace("${{ github.run_id }}", "1")
                .replace("${{ github.server_url }}", "https://github.com")
            )
            proc = subprocess.run(
                ["bash", "-lc", rendered],
                cwd=tmp_path,
                env=env,
                capture_output=True,
                text=True,
            )
            stdout_parts.append(f"$ {name}\n{proc.stdout}")
            if proc.stderr:
                stderr_parts.append(f"$ {name}\n{proc.stderr}")
            if proc.returncode != 0:
                exit_code = proc.returncode
                break

        events: list[GhEvent] = []
        created_tag = None
        for line in log_path.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "DELETED":
                events.append(GhEvent("delete", parts[1:]))
            elif parts[0] == "CREATED":
                created_tag = parts[1]
                events.append(GhEvent("create", parts[1:]))
            elif parts[0] == "GIT_PUSH_DELETE":
                events.append(GhEvent("git-delete", parts[1:]))
            elif parts[0] == "UPLOAD":
                events.append(GhEvent("upload", parts[1:]))
            elif parts[0] == "CLEANUP_DRAFT":
                events.append(GhEvent("cleanup-draft", []))
            elif parts[0] == "GH":
                events.append(GhEvent("gh", parts[1:]))

        notes = notes_path.read_text(encoding="utf-8") if notes_path.exists() else ""
        checksum_text = (
            checksum_path.read_text(encoding="utf-8") if checksum_path.exists() else ""
        )
        uploaded = sorted(p.name for p in uploaded_dir.iterdir()) if uploaded_dir.exists() else []
        return PublishTrace(
            job_ran=True,
            events=events,
            stdout="\n".join(stdout_parts),
            stderr="\n".join(stderr_parts),
            exit_code=exit_code,
            created_tag=created_tag,
            notes=notes,
            uploaded=uploaded,
            checksum_text=checksum_text,
        )


def default_results(jobs: dict[str, JobSpec], **overrides: str) -> dict[str, str]:
    results = {job_id: "success" for job_id in jobs if job_id != PUBLISH_JOB_ID}
    results.update(overrides)
    return results


def would_publish(jobs: dict[str, JobSpec], results: dict[str, str], cancelled: bool = False) -> bool:
    spec = jobs[PUBLISH_JOB_ID]
    return evaluate_if(spec.raw_if, spec.needs, results, cancelled=cancelled)


def yaml_text() -> str:
    return RELEASE_YML.read_text(encoding="utf-8")


def pr_ci_text() -> str:
    return PR_CI_YML.read_text(encoding="utf-8") if PR_CI_YML.exists() else ""


def workflow_has_aot_job(doc: dict[str, Any], text: str) -> bool:
    if "src/tests/recompiler" in text:
        return True
    if "exporter-smoke.yml" in text:
        return True
    for spec in parse_jobs(doc).values():
        if spec.uses and "exporter-smoke.yml" in spec.uses:
            return True
    return False


def workflow_has_test_suite_job(doc: dict[str, Any], text: str) -> bool:
    if re.search(r"--target\s+tests\b", text):
        return True
    if "YUZU_TESTS=ON" in text or "-DYUZU_TESTS=ON" in text:
        return True
    for spec in parse_jobs(doc).values():
        if spec.uses and spec.uses.endswith("tests.yml"):
            return True
        if spec.job_id == "tests" and spec.job_id != "exporter-smoke":
            return True
    return False


def publish_deletes_a_release(spec: JobSpec) -> bool:
    for _, script in publish_scripts(spec):
        if "gh release delete" in script or "git push --delete" in script:
            return True
    return False


def immutable_tag(tag: str | None) -> bool:
    if not tag:
        return False
    if tag == ROLLING_TAG or tag.endswith("-latest"):
        return False
    return bool(re.search(r"[0-9a-f]{7,}", tag)) or bool(re.search(r"\d{8}", tag))


def has_commit_metadata(trace: PublishTrace, commit: str) -> bool:
    blob = trace.notes + "\n" + trace.checksum_text + "\n" + "\n".join(trace.uploaded)
    return commit in blob or commit[:12] in blob or commit[:7] in blob


def has_checksum_metadata(trace: PublishTrace) -> bool:
    blob = (trace.notes + "\n" + trace.checksum_text).lower()
    if "sha256" in blob or "checksum" in blob:
        return True
    return any("sha256" in name.lower() or "checksum" in name.lower() for name in trace.uploaded)


def has_toolchain_metadata(trace: PublishTrace) -> bool:
    blob = (trace.notes + "\n" + trace.checksum_text + "\n" + "\n".join(trace.uploaded)).lower()
    keys = ("cmake", "compiler", "toolchain", "clang", "gcc", "msvc", "appleclang")
    return any(key in blob for key in keys)


class Check:
    def __init__(self) -> None:
        self.fails = 0

    def expect(self, cond: bool, msg: str) -> None:
        if cond:
            print(f"PASS: {msg}")
        else:
            print(f"FAIL: {msg}")
            self.fails += 1


def sample_artifacts(missing: set[str] | None = None) -> dict[str, bytes]:
    files = {
        "windows/drippu-windows-x64.zip": b"win",
        "linux/drippu-linux-x64.tar.gz": b"lin",
        "macos/drippu-macos-arm64.tar.gz": b"mac",
        "android/suyu.apk": b"apk",
        "libretro/drippu-libretro-core-linux-x64.tar.gz": b"lr",
        "libretro-windows/drippu-libretro-core-windows-x64.zip": b"lrw",
        "freebsd/drippu-freebsd-x64.tar.gz": b"fbsd",
        "libretro-android/drippu-libretro-core-android-arm64.tar.gz": b"lra",
    }
    if missing:
        for key in list(files):
            job = key.split("/", 1)[0]
            if job in missing:
                del files[key]
    return files


def main() -> int:
    check = Check()
    if not RELEASE_YML.is_file():
        print(f"FAIL: missing {RELEASE_YML}")
        return 1
    doc = load_workflow(RELEASE_YML)
    jobs = parse_jobs(doc)
    text = yaml_text()
    check.expect(PUBLISH_JOB_ID in jobs, "release workflow defines a publish job")
    spec = jobs[PUBLISH_JOB_ID]
    required = required_job_ids(jobs)
    optional = optional_job_ids(jobs)

    print(f"publish if: {spec.raw_if!r}")
    print(f"publish needs: {spec.needs}")
    print(f"required jobs: {required}")
    print(f"optional jobs: {optional}")

    check.expect(
        PR_CI_YML.is_file() and "pull_request" in pr_ci_text(),
        "PR CI exists separately from publishing (exporter-smoke.yml on pull_request)",
    )

    linux_fail = default_results(jobs, **{"build-linux": "failure"})
    publishes_after_required_fail = would_publish(jobs, linux_fail)
    print(f"publish after required linux failure: {publishes_after_required_fail}")
    check.expect(
        not publishes_after_required_fail,
        "required platform failure does not run Publish Release",
    )

    if optional:
        opt = optional[0]
        opt_fail = default_results(jobs, **{opt: "failure"})
        publishes_after_optional_fail = would_publish(jobs, opt_fail)
        print(f"publish after optional {opt} failure: {publishes_after_optional_fail}")
        check.expect(
            publishes_after_optional_fail,
            f"optional {opt} failure still allows Publish Release",
        )

    all_fail = {job_id: "failure" for job_id in jobs if job_id != PUBLISH_JOB_ID}
    check.expect(
        not would_publish(jobs, all_fail),
        "all platform failures do not run Publish Release",
    )
    check.expect(
        not would_publish(jobs, default_results(jobs), cancelled=True),
        "cancelled workflow does not run Publish Release",
    )
    check.expect(
        would_publish(jobs, default_results(jobs)),
        "all required successes run Publish Release",
    )

    check.expect(
        not publish_deletes_a_release(spec),
        "publish job does not delete a release or tag",
    )
    check.expect(
        workflow_has_aot_job(doc, text),
        "release workflow generates and compiles an AOT project (exporter smoke)",
    )
    check.expect(
        workflow_has_test_suite_job(doc, text),
        "release workflow builds and runs the test suite",
    )

    commit = "cafebabedeadbeef0123456789abcdef01234567"
    print("--- drive: required linux missing, leftover windows zip ---")
    if publishes_after_required_fail:
        trace = run_publish_shell(spec, sample_artifacts(missing={"linux"}), commit=commit)
        kinds = [event.kind for event in trace.events]
        print(trace.stdout)
        print(f"events: {kinds}")
        print(f"created_tag: {trace.created_tag}")
        print(f"exit: {trace.exit_code}")
        check.expect(
            "delete" not in kinds and "git-delete" not in kinds,
            "required failure does not delete v0.04-latest",
        )
        check.expect(
            trace.created_tag is None,
            "required failure does not create a replacement release",
        )
    else:
        print("publish job skipped (gate held)")

    print("--- drive: empty artifacts, all jobs reported success ---")
    empty_trace = run_publish_shell(spec, {}, commit=commit)
    empty_kinds = [event.kind for event in empty_trace.events]
    print(empty_trace.stdout)
    print(f"events: {empty_kinds}")
    print(f"exit: {empty_trace.exit_code}")
    check.expect(
        "delete" not in empty_kinds and "git-delete" not in empty_kinds,
        "empty artifacts do not delete an existing release",
    )
    check.expect(empty_trace.created_tag is None, "empty artifacts do not create a release")
    check.expect(empty_trace.exit_code != 0, "empty artifacts fail the publish step")

    print("--- drive: linux artifact missing, jobs reported success ---")
    partial_trace = run_publish_shell(spec, sample_artifacts(missing={"linux"}), commit=commit)
    partial_kinds = [event.kind for event in partial_trace.events]
    print(partial_trace.stdout)
    print(f"events: {partial_kinds}")
    print(f"exit: {partial_trace.exit_code}")
    check.expect(
        "delete" not in partial_kinds and "create" not in partial_kinds,
        "missing required linux artifact does not delete or publish",
    )
    check.expect(partial_trace.exit_code != 0, "missing required linux artifact fails publish")

    print("--- drive: required artifacts present ---")
    full_trace = run_publish_shell(spec, sample_artifacts(), commit=commit)
    full_kinds = [event.kind for event in full_trace.events]
    print(full_trace.stdout)
    if full_trace.stderr:
        print(full_trace.stderr)
    print(f"events: {full_kinds}")
    print(f"created_tag: {full_trace.created_tag}")
    print(f"notes: {full_trace.notes!r}")
    print(f"exit: {full_trace.exit_code}")
    check.expect(full_trace.exit_code == 0, "successful artifact set publishes")
    check.expect(
        "delete" not in full_kinds and "git-delete" not in full_kinds,
        "successful publish does not delete a prior tag",
    )
    check.expect(
        immutable_tag(full_trace.created_tag),
        f"published tag is immutable (not {ROLLING_TAG}): {full_trace.created_tag}",
    )
    check.expect(
        has_commit_metadata(full_trace, commit),
        "release notes or files include the commit id",
    )
    check.expect(
        has_checksum_metadata(full_trace),
        "release includes checksum metadata",
    )
    check.expect(
        has_toolchain_metadata(full_trace),
        "release includes toolchain metadata",
    )

    print(f"check_release_gate: {check.fails} failure(s)")
    return 1 if check.fails else 0


if __name__ == "__main__":
    sys.exit(main())
