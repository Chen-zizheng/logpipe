#!/bin/bash
set -u
cd /root/logpipe && cp -r /mnt/d/deepharnes/projects/logpipe/bench/bench.cpp bench/ && cmake --build build -j16 2>&1 | grep -E 'error|warning' | head -10
cd build
rm -rf /tmp/lp_logs
./logpipe --port 9100 --out /tmp/lp_logs --rotate-mb 16 --keep 5 --threads 4 --queue 65536 --web /root/logpipe/web > /tmp/lp_server.log 2>&1 &
SRV=$!
sleep 0.5
echo "== sustained 10s, 8 conns =="
./logpipe_bench --port 9100 --conns 8 --duration 10 --line-bytes 96 | tail -4
sleep 2
echo "-- stats after drain --"
curl -s http://127.0.0.1:9100/api/stats; echo
echo "-- disk --"
du -sh /tmp/lp_logs; ls /tmp/lp_logs | wc -l
kill $SRV; sleep 0.3
