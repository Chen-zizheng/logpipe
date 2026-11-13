#!/bin/bash
set -u
cd /root/logpipe/build
rm -rf /tmp/lp_logs
./logpipe --port 9100 --out /tmp/lp_logs --rotate-mb 8 --keep 5 --threads 2 --queue 65536 --web /root/logpipe/web > /tmp/lp_server.log 2>&1 &
SRV=$!
sleep 0.7
for cfg in "4 25000" "8 25000" "16 25000"; do
  set -- $cfg
  echo "== conns=$1 lines=$2 =="
  ./logpipe_bench --port 9100 --conns $1 --lines $2 --line-bytes 96 | tail -3
  sleep 1.5
  curl -s http://127.0.0.1:9100/api/stats; echo
done
echo "-- files --"
ls -l /tmp/lp_logs | head
wc -l /tmp/lp_logs/app.log*
kill $SRV; sleep 0.5
echo "-- server --"; cat /tmp/lp_server.log
