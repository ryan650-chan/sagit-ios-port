#!/usr/bin/env python3
"""Pull the sagit-port kernel log from the phone through adb.

The kernel writer copies the console ring into the last 32 MiB of the userdata
partition every two seconds (the APFS container is smaller than the partition,
so that tail is unused). After a hang, boot the Android recovery (TWRP), plug
USB, and run:

    python tools/sagit-log-pull.py

It dd's the window out, pulls it, prints the newest lines and writes a
timestamped report to the Desktop (``2026-10-26_13-05-12.txt``) containing the
geometry header plus the whole captured ring, not just the printed tail.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import struct
import subprocess
import sys
import time
import zlib

WINDOW = 32 << 20
MAGIC = b'SAGITLG1'
MAGIC2 = b'SAGITLG2'
USERDATA = '/dev/block/by-name/userdata'
# Real userdata size on sagit, measured with blockdev --getsize64 (112 GiB).
# 12,654,215,168 is the size of the ios.apfs *image*, not the partition; using it
# would point the window offset at the wrong place.
DEFAULT_PARTITION = 120410058752
# `adb wait-for-device` only waits for state "device"; a phone sitting in
# recovery reports "recovery" and would block forever.
READY_STATES = {'device', 'recovery'}

# Header v2 (kernel/bsd/miscfs/devfs/sagit_bootlog.c). Layout inside the window:
# block 0 header, blocks 1..32 the kernel message buffer, oldest byte first.
HDR2 = struct.Struct('<8sIIIIQQQQQQQQQIIIIII')
BLOCK = 4096


def resolve_adb(explicit: str | None) -> str:
    """Find a usable adb: explicit flag, env var, PATH, then common installs."""
    import os
    import shutil

    candidates = [explicit, os.environ.get('SAGIT_ADB'), shutil.which('adb')]
    candidates += [
        r'D://platform-tools//adb.exe',
        os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk',
                     'platform-tools', 'adb.exe'),
        r'C://platform-tools//adb.exe',
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    raise SystemExit('adb not found: pass --adb <path> or put it on PATH')


def adb_cmd(serial: str | None, exe: str | None = None) -> list[str]:
    return [resolve_adb(exe)] + (['-s', serial] if serial else [])


def wait_for_device(adb: list[str], timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while True:
        out = subprocess.run(adb + ['devices'], capture_output=True, text=True).stdout
        states = {parts[1] for parts in
                  (line.split() for line in out.splitlines()[1:]) if len(parts) > 1}
        ready = states & READY_STATES
        if ready:
            print(f'adb state: {"/".join(sorted(ready))}')
            return
        if time.monotonic() > deadline:
            raise SystemExit(f'no usable adb device (states={sorted(states) or ["none"]})')
        time.sleep(1)


def default_save_dir() -> pathlib.Path:
    """Desktop, honouring a OneDrive-redirected profile; else the cwd."""
    home = pathlib.Path.home()
    candidates = []
    onedrive = os.environ.get('OneDrive') or os.environ.get('OneDriveConsumer')
    if onedrive:
        candidates.append(pathlib.Path(onedrive) / 'Desktop')
    candidates += [home / 'Desktop', home / '桌面']
    for path in candidates:
        if path.is_dir():
            return path
    return pathlib.Path.cwd()


def decode_v2(data: bytes, index: int, window_offset: int, partition_size: int):
    """Return (status, header_lines, body_text). status 2 means a torn read."""
    (magic, version, hdr_bytes, block_size, window_blocks, win_off, win_bytes,
     media_bytes, boot_id, seq, mtime, writes, errors, skipped, msg_bytes,
     msg_ring, msg_wrap, status, crc, _reserved) = HDR2.unpack_from(data, index)

    area = max(0, (window_blocks - 1) * block_size)
    raw_area = data[index + block_size:index + block_size + area]
    body = raw_area[:msg_bytes]
    header, torn = [], False
    if crc and area:
        check = zlib.crc32(raw_area)
        if check != crc:
            torn = True
            header.append(f'torn read: header crc 0x{crc:08x} but body crc 0x{check:08x} '
                          '(body written after the header was captured)')

    header.append(f'magic=SAGITLG2 version={version} boot_id={boot_id} seq={seq}')
    header.append(f'writer_geometry: media={media_bytes} window_offset={win_off} '
                  f'window_bytes={win_bytes} block={block_size} blocks={window_blocks}')
    header.append(f'reader_geometry: partition={partition_size} window_offset={window_offset}')
    if win_off != window_offset:
        header.append('MISMATCH: writer and reader disagree on the window start')
    header.append(f'msg={msg_bytes}/{msg_ring} wrap={msg_wrap} status=0x{status:x} '
                  f'writes={writes} errors={errors} skipped={skipped}')
    return (2 if torn else 0), header, body.decode('utf-8', 'replace')


def decode(data: bytes, window_offset: int, partition_size: int):
    """Return (status, header_lines, body_text)."""
    for magic, name in ((MAGIC2, 'SAGITLG2'), (MAGIC, 'SAGITLG1')):
        index = data.find(magic)
        if index >= 0:
            header = [f'{name} header at window +{index}']
            break
    else:
        return 1, ['no SAGITLG1/SAGITLG2 header in the window: '
                   'the kernel has not written a log yet'], ''

    if magic == MAGIC2:
        status, more, body = decode_v2(data, index, window_offset, partition_size)
        return status, header + more, body

    _, ring_size, _reserved, total, mtime, win_off = struct.unpack_from(
        '<8sIIQQQ', data, index)
    body = data[index + 64:index + 64 + ring_size].decode('utf-8', 'replace')
    header += [f'ring={ring_size} total={total} mtime={mtime} offset={win_off}']
    return 0, header, body


def save_report(directory: pathlib.Path, header: list[str], body: str,
                source: str) -> pathlib.Path:
    stamp = time.strftime('%Y-%m-%d_%H-%M-%S')
    path = directory / f'{stamp}.txt'
    lines = [f'sagit kernel log capture', f'generated {time.strftime("%Y-%m-%d %H:%M:%S")}',
             f'window file {source}', '-' * 60, *header, '-' * 60, body]
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-s', '--serial', help='adb serial when several devices are attached')
    parser.add_argument('-o', '--output', default='sagit-log.bin', help='local window file')
    parser.add_argument('--partition-bytes', type=int, default=DEFAULT_PARTITION,
                        help='userdata partition size (default: Mi 6)')
    parser.add_argument('--tail', type=int, default=200, help='lines to print')
    parser.add_argument('--retries', type=int, default=3,
                        help='re-reads when a capture lands mid-write (default 3)')
    parser.add_argument('--save-dir', type=pathlib.Path, default=None,
                        help='where to write the timestamped .txt (default: Desktop)')
    parser.add_argument('--no-save', action='store_true', help='do not write the .txt')
    parser.add_argument('--adb', help='path to the adb executable')
    args = parser.parse_args(argv)

    adb = adb_cmd(args.serial, args.adb)
    wait_for_device(adb)

    size = args.partition_bytes
    try:
        out = subprocess.check_output(adb + ['shell', f'blockdev --getsize64 {USERDATA}'],
                                      text=True, stderr=subprocess.DEVNULL).strip()
        if out.isdigit():
            size = int(out)
        else:
            print(f'blockdev gave {out!r}: falling back to {size}, the window offset may be wrong')
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f'blockdev failed ({exc}): falling back to {size}, the window offset may be wrong')

    offset = (size - WINDOW) & ~4095
    skip = offset // 4096
    count = WINDOW // 4096
    print(f'userdata={size} window_offset={offset} ({skip} blocks of 4096)')

    # The writer rewrites the body then the header every few seconds, so a
    # capture can land between the two and see a new body with the old header
    # (torn read). Re-read instead of printing a misleading CRC warning.
    status, header, body = 1, [], ''
    for attempt in range(1, args.retries + 1):
        subprocess.run(adb + ['shell',
                              f'dd if={USERDATA} of=/tmp/sagit-log.bin bs=4096 '
                              f'skip={skip} count={count}'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(adb + ['pull', '/tmp/sagit-log.bin', args.output],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        with open(args.output, 'rb') as stream:
            data = stream.read()
        status, header, body = decode(data, offset, size)
        if status != 2:
            break
        if attempt < args.retries:
            print(f'torn read (attempt {attempt}/{args.retries}); re-reading in 1 s')
            time.sleep(1.0)

    for line in header:
        print(line)
    lines = body.splitlines()
    for line in lines[-args.tail:]:
        print(line)

    if not args.no_save:
        directory = args.save_dir or default_save_dir()
        directory.mkdir(parents=True, exist_ok=True)
        path = save_report(directory, header, body, str(pathlib.Path(args.output).resolve()))
        print(f'\nsaved {len(lines)} lines -> {path}')

    return status


if __name__ == '__main__':
    sys.exit(main())
