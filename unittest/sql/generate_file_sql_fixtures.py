#!/usr/bin/env python3
"""Generate independent Lab2 input files; pyarrow is a test-only dependency."""
import argparse
from datetime import date, datetime, timedelta
import json
from pathlib import Path
import struct

import pyarrow as pa
import pyarrow.parquet as pq


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    out = args.directory.resolve()
    out.mkdir(parents=True, exist_ok=True)
    orders = pa.table({
        "order_id": pa.array(range(101, 107), type=pa.int64()),
        "person_id": pa.array([1, 1, 2, 4, 4, 6], type=pa.int32()),
        "total": pa.array([12.5, 20., 8.25, 50., None, 59.99], type=pa.float64()),
        "channel": pa.array(["web", "shop", None, "web", "shop", "web"]),
        "paid": pa.array([True, False, True, False, None, True]),
        "order_date": pa.array([date(2026, 8, 1) + timedelta(days=i) for i in range(6)]),
        "stamp": pa.array([datetime(2026, 8, 1, 10, 11, 12, 123456) + timedelta(days=i)
                           for i in range(6)], type=pa.timestamp("us")),
        "ratio": pa.array([1.25, None, 2.5, 3.75, 4.5, 5.25], type=pa.float32()),
    })
    for name, page_version, codec, dictionary in [
        ("orders", "1.0", "snappy", True),
        ("orders_v2", "2.0", "snappy", True),
        ("orders_plain", "1.0", "NONE", False),
        ("orders_zstd", "1.0", "zstd", True),
    ]:
        pq.write_table(orders, out / (name + ".parquet"), row_group_size=2,
                       data_page_version=page_version, compression=codec,
                       use_dictionary=dictionary, write_page_checksum=True)
    pq.write_table(pa.table({"stamp": orders["stamp"]}), out / "int96.parquet",
                   use_deprecated_int96_timestamps=True)
    pq.write_table(pa.table({"nested": [[1, 2], None]}), out / "nested.parquet")
    with pq.ParquetWriter(out / "empty.parquet", pa.schema([pa.field("id", pa.int64())])):
        pass
    pq.write_table(pa.table({"stamp": pa.array([datetime(1970, 1, 1, 0, 0, 1, 234000),
                                               datetime(1969, 12, 31, 23, 59, 59, 999000)],
                                              type=pa.timestamp("ms"))}),
                   out / "millis.parquet", version="1.0", compression="NONE")
    values = ["value_" + str(i).zfill(5) + "x" * 1100 for i in range(1, 5001)]
    big = pa.table({"id": pa.array(range(1, 5001), type=pa.int64()), "value": values})
    pq.write_table(big, out / "big.parquet", row_group_size=311,
                   data_page_version="2.0", compression="snappy", use_dictionary=False,
                   data_page_size=4096, write_batch_size=31)

    # Isolate one large value per one-value page, staggered by row, so only
    # one projected page is logically large at a time while vectors retain
    # the capacities of earlier columns' large pages.
    retained_rows = 22
    retained_columns = 21
    retained_value_bytes = 13 * 1024 * 1024
    retained_names = [f"col_{i:02d}" for i in range(retained_columns)]
    retained_arrays = []
    retained_small_values = []
    for column in range(retained_columns):
        retained_values = [f"s{row:02d}_c{column:02d}" for row in range(retained_rows)]
        retained_small_values.extend(retained_values)
        prefix = f"large_{column:02d}_"
        retained_values[column] = prefix + ("x" * (retained_value_bytes - len(prefix)))
        retained_arrays.append(pa.array(retained_values, type=pa.string()))
        del retained_values
    retained_schema = pa.schema([pa.field(name, pa.string(), nullable=False)
                                 for name in retained_names])
    retained_table = pa.Table.from_arrays(retained_arrays, schema=retained_schema)
    retained_path = out / "parquet_retained_capacity.parquet"
    pq.write_table(retained_table, retained_path, row_group_size=retained_rows,
                   version="1.0", data_page_version="1.0", compression="NONE",
                   use_dictionary=False, data_page_size=64 * 1024, write_batch_size=1)
    retained_metadata = pq.ParquetFile(retained_path).metadata
    retained_chunk_bytes = [retained_metadata.row_group(0).column(i).total_uncompressed_size
                            for i in range(retained_columns)]
    if (retained_metadata.num_row_groups != 1 or retained_metadata.num_rows != retained_rows
            or any(size > 16 * 1024 * 1024 for size in retained_chunk_bytes)):
        raise RuntimeError("retained-capacity Parquet fixture exceeded its intended bounds")
    retained_page_bound = (retained_value_bytes + 4
                           + (retained_columns - 1) * (max(map(len, retained_small_values)) + 4))
    del retained_table, retained_arrays, retained_small_values
    (out / "big.csv").write_text("id,value\n" + "".join(
        f"{i},{value}\n" for i, value in enumerate(values, 1)))
    (out / "big.jsonl").write_text("".join(json.dumps({"id": i, "value": value}) + "\n"
                                             for i, value in enumerate(values, 1)))
    (out / "people.csv").write_text("id,name\n1,Alice\n2,Bob\n3,Cy\n4,Di\n5,Ed\n6,Fay\n")
    (out / "cross.jsonl").write_text('{"id":1,"score":10}\n{"id":3,"score":30}\n')

    # Keep metadata intact and damage only the second column's data page.
    pair = pa.Table.from_arrays([pa.array([1, 2, 3], type=pa.int32()),
                                pa.array([10, 20, 30], type=pa.int32())],
                               schema=pa.schema([pa.field("good", pa.int32(), nullable=False),
                                                 pa.field("bad", pa.int32(), nullable=False)]))
    pair_path = out / "pruning.parquet"
    pq.write_table(pair, pair_path, compression="NONE", use_dictionary=False)
    page_offset = pq.ParquetFile(pair_path).metadata.row_group(0).column(1).data_page_offset
    data = bytearray(pair_path.read_bytes())
    data[page_offset:page_offset + 16] = b"\xff" * 16
    pair_path.write_bytes(data)
    data = bytearray((out / "orders.parquet").read_bytes())
    (out / "truncated.parquet").write_bytes(data[:-11])
    data[-8:-4] = struct.pack("<I", 0xffffffff)
    (out / "footer_overflow.parquet").write_bytes(data)
    (out / "fixture-generation.json").write_text(json.dumps({
        "pyarrow": pa.__version__, "orders_rows": 6, "orders_row_groups": 3,
        "big_rows": 5000, "damaged_column": 1, "damaged_page_offset": page_offset,
        "retained_capacity": {
            "file": retained_path.name, "rows": retained_rows, "row_groups": 1,
            "columns": retained_columns, "large_value_bytes": retained_value_bytes,
            "large_value_row_by_column": list(range(retained_columns)),
            "dictionary": False, "compression": "NONE", "values_per_data_page": 1,
            "max_current_page_payload_bytes": retained_page_bound,
            "max_column_chunk_uncompressed_bytes": max(retained_chunk_bytes),
        },
    }, indent=2) + "\n")
    print("Fixtures:", out)


if __name__ == "__main__":
    main()
