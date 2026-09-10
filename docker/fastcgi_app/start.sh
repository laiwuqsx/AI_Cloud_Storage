#!/bin/sh
set -eu

spawn-fcgi -n -a 0.0.0.0 -p 10000 -f /app/bin_cgi/login &
spawn-fcgi -n -a 0.0.0.0 -p 10001 -f /app/bin_cgi/register &
spawn-fcgi -n -a 0.0.0.0 -p 10002 -f /app/bin_cgi/myfiles &

wait
