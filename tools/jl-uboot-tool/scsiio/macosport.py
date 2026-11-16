"""
macOS SCSIDev backend for the JieLi UBOOT tool.

Talks to the raw SCSI layer via the compiled `jlsgio` helper
(IOKit SCSITaskUserClient). Requires the helper binary next to this file:

    clang -O2 -o jlsgio jlsgio.c -framework IOKit -framework CoreFoundation
"""

import os
import shutil
import subprocess

from scsiio.common import SCSIDevBase, SCSIException

_HELPER = None
_TIMEOUT_MS = 10000


def _helper_path():
    global _HELPER
    if _HELPER is None:
        cand = os.path.join(os.path.dirname(os.path.abspath(__file__)), "jlsgio")
        if not os.path.isfile(cand) or not os.access(cand, os.X_OK):
            cand = shutil.which("jlsgio")
        if not cand:
            raise ImportError(
                "jlsgio helper not found. Build it in scsiio/: "
                "clang -O2 -o jlsgio jlsgio.c -framework IOKit -framework CoreFoundation"
            )
        _HELPER = cand
    return _HELPER


def _parse_output(out):
    status = service = xfer = None
    sense = b""
    data = None
    for line in (out or "").splitlines():
        if line.startswith("STATUS="):
            status = int(line[7:])
        elif line.startswith("SERVICE="):
            service = int(line[8:])
        elif line.startswith("XFER="):
            xfer = int(line[5:])
        elif line.startswith("SENSE="):
            sense = bytes.fromhex(line[6:])
        elif line.startswith("DATA="):
            data = bytes.fromhex(line[5:])
    return status, service, xfer, sense, data


class SCSIDev(SCSIDevBase):
    """
    SCSI generic I/O via the jlsgio helper (IOKit SCSITaskUserClient).
    """

    def open(self, path):
        self.path = path
        self.is_open = True

        # Best-effort unmount so we can obtain exclusive SCSITask access.
        if path.startswith("/dev/disk"):
            subprocess.run(
                ["diskutil", "unmountDisk", path],
                capture_output=True,
            )

    def close(self):
        self.is_open = False

    def execute(self, cdb, data_out, data_in, max_sense_len=32, return_sense_buffer=False):
        args = [_helper_path(), "exec", "--dev", self.path,
                "--cdb", bytes(cdb).hex(),
                "--timeout", str(_TIMEOUT_MS)]

        if data_out:
            args += ["--dout", bytes(data_out).hex()]
        elif data_in:
            args += ["--din", str(len(data_in))]

        try:
            p = subprocess.run(args, capture_output=True, text=True, timeout=_TIMEOUT_MS / 1000 + 30)
        except FileNotFoundError as e:
            raise SCSIException("jlsgio not found: %s" % e)

        status, service, xfer, sense, data = _parse_output(p.stdout)

        if status is None:
            raise SCSIException(
                "jlsgio failed (rc=%d): %s" % (p.returncode, (p.stderr or p.stdout or "").strip())
            )

        if data_in and data is not None:
            data_in[: len(data)] = data

        if status != 0:  # kSCSITaskStatus_GOOD == 0
            msg = "Task status %s, service %s" % (status, service)
            if sense:
                msg += "\nSense: %s" % sense.hex(" ")
            raise SCSIException(msg)

        resid = (len(data_in) - len(data)) if (data_in and data is not None) else 0
        if return_sense_buffer:
            return resid, sense[:max_sense_len]
        return resid


def scan():
    """Return [(path, vendor, product, rev), ...] for all USB SCSI-task devices.

    Only USB devices are probed (internal SATA/NVMe disks are left alone).
    """
    try:
        p = subprocess.run([_helper_path(), "scan"], capture_output=True, text=True, timeout=30)
    except FileNotFoundError as e:
        raise SCSIException("jlsgio not found: %s" % e)

    devs = []
    for line in (p.stdout or "").splitlines():
        parts = line.split("\t")
        if len(parts) >= 4:
            devs.append((parts[0], parts[1], parts[2], parts[3]))
    return devs