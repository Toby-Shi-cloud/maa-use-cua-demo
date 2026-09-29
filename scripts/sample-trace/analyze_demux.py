"""Align sender traces with receiver logs and audit ScrollDrag dequeue scope.
Usage: python3 analyze_demux.py RECEIVER_JSONL SENDER_CSV [SENDER_CSV ...]
Only infers scope from synchronous before/exit markers; does not read accumulator.
"""
import csv
import json
import sys
from pathlib import Path

rows = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
for filename in sys.argv[2:]:
    sender = list(csv.DictReader(open(filename)))
    lo = int(sender[0]['before_ns']) / 1e9
    hi = int(sender[-1]['after_ns']) / 1e9 + .3
    trial = [row for row in rows if lo <= row['uptime'] <= hi]
    depth = 0
    inside, outside = [], []
    for row in trial:
        if row['kind'] == 'scrollDrag.before':
            depth += 1
        elif row['kind'] == 'scrollDrag.exit':
            depth -= 1
            assert depth >= 0, 'Unbalanced ScrollDrag markers'
        elif row['kind'] == 'queue.dequeue' and row.get('subtype') == 6:
            (inside if depth else outside).append(row)
    assert depth == 0, 'Incomplete ScrollDrag call'
    pans = [row for row in trial if row['kind'] == 'pan.action']
    begin = next((row for row in pans if row['state'] == 1), None)
    changed = [row for row in pans if row['state'] == 2]
    if begin is None or not changed:
        print(json.dumps(dict(trial=Path(filename).stem, incomplete_pan=True,
            inside_count=len(inside), outside_count=len(outside))))
        continue
    last = changed[-1]
    # The current Sample logs its view-space position; no transforms changed.
    actual = [last['location'][k] - begin['location'][k] for k in (0, 1)]
    print(json.dumps(dict(
        trial=Path(filename).stem, pan_begin=begin['location'], pan_last=last['location'],
        circle_delta=actual, inside_count=len(inside), outside_count=len(outside),
        inside_delta=[sum(row[key] for row in inside) for key in ('dx','dy')],
        outside_delta=[sum(row[key] for row in outside) for key in ('dx','dy')],
        outside_changed_seq=[row['seq'] for row in outside if row['gesturePhase']==2],
    )))
