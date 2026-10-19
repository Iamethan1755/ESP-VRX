#!/usr/bin/env python3
"""Resolve the next immutable C5VRX-4 semantic alpha version from git tags."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PATTERN = re.compile(r'^c5vrx4-v(4)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-alpha\.([1-9][0-9]*)$')

def resolve_version(tags, same_commit_tags=()):
    def parsed(tag):
        match = PATTERN.fullmatch(tag)
        return tuple(map(int, match.groups())) if match else None
    existing = [(parsed(tag), tag) for tag in same_commit_tags if parsed(tag)]
    if existing:
        tag = max(existing)[1]
        return {'version': tag.removeprefix('c5vrx4-v'), 'tag': tag}
    versions = [parsed(tag) for tag in tags if parsed(tag)]
    major, minor, patch, alpha = max(versions, default=(4, 0, 0, 0))
    version = f'{major}.{minor}.{patch}-alpha.{alpha + 1}'
    return {'version': version, 'tag': f'c5vrx4-v{version}'}

def published_version(tags, same, build_sha, input_sha, repository):
    # Release assets survive a history rewrite; git tags alone are not an
    # authoritative record of the commit/input set that was actually published.
    published = subprocess.check_output(
        ['gh', 'api', '--paginate', f'repos/{repository}/releases',
         '--jq', '.[] | select(.draft == false) | .tag_name'], text=True
    ).splitlines()
    reusable = []
    for tag in same:
        if not PATTERN.fullmatch(tag):
            continue
        if tag not in published:
            reusable.append(tag)
            continue
        with tempfile.TemporaryDirectory() as previous:
            subprocess.run(
                ['gh', 'release', 'download', tag, '--repo', repository,
                 '--pattern', 'COMMIT', '--pattern', 'FIRMWARE_INPUT_SHA',
                 '--dir', previous], check=True
            )
            commit = (Path(previous) / 'COMMIT').read_text().strip()
            inputs = (Path(previous) / 'FIRMWARE_INPUT_SHA').read_text().strip()
            if commit == build_sha or inputs == input_sha:
                reusable.append(tag)
    return resolve_version(set(tags) | set(published), reusable)


def main():
    command = ['git', '-c', f'safe.directory={ROOT}', 'tag']
    tags = subprocess.check_output(command + ['--list'], cwd=ROOT, text=True).splitlines()
    same = subprocess.check_output(command + ['--points-at', os.environ['GITHUB_SHA']], cwd=ROOT, text=True).splitlines()
    if '--published-releases' in sys.argv:
        input_sha = subprocess.check_output(
            [sys.executable, str(ROOT / 'tools/firmware_input_hash.py'), '4'],
            cwd=ROOT, text=True
        ).strip()
        result = published_version(tags, same, os.environ['GITHUB_SHA'],
                                   input_sha, os.environ['GITHUB_REPOSITORY'])
    else:
        result = resolve_version(tags, same)
    print(json.dumps(result))

if __name__ == '__main__':
    main()
