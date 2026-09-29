#!/usr/bin/env python3
"""Summarize pan/touch endpoints; coordinates in UIKit points (not screenshot pixels)."""
import json, sys
from pathlib import Path

def sub(a,b): return [round(x-y,6) for x,y in zip(a,b)]
def report(rows):
    pans=[r for r in rows if r['kind']=='pan.action']
    began=next((r for r in pans if r['state']==1),None)
    changes=[r for r in pans if r['state']==2]
    ended=next((r for r in reversed(pans) if r['state'] in (3,4)),None)
    touches=[r for r in rows if r['kind'].startswith('touch.')]
    if not began or not changes: return
    last=changes[-1]
    print(json.dumps(dict(
        first_seq=rows[0]['seq'], start=began['startCircle'],
        touch_start=touches[0]['location'] if touches else None,
        touch_end=touches[-1]['location'] if touches else None,
        touch_delta=sub(touches[-1]['location'],touches[0]['location']) if touches else None,
        pan_start=began['location'], pan_last_changed=last['location'],
        pan_end=ended['location'] if ended else None,
        circle_delta=sub(last['circleAfter'],began['startCircle']),
        recognition_offset=sub(began['location'],touches[0]['location']) if touches else None,
        ignored_end_delta=sub(ended['location'],last['location']) if ended else None,
        touch_tail_after_last_changed=sub(touches[-1]['location'],last['location']) if touches else None,
        view_bounds=began.get('bounds'),
        changed_count=len(changes), touch_count=len(touches),
        coalesced_count=sum(len(r.get('coalesced',[])) for r in touches),
        max_delivery_lag_ms=round(max((r['uptime']-r['timestamp'])*1000 for r in touches),3) if touches else None,
        end_state=ended['state'] if ended else None),ensure_ascii=False))

for path in sys.argv[1:]:
    rows=[]
    for line in Path(path).read_text().splitlines():
        r=json.loads(line)
        if r['kind']=='touch.began' and rows:
            report(rows); rows=[]
        rows.append(r)
    report(rows)
