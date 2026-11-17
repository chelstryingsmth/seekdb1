# Local file SQL

File table functions infer a schema, then scan through seekdb's native function-table operator. Filtering, grouping, sorting and joins use the ordinary seekdb planner and executor. No external SQL engine is used. Input files are opened read-only; records and Parquet pages are bounded rather than loading the entire file.

## Enable and query

The session variable `enable_file_sql` defaults to `OFF`. A local administrative connection must configure `secure_file_priv` to a dedicated existing directory. On Windows use the server's named pipe (discovered in `run/sql.pipe`); TCP connections cannot change this existing security setting.

```sql
SET GLOBAL secure_file_priv = '/absolute/test-directory';
-- Reconnect to inherit the global setting.
SET SESSION enable_file_sql = ON;
SELECT * FROM read_csv('/absolute/test-directory/basic.csv', header=true, sample_size=-1);
SELECT * FROM read_json('/absolute/test-directory/basic.jsonl', format='newline_delimited');
SELECT * FROM read_ndjson('/absolute/test-directory/basic.jsonl');
SELECT count(*) FROM read_parquet('/absolute/test-directory/simple.parquet');
SELECT file FROM glob('/absolute/test-directory/*.csv');
DESCRIBE SELECT * FROM read_csv('/absolute/test-directory/basic.csv');
COPY (SELECT 1 AS id, 'example' AS name) TO '/absolute/test-directory/export.csv' (FORMAT CSV, HEADER);
```

CSV accepts `header`, `delimiter`/`delim`, `quote`, `escape`, `nullstr`/`null_string`, and `sample_size=-1`. CSV/JSONL inference scans the entire source without retaining its rows. Types are NULL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE and DATETIME. Duplicate CSV headers receive deterministic suffixes. JSONL requires flat objects; absent fields become NULL. COPY accepts CSV header, delimiter, quote, escape and NULL text options. Existing destinations are refused; failed exports remove their temporary output.

Paths must remain inside the configured root after canonicalization. Scans validate file identity, size, modification and change timestamps and reopen safely on rescan. Inferred schemas are not reused from the plan cache. Globs expand the basename only, with `*` and `?`; they discover files and do not merge them.

## Tests

Build using the repository's platform build instructions and bundled dependencies. On Windows the C++ toolchain is **Visual Studio 2022 Build Tools**, including the C++ desktop workload and Windows SDK; VS Code alone does not provide a compiler. The normal entry point is `build.ps1 init`, then `build.ps1 release --ninja`. Rust and the repository's pinned toolchains are also required by the normal build.

The portable reader checks need Python 3 and a C++20 compiler:

```text
python unittest/sql/engine/basic/run_file_sql_readers.py --compiler <g++-path>
```

The integration runner uses only Python's standard library. Start an isolated, empty-password test server with a dedicated base directory and unused port; never point it at a production instance. Example server options are `--nodaemon --base-dir <test-base> --port 28891 --parameter memory_budget=2G --parameter log_disk_size=2G --parameter datafile_size=128M`.

```text
python unittest/sql/engine/basic/run_file_sql_integration.py --port 28891 --pipe-file <test-base>/run/sql.pipe
```

The runner copies the organizer's fixtures into a temporary directory, sets and restores the global secure root, and checks both success and rejection cases. The organizer's Linux mysqltest entry point remains `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test`; its deployment include scripts are unchanged. Portable checks alone do not establish native SQL correctness.

## Limits

Records and Parquet pages are limited to 16 MiB, CSV/JSON schemas to 4096 columns, glob results to 100000 paths, and projected Parquet columns to 256. Parquet supports flat primitive columns with PLAIN, dictionary and RLE/bit-packed encoding, uncompressed or Snappy pages, data page versions 1/2 and multiple row groups. Nested Parquet, decimal, INT96 and other compression codecs/encodings are rejected. These limits are explicit failures, not silent data coercion. Optional Parquet features outside this subset still require additional implementation and validation.
