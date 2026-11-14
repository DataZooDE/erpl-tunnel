#!/usr/bin/env python3
"""scripts/check_skill_docs.py -- keep the Claude Code plugin honest (offline, no network).

Checks plugins/erpl-tunnel/ and README.md against a built extension:

1. Every `tunnel_*` name used in a backtick span, in call syntax `name(`, after PRAGMA, or in a SKILL.md
   frontmatter is a registered function or pragma (`duckdb_functions()`), or listed with a reason in
   scripts/doc_snippets/allowed_names.txt.
2. Every backticked `erpl-tunnel-*` skill the plugin or README names exists under plugins/erpl-tunnel/skills.
3. Every CREATE SECRET of TYPE tunnel / ssh_tunnel in a ```sql fence is accepted (created in :memory:; nothing
   connects), and every fence whose first line is `-- check:offline` runs.

Usage: python3 scripts/check_skill_docs.py [--duckdb CMD] [--preamble SQL]
Default: build/release/duckdb with `LOAD '<build>/erpl_tunnel.duckdb_extension'`. Exit 0 clean, 1 on FAIL or
STALE, 2 when DuckDB cannot run.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGIN = "plugins/erpl-tunnel"
ALLOWED = os.path.join(REPO, "scripts", "doc_snippets", "allowed_names.txt")
EXT = os.path.join(REPO, "build", "release", "extension", "erpl_tunnel", "erpl_tunnel.duckdb_extension")

FENCE_RE = re.compile(r"^```[ \t]*([A-Za-z0-9_+-]*)[ \t]*$")
NAME = r"tunnel_[a-z0-9_]*[a-z0-9]"
SPAN_RE = re.compile(r"`([^`\n]*)`")
NAME_RE = re.compile(r"\b(" + NAME + r")\b", re.IGNORECASE)
CALL_RE = re.compile(r"(?:\b(AS|SECRET|TABLE|VIEW)\s+)?\b(" + NAME + r")\s*\(", re.IGNORECASE)
PRAGMA_RE = re.compile(r"\bPRAGMA\s+(" + NAME + r")\b", re.IGNORECASE)
SKILL_RE = re.compile(r"^erpl-tunnel-[a-z0-9-]+$")
SECRET_RE = re.compile(r"CREATE\s+(?:OR\s+REPLACE\s+)?(?:PERSISTENT\s+|TEMPORARY\s+)?SECRET\b.*?\);",
                       re.IGNORECASE | re.DOTALL)
# Shaped like a tunnel_* name but not part of the SQL surface.
NOT_SURFACE = {"tunnel_id"}


def files():
    out = ["README.md"]
    for base, dirs, names in os.walk(os.path.join(REPO, PLUGIN)):
        dirs.sort()
        out += [os.path.relpath(os.path.join(base, n), REPO).replace(os.sep, "/") for n in sorted(names)
                if n.endswith(".md")]
    return out


def allowed():
    names = set()
    if os.path.exists(ALLOWED):
        with open(ALLOWED, encoding="utf-8") as f:
            names = {l.split("\t", 1)[0].strip().lower() for l in f if l.strip() and not l.startswith("#")}
    return names


def run(cmd, preamble, sql):
    tmp = tempfile.mkdtemp(prefix="skill_docs_")
    try:
        full = f"SET secret_directory = '{tmp}';\n{preamble}\n{sql}"
        env = dict(os.environ, DATAZOO_DISABLE_TELEMETRY="1")
        try:
            r = subprocess.run(cmd + ["-unsigned", "-bail", "-csv", ":memory:"], input=full, cwd=tmp,
                               capture_output=True, text=True, timeout=120, env=env)
            return r.returncode, r.stdout, r.stderr
        except OSError as e:
            print(f"ERROR: cannot run {' '.join(cmd)}: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def scan(text):
    lines = text.splitlines()
    fm_end = next((i for i in range(1, len(lines)) if lines[0].strip() == "---" and lines[i].strip() == "---"), 0)
    names, skills, fences = [], [], []
    in_fence, lang, body, start = False, "", [], 0
    for i, line in enumerate(lines):
        m = FENCE_RE.match(line.strip())
        if m and not in_fence:
            in_fence, lang, body, start = True, m.group(1).lower(), [], i + 1
            continue
        if in_fence and line.strip() == "```":
            in_fence = False
            fences.append((lang, "\n".join(body), start))
            continue
        if in_fence:
            body.append(line)
            if lang == "sql":
                code = line.split("--", 1)[0]
                names += [(n, i + 1) for d, n in CALL_RE.findall(code) if not d]
                names += [(n, i + 1) for n in PRAGMA_RE.findall(code)]
            continue
        if 0 < i < fm_end:
            names += [(n, i + 1) for n in NAME_RE.findall(line)]
            continue
        for span in SPAN_RE.findall(line):
            names += [(n, i + 1) for n in NAME_RE.findall(span)]
            if SKILL_RE.match(span.strip()):
                skills.append((span.strip(), i + 1))
        names += [(n, i + 1) for d, n in CALL_RE.findall(line) if not d]
    return [(n.lower(), l) for n, l in names if n.lower() not in NOT_SURFACE], skills, fences


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duckdb", default=os.path.join(REPO, "build", "release", "duckdb"))
    ap.add_argument("--preamble", default=f"LOAD '{EXT}';")
    args = ap.parse_args()
    cmd = shlex.split(args.duckdb)
    rc, out, err = run(cmd, args.preamble, "SELECT DISTINCT function_name FROM duckdb_functions();")
    if rc != 0:
        print(f"ERROR: cannot query duckdb_functions(): {(err or out).strip()[:400]}", file=sys.stderr)
        return 2
    surface = {l.strip().strip('"').lower() for l in out.splitlines()[1:] if l.strip()}
    skills = {d for d in os.listdir(os.path.join(REPO, PLUGIN, "skills"))}
    allow = allowed()
    fails, passes, mentioned = [], 0, set()
    for path in files():
        with open(os.path.join(REPO, path), encoding="utf-8") as f:
            names, refs, fences = scan(f.read())
        for name, line in names:
            mentioned.add(name)
            if name not in surface and name not in allow:
                fails.append(f"FAIL\t{path}:{line}\tunknown name {name}")
        for skill, line in refs:
            if skill not in skills:
                fails.append(f"FAIL\t{path}:{line}\tskill {skill} does not exist")
        for lang, body, line in fences:
            if lang != "sql":
                continue
            stmts = [s for s in SECRET_RE.findall(body) if re.search(r"\bTYPE\s+(ssh_)?tunnel\b", s, re.I)]
            if body.lstrip().startswith("-- check:offline"):
                stmts.append(body)
            for stmt in stmts:
                rc, out, err = run(cmd, args.preamble, stmt)
                if rc == 0:
                    passes += 1
                else:
                    fails.append(f"FAIL\t{path}:{line}\t{(err or out).strip().splitlines()[0][:200]}")
    for name in sorted(allow - mentioned):
        fails.append(f"STALE\tscripts/doc_snippets/allowed_names.txt\t{name} is no longer mentioned")
    print("\n".join(fails))
    print(f"check_skill_docs: {len(fails)} problems, {passes} statements ran")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
