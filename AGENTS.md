# Repository map

c-utils is a C89 library with explicit memory ownership, checked bounds,
arena allocation, and platform I/O. It is not header-only. Start with
`README.md` for usage and `include/cu.h` for public declarations and contracts.

## Layout

- `include/cu.h`: the single public library header, including typed macros,
  ownership annotations, return values, and lifetime rules.
- `src/cu_*.c`: library implementations; the API-to-source map is below.
- `tests/test_*.c`: native C tests. `tests/death.h` and `tests/align.h` are
  shared helpers; `tests/data/` holds fixtures. Python tests and scripts in
  `tests/` cover verification infrastructure and platform checks.
- `fuzz/`: fuzz harnesses and seed inputs for individual API families.
- `proofs/`: bounded-model-checking harnesses, including arithmetic,
  containers, strings, bytes, arena growth, and HTTP bodies.
- `docs/`: detailed platform/API guides and performance review.
- `examples/curun/`: bounded subprocess CLI consumer with its own header,
  source, Makefile, tests, fuzz harness, and README.
- `examples/cuhealth/`: finite local HTTP health/status consumer with the
  same general layout. Neither example adds public library API.
- `Makefile`: native tests, example targets, and optional maintainer checks.
- `dcheck.conf`: private checker configuration. `tools/verify_deep.py`
  bounds checker execution; `ci/` contains CI scripts and dependency metadata.
- `ROADMAP.md`: planned work, not the current API contract.

## Public API implementations

All families below are declared in `include/cu.h`. Source paths are relative
to `src/`; consult the header before changing behavior.

| Family | Implementation |
|---|---|
| Checked size arithmetic (`cu_size_*`), arenas, temp scopes, scratch arenas | `cu_arena.c` |
| C89 64-bit unsigned values (`CuU64`, `cu_u64*`) | `cu_u64.c` |
| String views, slicing, search, copies, ASCII classification | `cu_str.c` |
| Integer parsing (`cu_parse_*`), text builder (`CuBuilder`, core `cu_sb_*`) | `cu_fmt.c` |
| Binary integer readers and builder appends (`cu_rd_*`, `cu_sb_u*`) | `cu_bytes.c` |
| SHA-256 and SHA-1 | `cu_sha256.c`, `cu_sha1.c` |
| Vectors and borrowed element slices (`CuVec`, `CuSlice`) | `cu_vec.c` |
| String-key maps, iteration, FNV-1a hash | `cu_map.c` |
| Generational pools and handles (`CuPool`, `CuHandle`) | `cu_pool.c` |
| Kind-checked node links (`CuPlex`, `CuLink`) | `cu_plex.c` |
| JSON parsing, lookup, integer conversion, output (`CuJson`, `CuJsonOut`) | `cu_json.c` |
| Base64 encoding and decoding | `cu_base64.c` |
| zlib/gzip inflation, stored-block zlib output, Adler-32, CRC-32 | `cu_zlib.c` |
| Whole-file and stream reads (`cu_file_read`, `cu_stream_read`) | `cu_file.c` |
| Chunked blocking input (`CuInput`) | `cu_input.c` |
| Directory listing (`CuDir`) | `cu_dir.c` |
| Path, descriptor, and directory-relative metadata (`CuFileInfo`) | `cu_metadata.c` |
| File append/sync/truncate/lock, path rename/remove, directory make/sync | `cu_append.c` |
| Threads, mutexes, conditions | `cu_thread.c` |
| Bounded cooperative task batches (`CuTaskSpec`, `cu_tasks_run`) | `cu_task.c` |
| Subprocess start, redirected stdio, wait, signal | `cu_process.c` |
| Bounded child pipe communication (`CuProcessIO`, `CuProcessCapture`) | `cu_process_io.c` |
| TCP sockets, timed numeric-IP connects, poller (`CuPoller`) | `cu_socket.c` |
| Borrowed memory/socket readers (`CuReader`) | `cu_reader.c` |
| HTTP request heads and exact-length bodies (`CuHttpRequest`) | `cu_http.c` |
| Bounded HTTP/1.1 response heads | `cu_http_response.c` |

Related guides are `docs/THREADS.md`, `docs/TASKS.md`, `docs/PROCESSES.md`,
`docs/PROCESS_IO.md`, `docs/CONNECT.md`, `docs/METADATA.md`, `docs/HTTP.md`,
and `docs/HTTP_RESPONSES.md`.

## Build and test

From the repository root, run:

```sh
make test
```

This builds each `tests/test_*.c` with all `src/*.c` and runs the resulting
executables in `build/`. It needs Make and a C compiler, not `dcheck`.
`make` alone selects the private `check` target, so use `make test` explicitly.
The Makefile always applies these strict language and warning flags:

```text
-std=c89 -pedantic-errors -Wall -Wextra -Werror
```

`CFLAGS` adds flags (default `-g`); `CC` selects the compiler and `BUILD`
selects the output directory. On older glibc, use the documented thread flags:

```sh
make test CC=gcc CFLAGS='-g -pthread'
```

For an application, compile its source alongside `src/*.c` with `-Iinclude`,
the strict flags above, and `-pthread` on POSIX, as in the README quick start.
There is no generated code or unity-build file.

`make curun` and `make cuhealth` build the example executables in `build/`;
`make test-curun` and `make test-cuhealth` run their checks.
`make test-wasi-strict` requires the tools and Linux resource-control setup
listed in the README. `make check` and `make deep` require the separate private
`dcheck` toolchain; they are not prerequisites for ordinary native tests.

## Contracts to preserve

`CuStr` borrows bytes and is not NUL-terminated. Arena allocations are
uninitialized; temp scopes end exactly once in reverse order. Containers
must be mutated in their owning scope. Allocation failures are sticky;
check each API's result or failure flag. Contract violations abort.
Arena cleanup does not close OS resources: close handles, join threads, and
destroy synchronization objects before releasing their storage. Platform
support and unsupported-operation behavior are documented in the header and
README. Match each source file's existing style when editing it.
