#!/bin/sh
# One-shot setup for the JieLi watch rescue on macOS.
set -e
cd "$(dirname "$0")"

echo "==> creating venv"
python3 -m venv tools/.venv

echo "==> installing deps"
tools/.venv/bin/pip install -q crcmod pyyaml pycryptodomex tqdm

echo "==> building macOS SCSI helper"
sh tools/jl-uboot-tool/scsiio/build.sh

echo
echo "done. use it like this:"
echo "  tools/.venv/bin/python tools/jl-uboot-tool/jldevfind.py"
echo "  tools/.venv/bin/python tools/jl-uboot-tool/jluboottool.py --chip br28"