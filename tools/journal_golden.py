#!/usr/bin/env python3
"""Encodes the journal in tests/unit/test_journal.cpp without using any of the
engine's code, so the bytes it prints are a second opinion on journal.hpp.

The layout below is written from ADR-010. Run it and paste the output over
kGolden in the test if the format changes on purpose (bump the version too).
"""
import struct
import zlib

CAPACITY = 65536
MAGIC = b"LOMEJRNL"

LIMIT, MARKET, CANCEL, MODIFY = 1, 2, 3, 4
BUY, SELL = 0, 1
GTC, IOC, FOK = 0, 1, 2


def with_crc(body: bytes) -> bytes:
    return body + struct.pack("<I", zlib.crc32(body))


def header(capacity: int) -> bytes:
    return with_crc(MAGIC + struct.pack("<II", 1, capacity))


def limit(id_, side, tif, price, qty, account):
    return with_crc(struct.pack("<BQBBqII", LIMIT, id_, side, tif, price, qty, account))


def market(id_, side, qty, account):
    return with_crc(struct.pack("<BQBII", MARKET, id_, side, qty, account))


def cancel(id_):
    return with_crc(struct.pack("<BQ", CANCEL, id_))


def modify(id_, price, qty):
    return with_crc(struct.pack("<BQqI", MODIFY, id_, price, qty))


records = [
    limit(1, BUY, GTC, 100, 10, 1),
    limit(2, SELL, IOC, 101, 5, 2),
    limit(3, BUY, FOK, -5, 4294967295, 7),
    market(4, SELL, 3, 9),
    cancel(2**64 - 1),
    modify(1, -(2**63), 1),
]

data = header(CAPACITY) + b"".join(records)
print("// %d bytes: header, then %d records" % (len(data), len(records)))
for i in range(0, len(data), 12):
    print("    " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
