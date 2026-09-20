#!/usr/bin/env python3
"""Check the combined source state and every recovered ToolBook checkpoint."""
import argparse
import json
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--combined', required=True, help='temporary merge of all three series')
    args = parser.parse_args()
    data = json.loads(Path(__file__).with_name('provenance.json').read_text())
    def git(*a):
        return subprocess.check_output(['git', '-C', str(args.repository), *a])
    def tree(ref, path=None):
        a = ['ls-tree', '-r', '-z', ref]
        if path:
            a += ['--', path]
        entries = {}
        for entry in git(*a).split(b'\0'):
            if entry:
                attr, name = entry.split(b'\t', 1)
                mode, _, blob = attr.decode().split()
                entries[name.decode()] = dict(mode=mode, blob=blob)
        return entries
    expected = tree(data['base'])
    expected.update(data['source_files'])
    actual = {p: e for p, e in tree(args.combined).items() if not p.startswith('devtools/')}
    differences = [p for p in expected.keys() | actual.keys() if expected.get(p) != actual.get(p)]
    if differences:
        raise SystemExit('Source mismatch: ' + ', '.join(differences))
    checked = 0
    for state in data['patches'] + data['nested']:
        if not state['toolbook']:
            continue
        commits = data['mapping'].get(state['origin'] + ':' + state['sha'], [])
        if not any(tree(commit, 'engines/toolbook') == state['toolbook'] for commit in commits):
            raise SystemExit('Missing ToolBook state: ' + state['sha'])
        checked += 1
    print(f'PASS: {len(expected)} source files match the original; {checked} ToolBook checkpoint records represented.')

if __name__ == '__main__':
    main()
