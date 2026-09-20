#!/bin/sh
set -eu

spawn-fcgi -n -a 0.0.0.0 -p 10000 -f /app/bin_cgi/login &
spawn-fcgi -n -a 0.0.0.0 -p 10001 -f /app/bin_cgi/register &
spawn-fcgi -n -a 0.0.0.0 -p 10002 -f /app/bin_cgi/myfiles &
spawn-fcgi -n -a 0.0.0.0 -p 10003 -f /app/bin_cgi/md5 &
spawn-fcgi -n -a 0.0.0.0 -p 10004 -f /app/bin_cgi/dealfile &
spawn-fcgi -n -a 0.0.0.0 -p 10005 -f /app/bin_cgi/logout &
spawn-fcgi -n -a 0.0.0.0 -p 10006 -f /app/bin_cgi/upload &
spawn-fcgi -n -a 0.0.0.0 -p 10007 -f /app/bin_cgi/share &
spawn-fcgi -n -a 0.0.0.0 -p 10008 -f /app/bin_cgi/download &

wait
