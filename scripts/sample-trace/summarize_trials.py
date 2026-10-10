#!/usr/bin/env python3
"""Audit completed trial records against sender/receiver traces and summarize."""
import csv
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
rows = []
for path in sorted(root.glob('*.json')):
    row = json.loads(path.read_text())
    if not {'case','trial','delta','receiver','post_hold_us','front_after'} <= row.keys():
        continue
    name = f"{row['case']}-{row['trial']:03}"
    assert path.name == name + '.json', path
    assert row['measurement_version'] == 'blue-profile-v2', path
    assert row['front_before']['pid'] != row['pid'] and row['front_after']['pid'] != row['pid'], path
    assert row['before']['size'] == row['after']['size'] == [3946,2276], path
    assert max(abs(row['reset']['center'][0]-1973),abs(row['reset']['center'][1]-1170)) < 2, path
    sender = list(csv.DictReader((root / (name+'-send.csv')).open()))
    receiver = row['receiver']
    assert len(sender) == receiver['inside_count'] + receiver['outside_count'], path
    totals = [sum(float(x[k]) for x in sender) for k in ('dx','dy')]
    received = [receiver['inside_delta'][k] + receiver['outside_delta'][k] for k in (0,1)]
    assert max(abs(totals[k]-received[k]) for k in (0,1)) < 1e-4, (path,totals,received)
    delta = [row['after']['center'][k]-row['before']['center'][k] for k in (0,1)]
    assert delta == row['delta'], path
    assert row['near_target'] == (max(abs(delta[0]-2359),abs(delta[1]+1533))<2), path
    row['trace_span_ms'] = (int(sender[-1]['after_ns'])-int(sender[0]['before_ns']))/1e6
    row['max_step_call_ms'] = max((int(x['after_ns'])-int(x['before_ns']))/1e6 for x in sender)
    rows.append(row)
assert len({(x['case'],x['trial']) for x in rows}) == len(rows)
groups = []
for duration, hold in sorted({(x['duration_ms'],x['post_hold_us']) for x in rows}):
    trials = [x for x in rows if x['duration_ms']==duration and x['post_hold_us']==hold]
    groups.append({
        'duration_ms':duration,'post_hold_us':hold,'trials':len(trials),
        'near_target':sum(x['near_target'] for x in trials),
        'all_nonzero_internal':sum(not x['receiver']['outside_nonzero'] for x in trials),
        'zero_touch_tail':sum(max(abs(v) for v in x['receiver']['tail_delta'])<1e-5 for x in trials),
        'range_delta_px':[[min(x['delta'][k] for x in trials),max(x['delta'][k] for x in trials)] for k in (0,1)],
        'worst_axis_error_px':max(max(abs(x['delta'][0]-2359),abs(x['delta'][1]+1533)) for x in trials),
        'trace_span_ms':[min(x['trace_span_ms'] for x in trials),max(x['trace_span_ms'] for x in trials)],
        'max_step_call_ms':max(x['max_step_call_ms'] for x in trials),
        'capture_preflight_retries':sum(len(x['capture_retries']) for x in trials),
        'short_trials':[{'case':x['case'],'trial':x['trial'],'delta':x['delta'],
                         'outside_delta':x['receiver']['outside_delta'],
                         'touch_tail':x['receiver']['tail_delta']} for x in trials if not x['near_target']],
    })
summary={'target_delta_px':[2359,-1533],'axis_tolerance_px':2,'valid_trials':len(rows),
         'measurement_version':'blue-profile-v2','groups':groups,
         'excluded_attempts':['measurement-interruption.json']}
(root/'results.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in rows))
(root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
