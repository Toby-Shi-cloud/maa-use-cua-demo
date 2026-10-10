#! /bin/bash

set -auo pipefail

for i in $(seq 1 10); do

./build/cua-shot --app TouchAlternativesSample --gesture-click \
  --x 1024 --y 832

sleep 2

./build/live-relay --replay gesture.move.cgevents 1758 38888
# ./build/cua-shot --app TouchAlternativesSample --gesture \
#   --from-x 1024 --from-y 832 \
#   --to-x 1576.79 --to-y 267.21 \
#   --steps 20 --duration-ms 400 \
#   --gesture-easing smoothstep \
#   --gesture-end-hold-ms 100

sleep 0.3

./build/cua-shot --app TouchAlternativesSample --output \
  screenshots/TAS_$i.png

sleep 2

done
