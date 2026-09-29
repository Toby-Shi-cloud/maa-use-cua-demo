"""Usage: align_queue.py RECEIVER.jsonl SENDER.csv [SENDER.csv ...]"""
import json,csv,sys
from pathlib import Path
r=[json.loads(l) for l in open(sys.argv[1])]
for path in sys.argv[2:]:
 f=Path(path)
 if not f.exists():continue
 s=list(csv.DictReader(f.open()));lo=int(s[0]['before_ns'])/1e9;hi=int(s[-1]['after_ns'])/1e9+.3
 a=[x for x in r if lo<=x['uptime']<=hi];q=[x for x in a if x['kind']=='queue.dequeue' and x.get('subtype')==6];t=[x for x in a if x['kind'].startswith('touch.')];pan=[x for x in a if x['kind']=='pan.action'];b=next(x for x in pan if x['state']==1);c=[x for x in pan if x['state']==2][-1]
 print(json.dumps(dict(trial=f.name,received=len(q),sum_dx=sum(x['dx'] for x in q),touch_start=t[0]['location'][0],touch_end=t[-1]['location'][0],touch_dx=t[-1]['location'][0]-t[0]['location'][0],circle_dx=c['circleAfter'][0]-b['circleBefore'][0],recognition_dx=b['location'][0]-t[0]['location'][0],tail=t[-1]['location'][0]-c['location'][0])))
