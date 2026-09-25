#!/usr/bin/env python3
"""parquet_decimal_writer.py - minimal Parquet writer for DECIMAL columns, with an independent value oracle.

No dependencies (standard library only), so stateless tests can build their fixtures at run time. It writes exactly the
physical layout a test asks for, which the usual writers cannot: FIXED_LEN_BYTE_ARRAY of any width 1..64 with any declared
precision (also a precision narrower than the width, a shape real writers may produce), BYTE_ARRAY decimals of any byte
length 0..64 per value, PLAIN / BYTE_STREAM_SPLIT / DELTA_BYTE_ARRAY / RLE_DICTIONARY encodings, UNCOMPRESSED or GZIP pages,
data page V1 or V2, and REQUIRED or OPTIONAL columns. No statistics are written.

  parquet_decimal_writer.py matrix <out-dir>     write the Decimal512 read matrix (fixtures + expected.tsv per fixture)
  parquet_decimal_writer.py selftest             encoder checks against hand-computed byte strings

Library use: write_file(path, columns, codec=..., page_version=...), where each column is a dict with
  name, physical ('flba' | 'byte_array' | 'int32'), type_length (flba), precision, scale (decimals),
  encoding ('plain' | 'byte_stream_split' | 'delta_byte_array' | 'dict'), optional (bool),
  values: list of unscaled ints or None, sizes: per-value byte length for byte_array (default: minimal).
format_decimal(unscaled, scale) is the oracle for ClickHouse's text output of a Decimal (trailing zeros trimmed;
None -> "NULL", the marker the checks print for NULL).
"""
from __future__ import annotations

import os
import struct
import sys
import zlib

# ---- Thrift compact protocol (write side only) -------------------------------------------------------------------
CT_TRUE, CT_FALSE, CT_I32, CT_I64, CT_BINARY, CT_LIST, CT_STRUCT = 1, 2, 5, 6, 8, 9, 12


def varint(n: int) -> bytes:
    assert n >= 0
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def zigzag(n: int) -> int:
    return (n << 1) ^ (n >> 63) if n >= 0 else ((-n) << 1) - 1


class TStruct:
    """Fields as (id, kind, value); kind: 'i32', 'i64', 'bin', 'bool', 'struct', ('list', elem_kind)."""

    def __init__(self, *fields):
        self.fields = sorted((f for f in fields if f[2] is not None), key=lambda f: f[0])

    def encode(self) -> bytes:
        out = bytearray()
        last = 0
        for fid, kind, value in self.fields:
            if kind == "bool":
                ctype = CT_TRUE if value else CT_FALSE
            else:
                ctype = {"i32": CT_I32, "i64": CT_I64, "bin": CT_BINARY, "struct": CT_STRUCT}.get(kind, CT_LIST)
            delta = fid - last
            if 0 < delta <= 15:
                out.append((delta << 4) | ctype)
            else:
                out.append(ctype)
                out += varint(zigzag(fid))
            last = fid
            if kind != "bool":
                out += encode_value(kind, value)
        out.append(0)
        return bytes(out)


def encode_value(kind, value) -> bytes:
    if kind in ("i32", "i64"):
        return varint(zigzag(value))
    if kind == "bin":
        b = value.encode() if isinstance(value, str) else value
        return varint(len(b)) + b
    if kind == "struct":
        return value.encode()
    _, elem = kind
    ctype = {"i32": CT_I32, "i64": CT_I64, "bin": CT_BINARY, "struct": CT_STRUCT}[elem]
    n = len(value)
    head = bytes([(n << 4) | ctype]) if n < 15 else bytes([0xF0 | ctype]) + varint(n)
    return head + b"".join(encode_value(elem, v) for v in value)


# ---- Parquet enums ---------------------------------------------------------------------------------------------------
T_INT32, T_FLBA, T_BYTE_ARRAY = 1, 7, 6
REQUIRED, OPTIONAL = 0, 1
CONV_DECIMAL = 5
E_PLAIN, E_RLE, E_DELTA_BINARY_PACKED, E_DELTA_BYTE_ARRAY, E_RLE_DICTIONARY, E_BYTE_STREAM_SPLIT = 0, 3, 5, 7, 8, 9
CODEC = {"none": 0, "gzip": 2}
PAGE_DATA, PAGE_DICT, PAGE_DATA_V2 = 0, 2, 3


# ---- value encodings -------------------------------------------------------------------------------------------------
def be_twos(v: int, width: int) -> bytes:
    """Big-endian two's complement of v in exactly `width` bytes (width 0 only for v == 0)."""
    if width == 0:
        assert v == 0
        return b""
    lo, hi = -(1 << (8 * width - 1)), (1 << (8 * width - 1)) - 1
    assert lo <= v <= hi, f"{v} does not fit {width} bytes"
    return (v & ((1 << (8 * width)) - 1)).to_bytes(width, "big")


def min_width(v: int) -> int:
    w = 1
    while not (-(1 << (8 * w - 1)) <= v <= (1 << (8 * w - 1)) - 1):
        w += 1
    return w


def bitpack(values, width: int) -> bytes:
    """Parquet bit packing: values LSB first, little-endian bit order."""
    acc, nbits, out = 0, 0, bytearray()
    for v in values:
        acc |= v << nbits
        nbits += width
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


def delta_binary_packed(values) -> bytes:
    """DELTA_BINARY_PACKED with block size 128 and 4 miniblocks of 32."""
    block, minis = 128, 4
    per_mini = block // minis
    out = bytearray(varint(block) + varint(minis) + varint(len(values)) + varint(zigzag(values[0] if values else 0)))
    deltas = [values[i] - values[i - 1] for i in range(1, len(values))]
    for b in range(0, len(deltas), block):
        chunk = deltas[b:b + block]
        mn = min(chunk)
        out += varint(zigzag(mn))
        rel = [d - mn for d in chunk]
        widths, bodies = [], []
        for m in range(minis):
            part = rel[m * per_mini:(m + 1) * per_mini]
            if not part:
                widths.append(0)
                continue
            w = max(part).bit_length()
            widths.append(w)
            bodies.append(bitpack(part + [0] * (per_mini - len(part)), w))
        out += bytes(widths) + b"".join(bodies)
    return bytes(out)


def rle_runs(levels, width: int) -> bytes:
    """RLE/bit-packed hybrid using RLE runs only."""
    out, i = bytearray(), 0
    nbytes = (width + 7) // 8
    while i < len(levels):
        j = i
        while j < len(levels) and levels[j] == levels[i]:
            j += 1
        out += varint((j - i) << 1) + levels[i].to_bytes(nbytes, "little")
        i = j
    return bytes(out)


def encode_values(col, present):
    phys, enc = col["physical"], col["encoding"]
    if phys == "int32":
        assert enc == "plain"
        return b"".join(struct.pack("<i", v) for v in present), None
    if phys == "flba":
        w = col["type_length"]
        raw = [be_twos(v, w) for v in present]
    else:
        sizes = col.get("sizes")
        raw = [be_twos(v, (sizes[k] if sizes else min_width(v))) for k, v in enumerate(present)]
    if enc == "plain":
        if phys == "flba":
            return b"".join(raw), None
        return b"".join(struct.pack("<I", len(r)) + r for r in raw), None
    if enc == "byte_stream_split":
        assert phys == "flba"
        w, n = col["type_length"], len(raw)
        return bytes(raw[i][k] for k in range(w) for i in range(n)), None
    if enc == "delta_byte_array":
        # prefix length 0 for every value: the suffix is the whole value (a legal, if unhelpful, encoding)
        lengths = [len(r) for r in raw]
        return delta_binary_packed([0] * len(raw)) + delta_binary_packed(lengths) + b"".join(raw), None
    if enc == "dict":
        uniq = []
        index = {}
        for r in raw:
            if r not in index:
                index[r] = len(uniq)
                uniq.append(r)
        if phys == "flba":
            dict_page = b"".join(uniq)
        else:
            dict_page = b"".join(struct.pack("<I", len(r)) + r for r in uniq)
        bw = max(1, (len(uniq) - 1).bit_length())
        return bytes([bw]) + rle_runs([index[r] for r in raw], bw), (dict_page, len(uniq))
    raise ValueError(enc)


# ---- pages, chunks, file ---------------------------------------------------------------------------------------------
def compress(data: bytes, codec: str) -> bytes:
    if codec == "none":
        return data
    c = zlib.compressobj(6, zlib.DEFLATED, 31)
    return c.compress(data) + c.flush()


def column_chunk(col, offset: int, codec: str, page_version: int):
    values = col["values"]
    optional = col.get("optional", False)
    present = [v for v in values if v is not None]
    body, dictionary = encode_values(col, present)
    enc_id = {"plain": E_PLAIN, "byte_stream_split": E_BYTE_STREAM_SPLIT, "delta_byte_array": E_DELTA_BYTE_ARRAY,
              "dict": E_RLE_DICTIONARY}[col["encoding"]]
    out = bytearray()
    dict_offset = None
    uncompressed_total = 0
    if dictionary:
        dict_page, n = dictionary
        comp = compress(dict_page, codec)
        hdr = TStruct((1, "i32", PAGE_DICT), (2, "i32", len(dict_page)), (3, "i32", len(comp)),
                      (7, "struct", TStruct((1, "i32", n), (2, "i32", E_PLAIN)))).encode()
        dict_offset = offset
        out += hdr + comp
        uncompressed_total += len(hdr) + len(dict_page)
    data_offset = offset + len(out)
    levels = rle_runs([0 if v is None else 1 for v in values], 1) if optional else b""
    nulls = sum(v is None for v in values)
    if page_version == 1:
        payload = (struct.pack("<I", len(levels)) + levels if optional else b"") + body
        comp = compress(payload, codec)
        hdr = TStruct((1, "i32", PAGE_DATA), (2, "i32", len(payload)), (3, "i32", len(comp)),
                      (5, "struct", TStruct((1, "i32", len(values)), (2, "i32", enc_id), (3, "i32", E_RLE),
                                            (4, "i32", E_RLE)))).encode()
        out += hdr + comp
        uncompressed_total += len(hdr) + len(payload)
    else:
        comp = compress(body, codec)
        hdr = TStruct((1, "i32", PAGE_DATA_V2), (2, "i32", len(levels) + len(body)), (3, "i32", len(levels) + len(comp)),
                      (8, "struct", TStruct((1, "i32", len(values)), (2, "i32", nulls), (3, "i32", len(values)),
                                            (4, "i32", enc_id), (5, "i32", len(levels)), (6, "i32", 0),
                                            (7, "bool", codec != "none")))).encode()
        out += hdr + levels + comp
        uncompressed_total += len(hdr) + len(levels) + len(body)
    phys = {"flba": T_FLBA, "byte_array": T_BYTE_ARRAY, "int32": T_INT32}[col["physical"]]
    encodings = sorted({E_RLE, enc_id} | ({E_PLAIN} if dictionary else set()))
    meta = TStruct((1, "i32", phys), (2, ("list", "i32"), encodings), (3, ("list", "bin"), [col["name"]]),
                   (4, "i32", CODEC[codec]), (5, "i64", len(values)), (6, "i64", uncompressed_total),
                   (7, "i64", len(out)), (9, "i64", data_offset), (11, "i64", dict_offset))
    chunk = TStruct((2, "i64", offset), (3, "struct", meta))
    return bytes(out), chunk, len(out)


def schema_element(col):
    phys = {"flba": T_FLBA, "byte_array": T_BYTE_ARRAY, "int32": T_INT32}[col["physical"]]
    rep = OPTIONAL if col.get("optional") else REQUIRED
    if col["physical"] == "int32" and "precision" not in col:
        return TStruct((1, "i32", phys), (3, "i32", rep), (4, "bin", col["name"]))
    logical = TStruct((5, "struct", TStruct((1, "i32", col["scale"]), (2, "i32", col["precision"]))))
    return TStruct((1, "i32", phys), (2, "i32", col.get("type_length")), (3, "i32", rep), (4, "bin", col["name"]),
                   (6, "i32", CONV_DECIMAL), (7, "i32", col["scale"]), (8, "i32", col["precision"]),
                   (10, "struct", logical))


def write_file(path: str, columns, codec: str = "none", page_version: int = 1):
    nrows = len(columns[0]["values"])
    assert all(len(c["values"]) == nrows for c in columns)
    out = bytearray(b"PAR1")
    chunks, total = [], 0
    for col in columns:
        data, chunk, size = column_chunk(col, len(out), codec, page_version)
        out += data
        chunks.append(chunk)
        total += size
    schema = [TStruct((4, "bin", "schema"), (5, "i32", len(columns)))] + [schema_element(c) for c in columns]
    row_group = TStruct((1, ("list", "struct"), chunks), (2, "i64", total), (3, "i64", nrows))
    meta = TStruct((1, "i32", 1), (2, ("list", "struct"), schema), (3, "i64", nrows),
                   (4, ("list", "struct"), [row_group]), (6, "bin", "parquet_decimal_writer.py")).encode()
    out += meta + struct.pack("<I", len(meta)) + b"PAR1"
    with open(path, "wb") as f:
        f.write(out)


# ---- oracle ----------------------------------------------------------------------------------------------------------
def format_decimal(v, scale: int) -> str:
    """ClickHouse text output of a Decimal with this unscaled value (output_format_decimal_trailing_zeros = 0)."""
    if v is None:
        return "NULL"
    neg, a = v < 0, abs(v)
    whole, frac = divmod(a, 10 ** scale) if scale else (a, 0)
    s = str(whole)
    if scale:
        f = str(frac).rjust(scale, "0").rstrip("0")
        if f:
            s += "." + f
    return ("-" if neg and a else "") + s


# ---- the Decimal512 read matrix --------------------------------------------------------------------------------------
FLBA_WIDTHS = [1, 2, 7, 8, 9, 16, 31, 32, 33, 40, 48, 63, 64]
ENCODINGS = ["plain", "byte_stream_split", "delta_byte_array", "dict"]
SCALE = 10


def width_values(w: int):
    lo, hi = -(1 << (8 * w - 1)), (1 << (8 * w - 1)) - 1
    vals = [lo, hi, 0, 1, -1, lo + 1, hi - 1, (hi // 3) * (1 if w % 2 else -1)]
    return [v for v in vals]


def matrix(out_dir: str):
    os.makedirs(out_dir, exist_ok=True)
    listing = []
    for codec in ("none", "gzip"):
        for page_version in (1, 2):
            for optional in (False, True):
                name = f"d512_flba_{codec}_v{page_version}_{'nullable' if optional else 'required'}"
                cols = [{"name": "rn", "physical": "int32", "encoding": "plain", "values": list(range(10 if optional else 8))}]
                for w in FLBA_WIDTHS:
                    vals = width_values(w)
                    if optional:
                        vals = vals[:3] + [None] + vals[3:6] + [None] + vals[6:]
                    for enc in ENCODINGS:
                        cols.append({"name": f"w{w}_{enc}", "physical": "flba", "type_length": w, "precision": 154,
                                     "scale": SCALE, "encoding": enc, "optional": optional, "values": vals})
                write_file(os.path.join(out_dir, name + ".parquet"), cols, codec, page_version)
                with open(os.path.join(out_dir, name + ".expected.tsv"), "w") as f:
                    for c in cols[1:]:
                        f.write(c["name"] + "\t" + ",".join(format_decimal(v, SCALE) for v in c["values"]) + "\n")
                listing.append(name)
                # BYTE_ARRAY decimals: every byte length 0..64 once (length 0 reads as 0), plain and delta encodings
                name = f"d512_bytearray_{codec}_v{page_version}_{'nullable' if optional else 'required'}"
                sizes = list(range(0, 65))
                vals = [0 if s == 0 else ((-1) ** s) * ((1 << (8 * s - 1)) - 1 - s) for s in sizes]
                cols = [{"name": "rn", "physical": "int32", "encoding": "plain", "values": list(range(67 if optional else 65))}]
                for enc in ("plain", "delta_byte_array"):
                    v2, s2 = list(vals), list(sizes)
                    if optional:
                        v2 = v2[:10] + [None] + v2[10:40] + [None] + v2[40:]
                    col = {"name": f"ba_{enc}", "physical": "byte_array", "precision": 154, "scale": SCALE, "encoding": enc,
                           "optional": optional, "values": v2, "sizes": s2}
                    cols.append(col)
                write_file(os.path.join(out_dir, name + ".parquet"), cols, codec, page_version)
                with open(os.path.join(out_dir, name + ".expected.tsv"), "w") as f:
                    for c in cols[1:]:
                        f.write(c["name"] + "\t" + ",".join(format_decimal(v, SCALE) for v in c["values"]) + "\n")
                listing.append(name)
    with open(os.path.join(out_dir, "fixtures.txt"), "w") as f:
        f.write("\n".join(listing) + "\n")
    return listing


def selftest():
    assert varint(300) == b"\xac\x02" and zigzag(-1) == 1 and zigzag(1) == 2 and zigzag(-64) == 127
    assert be_twos(-1, 3) == b"\xff\xff\xff" and be_twos(1, 1) == b"\x01" and be_twos(-128, 1) == b"\x80"
    assert min_width(127) == 1 and min_width(128) == 2 and min_width(-129) == 2
    assert bitpack([0, 1, 2, 3, 4, 5, 6, 7], 3) == bytes([0x88, 0xC6, 0xFA])  # example of the Parquet spec
    assert rle_runs([1, 1, 1], 1) == b"\x06\x01"
    assert format_decimal(-5, 2) == "-0.05" and format_decimal(1234500, 2) == "12345" and format_decimal(10, 1) == "1"
    assert TStruct((1, "i32", 1), (4, "bin", "a")).encode() == b"\x15\x02\x38\x01a\x00"
    assert delta_binary_packed([7, 7, 7])[:4] == b"\x80\x01\x04\x03"
    print("selftest ok")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        selftest()
    elif len(sys.argv) == 3 and sys.argv[1] == "matrix":
        print("\n".join(matrix(sys.argv[2])))
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
