#!/usr/bin/env python3
"""Repeat the fixed Sample deployment gesture, saving screenshots and both traces.

Runs only against the current TouchAlternativesSample window. Receiver hooks must
have pairEvents=false and preScrollDelayUS=0. No game interaction is performed.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
SHOT = ROOT / 'build/cua-shot'
CAPTURE_RETRIES = []


def command(args, env=None):
    for attempt in range(3):
        p = subprocess.run(list(map(str, args)), cwd=ROOT, env=env, capture_output=True,
                           text=True, timeout=15)
        if not p.returncode:
            return p.stdout, p.stderr
        # This failure happens before any input action in cua-shot. Retry only
        # that preflight capture, and retain it separately from gesture results.
        if 'Error: get_window_state:' not in p.stderr or attempt == 2:
            raise RuntimeError(f'{args}: {p.stdout} {p.stderr}')
        CAPTURE_RETRIES.append({'args':list(map(str,args)), 'error':p.stderr})
        time.sleep(.2)


def frontmost():
    value, _ = command([ROOT / 'build/sample-frontmost'])
    pid, name = value.strip().split('\t', 1)
    return {'pid': int(pid), 'name': name}


def measure(path):
    value, _ = command([ROOT / 'build/sample-measure-ball', path])
    match = re.search(r'size=(\d+)x(\d+) center=\(([^,]+),([^\)]+)\) bounds=\((\d+),(\d+),(\d+),(\d+)\) n=(\d+)', value)
    if not match:
        raise RuntimeError(value)
    w, h, x, y, lx, ly, hx, hy, n = match.groups()
    answer = {'size': [int(w), int(h)], 'center': [float(x), float(y)],
              'bounds': list(map(int, (lx, ly, hx, hy))), 'pixels': int(n)}
    if not all(math.isfinite(v) for v in answer['center']) or int(n) < 10000:
        raise RuntimeError(f'Missing or clipped circle: {answer}')
    if int(lx) <= 1 or int(ly) <= 1 or int(hx) >= int(w)-2 or int(hy) >= int(h)-2:
        raise RuntimeError(f'Clipped circle: {answer}')
    return answer


def receiver_report(rows):
    depth = 0
    inside, outside = [], []
    for row in rows:
        kind = row['kind']
        if kind == 'scrollDrag.before':
            depth += 1
        elif kind == 'scrollDrag.exit':
            depth -= 1
        elif kind == 'queue.dequeue' and row.get('subtype') == 6:
            (inside if depth else outside).append(row)
    if depth:
        raise RuntimeError('Incomplete receiver trace')
    pans = [x for x in rows if x['kind'] == 'pan.action']
    begin = next((x for x in pans if x['state'] == 1), None)
    changed = [x for x in pans if x['state'] == 2]
    touches = [x for x in rows if x['kind'].startswith('touch.')]
    sub = lambda a, b: [a[k]-b[k] for k in (0, 1)]
    result = {'inside_count': len(inside), 'outside_count': len(outside),
              'inside_delta': [sum(x[k] for x in inside) for k in ('dx','dy')],
              'outside_delta': [sum(x[k] for x in outside) for k in ('dx','dy')],
              'outside_nonzero': [{'seq': x['seq'], 'phase': x['gesturePhase'],
                                    'delta': [x['dx'], x['dy']]} for x in outside if x['dx'] or x['dy']],
              'pan_changed_count': len(changed), 'touch_count': len(touches)}
    if begin and changed:
        last = changed[-1]
        result.update(pan_begin=begin['location'], pan_last=last['location'],
                      circle_delta=sub(last['circleAfter'], begin['startCircle']),
                      view_bounds=begin['bounds'])
        if touches:
            result.update(touch_begin=touches[0]['location'], touch_end=touches[-1]['location'],
                          recognition_offset=sub(begin['location'], touches[0]['location']),
                          tail_delta=sub(touches[-1]['location'], last['location']))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--receiver', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--case', default='baseline')
    parser.add_argument('--count', type=int, default=10)
    parser.add_argument('--first', type=int, default=1)
    parser.add_argument('--duration', type=int, default=400)
    parser.add_argument('--steps', type=int, default=26)
    parser.add_argument('--end-hold', type=int, default=40)
    parser.add_argument('--order', default='')
    parser.add_argument('--post-hold-us', type=int, default=0)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    listing, _ = command([SHOT, '--list', '--app', 'TouchAlternativesSample'])
    lines = listing.strip().splitlines()
    if len(lines) != 2:
        raise RuntimeError(listing)
    window, pid = map(int, lines[1].split('\t')[:2])
    env = {k: v for k, v in os.environ.items() if not k.startswith('CUA_GESTURE_')}
    env['CUA_GESTURE_GATE'] = 'step'
    if args.post_hold_us:
        env['CUA_GESTURE_POST_HOLD_US'] = str(args.post_hold_us)
    def shot(name):
        path = args.out / f'{name}.png'
        command([SHOT, '--window-id', window, '--output', path], env)
        return measure(path)
    try:
        for n in range(args.first, args.first + args.count):
            retry_start = len(CAPTURE_RETRIES)
            name = f'{args.case}-{n:03}'
            target = args.out / f'{name}.json'
            if target.exists():
                raise RuntimeError(f'Existing trial: {target}')
            fg = frontmost()
            if fg['pid'] == pid:
                raise RuntimeError('Sample is foreground; select another app before testing')
            (args.out / f'{name}-attempt.json').write_text(json.dumps({
                'case':args.case,'trial':n,'front_before':fg,'pid':pid,'window_id':window,
                'receiver':str(args.receiver),'args':vars(args)}, default=str, indent=2)+'\n')
            command([SHOT, '--window-id', window, '--gesture-click', '--x',1973,'--y',1138], env)
            time.sleep(.4)
            reset = shot(name + '-reset')
            if reset['size'] != [3946,2276] or abs(reset['center'][0]-1973) > 2 or abs(reset['center'][1]-1170) > 2:
                raise RuntimeError(f'Unexpected window or reset: {reset}')
            command([SHOT, '--window-id',window,'--gesture','--from-x',1973,'--from-y',1170,
                     '--to-x',600,'--to-y',2000,'--steps',36,'--duration-ms',400], env)
            time.sleep(.25)
            before = shot(name + '-before')
            start_offset = args.receiver.stat().st_size
            send_env = dict(env, CUA_GESTURE_PRELUDE='changed', CUA_GESTURE_PRIME='1',
                            CUA_GESTURE_PAN_PRIME='15', CUA_GESTURE_TRACE=str(args.out / f'{name}-send.csv'))
            if args.order:
                send_env['CUA_GESTURE_ORDER'] = args.order
            sender, stderr = command([SHOT,'--window-id',window,'--gesture','--from-x',545,
                '--from-y',2089,'--to-x',2904,'--to-y',556,'--steps',args.steps,
                '--duration-ms',args.duration,'--gesture-easing','ease-out-quad',
                '--gesture-field-scale-x',1.315,'--gesture-field-scale-y',1.315,
                '--gesture-start-hold-ms',100,'--gesture-end-hold-ms',args.end_hold], send_env)
            time.sleep(.65)
            after = shot(name + '-after')
            with args.receiver.open('rb') as f:
                f.seek(start_offset)
                receiver = [json.loads(x) for x in f.read().splitlines() if x]
            send = list(csv.DictReader((args.out / f'{name}-send.csv').open()))
            lo = int(send[0]['before_ns']) / 1e9
            hi = int(send[-1]['after_ns']) / 1e9 + .5
            receiver = [x for x in receiver if lo <= x['uptime'] <= hi]
            (args.out / f'{name}-receiver.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in receiver))
            delta = [after['center'][i] - before['center'][i] for i in (0,1)]
            final_fg = frontmost()
            report = {'case':args.case,'trial':n,'pid':pid,'window_id':window,
                      'measurement_version':'blue-profile-v2',
                      'duration_ms':args.duration,'steps':args.steps,'end_hold_ms':args.end_hold,
                      'order':args.order,'post_hold_us':args.post_hold_us,'front_before':fg,
                      'front_after':final_fg,'reset':reset,'before':before,'after':after,'delta':delta,
                      'near_target':max(abs(delta[0]-2359),abs(delta[1]+1533))<2,
                      'receiver':receiver_report(receiver),'sender_stdout':sender,'sender_stderr':stderr}
            report['capture_retries'] = CAPTURE_RETRIES[retry_start:]
            target.write_text(json.dumps(report,indent=2)+'\n')
            with (args.out/'results.jsonl').open('a') as f:
                f.write(json.dumps(report)+'\n')
            print(json.dumps({'case':args.case,'trial':n,'delta':[round(x,3) for x in delta],
                              'near_target':report['near_target'],'inside':report['receiver']['inside_count'],
                              'outside_nonzero':report['receiver']['outside_nonzero'],
                              'tail':report['receiver'].get('tail_delta')}),flush=True)
            if final_fg['pid'] == pid:
                raise RuntimeError('Sample became foreground during test')
    finally:
        try:
            os.kill(pid, signal.SIGCONT)
        except ProcessLookupError:
            pass


if __name__ == '__main__':
    main()
