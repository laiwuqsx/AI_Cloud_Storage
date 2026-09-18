#!/bin/sh
set -eu

role="${1:-}"

case "$role" in
    tracker)
        mkdir -p /data/fastdfs/tracker
        exec fdfs_trackerd /etc/fdfs/tracker.conf -N
        ;;
    storage)
        mkdir -p /data/fastdfs/storage-meta /data/fastdfs/storage
        exec fdfs_storaged /etc/fdfs/storage.conf -N
        ;;
    *)
        echo "usage: start-fastdfs tracker|storage" >&2
        exit 64
        ;;
esac
