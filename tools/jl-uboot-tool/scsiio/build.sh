#!/bin/sh
# Build the macOS SCSI passthrough helper for the JieLi UBOOT tool.
set -e
cd "$(dirname "$0")"
clang -O2 -o jlsgio jlsgio.c -framework IOKit -framework CoreFoundation
echo "built scsiio/jlsgio"