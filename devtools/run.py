#!/usr/bin/env python3
"""Build or run a ScummVM worktree with explicit data and isolated outputs."""
import argparse
import configparser
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['build', 'run'])
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--engine', choices=['director', 'toolbook'], required=True)
    p.add_argument('--data', type=Path)
    p.add_argument('--output', type=Path)
    p.add_argument('--input', type=Path)
    p.add_argument('--seconds', type=float, default=40)
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--scenario', choices=['timed', 'tour'], default='timed')
    p.add_argument('--port', type=int, default=3778)
    p.add_argument('--linger', type=float, default=0,
                   help='keep a completed tour alive briefly for bridge inspection')
    p.add_argument('--audio', action='store_true')
    args = p.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    if args.action == 'build':
        build.mkdir(parents=True, exist_ok=True)
        if not (build / 'Makefile').exists():
            subprocess.run([str(source / 'configure'), '--disable-all-engines',
                            '--enable-engine=' + args.engine], cwd=build, check=True)
        return subprocess.run(['make', '-j' + str(args.jobs)], cwd=build).returncode
    if not args.data or not args.output:
        p.error('run requires --data and a new --output directory')
    output, data = args.output.resolve(), args.data.resolve()
    binary = build / 'scummvm'
    if not binary.is_file() or not data.is_dir():
        p.error('build binary or game data directory is missing')
    output.mkdir(parents=True, exist_ok=False)
    for name in ['frames', 'shots', 'saves']:
        (output / name).mkdir()
    target = 'sevenwitches-win-ru' if args.engine == 'director' else 'bashnya-toolbook'
    config = configparser.ConfigParser(interpolation=None)
    config['scummvm'] = dict(savepath=str(output / 'saves'), screenshotpath=str(output / 'frames'),
                             extrapath=str(source / 'dists/engine-data'),
                             macos_savepath_migrated='true', confirm_exit='false')
    config[target] = dict(path=str(data), engineid=args.engine, language='ru', platform='windows',
                          gameid='sevenwitches' if args.engine == 'director' else 'knowledgetower',
                          enable_unsupported_game_warning='false', framedump_ms='1000',
                          screenshotpath=str(output / 'frames'))
    if args.input:
        scenario = args.input.read_bytes()
        (output / 'input.txt').write_bytes(scenario)
        config[target]['inputscript'] = str(output / 'input.txt')
    if args.scenario == 'tour':
        if args.engine != 'director':
            p.error('tour requires director')
        config[target]['debugbridge_port'] = str(args.port)
    ini = output / 'scummvm.ini'
    with ini.open('w') as f:
        config.write(f, space_around_delimiters=False)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software',
               SDL_FRAMEBUFFER_ACCELERATION='0')
    if not args.audio:
        env['SDL_AUDIODRIVER'] = 'dummy'
    cmd = [str(binary), '-c', str(ini), '--logfile=' + str(output / 'scummvm.log'),
           '-g', 'surfacesdl', '-d', '2', target]
    metadata = dict(source=str(source), build=str(build), data=str(data),
                    source_commit=subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD']).decode().strip(),
                    source_status=subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain']).decode(),
                    binary_sha256=sha256(binary), config_sha256=sha256(ini), command=cmd,
                    started=datetime.now(timezone.utc).isoformat(), scenario=args.scenario)
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    proc = None
    result = {'success': False}
    logpath = output / 'run.log'
    try:
        with logpath.open('wb') as log:
            proc = subprocess.Popen(cmd, cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        if args.scenario == 'tour':
            sys.path.insert(0, str(Path(__file__).resolve().parent / 'workbench/tools'))
            import witches
            from svmbridge import core
            core.RUNDIR = output
            core.STATE = output / 'bridge-state.json'
            core.write_state(core.RunState(proc.pid, args.port, target, str(logpath), time.time()))
            original_command = core.command
            def recorded_command(line, port=None):
                reply = original_command(line, port)
                with (output / 'commands.jsonl').open('a') as f:
                    f.write(json.dumps(dict(time=time.time(), command=line, output=reply), ensure_ascii=False) + '\n')
                return reply
            core.command = recorded_command
            for title, pattern, action in witches.WAYPOINTS['main']:
                if pattern:
                    print(title, witches.wait_for(pattern), flush=True)
                else:
                    time.sleep(2)
                if action:
                    core.command(action)
            failures = witches.tour()
            result.update(tour_failures=failures, final_state=core.command('state'))
            result['success'] = failures == 0
            if args.linger:
                print(f'Tour finished; bridge remains available for {args.linger} seconds.', flush=True)
                time.sleep(args.linger)
        else:
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError(f'ScummVM exited early: {proc.returncode}')
                time.sleep(0.25)
            result['success'] = True
    except Exception as e:
        result['error'] = str(e)
    finally:
        if proc:
            # Only the process created by this invocation. Historical wrappers
            # use broad pkill patterns and are intentionally not called here.
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            result['process_returncode'] = proc.returncode
        text = logpath.read_bytes().decode('utf-8', 'replace') if logpath.exists() else ''
        result['frames'] = len(list((output / 'frames').glob('*.png')))
        result['barriers'] = [line for line in text.splitlines() if 'пока не реализован' in line]
        result['fatal_errors'] = [line for line in text.splitlines() if 'ERROR:' in line or 'Uncaught Lingo error' in line]
        if result['fatal_errors'] or (not result['frames'] and args.scenario == 'timed'):
            result['success'] = False
        result['finished'] = datetime.now(timezone.utc).isoformat()
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    return 0 if result['success'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
