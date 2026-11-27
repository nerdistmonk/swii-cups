#!/usr/bin/env python3
"""Query-only probe for a StyleWriter on a serial port.

Sends status queries and prints every byte the printer returns. It never
sends reset, eject, speed change, or page data, so it uses no paper or ink.
Run it as root or as a user in the group that owns the serial device.
"""
import argparse
import os
import select
import termios
import time

# Each frame is FF FF FF followed by the query letter (see README.protocol).
# 'I' (eject and reset), 'S', 'h', 'u', and data frames are deliberately absent.
QUERIES = [
    ("?", "ident query, sent by the Mac at job start"),
    ("1", "status, polled by the Mac after form feed"),
    ("2", "error status, 0x04 = out of paper"),
    ("B", "buffer status, 0xF8 = empty at job start"),
    ("H", "setup query, 0x01 in the Mac trace"),
    ("p", "model code, documented for CS printers"),
]

BAUDS = {
    9600: termios.B9600,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
}


def open_port(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag: raw input
    attrs[1] = 0  # oflag: raw output
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # 8N1, no flow control
    attrs[3] = 0  # lflag: no canonical mode or echo
    attrs[4] = BAUDS[baud]
    attrs[5] = BAUDS[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_window(fd, seconds):
    end = time.monotonic() + seconds
    data = b""
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return data
        ready, _, _ = select.select([fd], [], [], left)
        if ready:
            data += os.read(fd, 256)


def main():
    ap = argparse.ArgumentParser(description="Query-only StyleWriter probe.")
    ap.add_argument("device", help="serial device, for example /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=57600, choices=sorted(BAUDS),
                    help="serial speed; the driver starts at 57600")
    ap.add_argument("--wait", type=float, default=1.0,
                    help="seconds to listen after each query")
    args = ap.parse_args()

    fd = open_port(args.device, args.baud)
    print(f"{args.device} at {args.baud} 8N1, queries only, no data sent")
    try:
        for code, note in QUERIES:
            termios.tcflush(fd, termios.TCIFLUSH)
            frame = b"\xff\xff\xff" + code.encode()
            os.write(fd, frame)
            reply = read_window(fd, args.wait)
            hex_reply = reply.hex(" ") if reply else "NONE"
            print(f"'{code}' ({note}): sent {frame.hex(' ')}, reply {hex_reply}")
            time.sleep(0.2)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
