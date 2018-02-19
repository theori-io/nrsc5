#!/usr/bin/env python
# Script to convert from old IQ files to new IQ files.
import struct
import sys

if len(sys.argv) != 3:
    print 'Usage:', sys.argv[0], 'INPUT-OLD OUTPUT-NEW'
    sys.exit(1)

fin = open(sys.argv[1], 'rb')
fout = open(sys.argv[2], 'wb')

while True:
    buf = fin.read(4096)
    if not buf:
        break

    out = ''.join([struct.pack('<h', (ord(x) - 127) << 8) for x in buf])
    fout.write(out)

sys.exit(0)
