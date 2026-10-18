# c-utils

Strings, containers, file I/O, and threads for C89, with explicit memory
ownership and checked bounds. Allocate related data in one arena, use scopes
for temporary work, and release it together instead of freeing each object.

[![Native tests](https://github.com/Trav55555/c-utils/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/Trav55555/c-utils/actions/workflows/test.yml)

The library follows the [Dependable C](https://dependablec.org/) subset,
but you do not need its checker to compile or use it. There is one public
header, [`include/cu.h`](include/cu.h), plus C source files. This is **not a
header-only library**.

[Quick start](#quick-start) · [Integration](#integration) ·
[API guide](#api-guide) · [Ownership and errors](#ownership-and-errors) ·
[Tests and platforms](#tests-and-platforms)

## Quick start

Clone the repository:

```sh
git clone https://github.com/Trav55555/c-utils.git
cd c-utils
```

Save this as `hello.c` in the repository root. It builds a string in an
arena and writes exactly its length, without assuming a terminating NUL.

```c
#include "cu.h"

int main(void)
{
	CuArena arena;
	CuBuilder builder;
	CuStr text;
	int result;

	cu_arena_init(&arena);
	cu_sb_init(&builder, &arena);
	cu_sb_str(&builder, CU_LIT("items="));
	cu_sb_ulong(&builder, 3UL);
	cu_sb_char(&builder, '\n');
	result = 1;
	if (!cu_sb_finish(&builder, &text))
		goto done;
	if (fwrite(text.ptr, 1, text.len, stdout) != text.len)
		goto done;
	if (fflush(stdout) != 0)
		goto done;
	result = 0;
done:
	cu_arena_release(&arena);
	return result;
}
```

Compile on a POSIX host with GCC or Clang, then run. `cc` is your C
compiler; use `gcc` or `clang` explicitly if you prefer.

```sh
cc -std=c89 -pedantic-errors -Wall -Wextra -Werror -Iinclude hello.c src/*.c -pthread -o hello
./hello
```

Expected output:

```text
items=3
```

`-pthread` enables the POSIX thread build and link settings. Older glibc
requires it to link pthreads; glibc 2.34+ and musl include pthreads in libc.

## Integration

Vendor `include/cu.h`, `src/*.c`, and [LICENSE](LICENSE) into your project.
Add the header directory to your include path and compile the source files
alongside your application, as above. You do not need generated code or a
checker. Use the same compiler flags for the library and your application.

For optimized builds, use `-O2` and, where supported, `-flto` to allow
inlining across translation units. There is no unity-build file.
If your build force-includes system headers, define `_POSIX_C_SOURCE=200809L`,
`_DEFAULT_SOURCE`, and `_GNU_SOURCE` on the compiler command line. These expose
the required platform declarations, including atomic Linux descriptor creation.

## API guide

The [public header](include/cu.h) is the canonical API reference, including
return values, scope rules, and pointer validity. These are the main groups:

| Need | API |
|---|---|
| Allocate and reclaim together | `CuArena`, `CuTemp`, per-thread scratch arenas |
| Check allocation sizes | `cu_size_add`, `cu_size_mul` |
| View, slice, search, and split bytes | `CuStr`, `cu_str_*` |
| Parse whole integers, classify ASCII | `cu_parse_long`, `cu_parse_ulong`, `cu_ascii_*` |
| Build text without format strings | `CuBuilder`, `cu_sb_*` |
| Hold and check 64-bit unsigned values in C89 | `CuU64`, `cu_u64_add`, `cu_u64_sub`, `cu_u64_cmp` |
| Read and write binary integers | Unsigned 8-, 16-, 32- and 64-bit readers and builder appends; explicit byte order |
| Hash content for addressing (not for secrets) | `CuSha256`, `cu_sha256` |
| Grow an array or view its elements | `CuVec`, `CuSlice` |
| Map strings to values | `CuMap`; copied keys, stable entries, insertion-order iteration, no removal |
| Remove objects without following stale handles | `CuPool`; one value type, generational handles |
| Link nodes of several kinds, including cycles | `CuPlex`; kind-checked generational links |
| Read files or streams | `cu_file_read`, `cu_stream_read`, chunked `CuInput` |
| List one directory's entries | `CuDir` |
| Write files durably: append, sync, truncate, lock; rename, remove, make and sync directories | `CuAppend`, `cu_path_rename`, `cu_dir_sync` |
| Start and join threads, synchronize | `CuThread`, `CuMutex`, `CuCond` |
| Run bounded task batches with cooperative cancellation and one deadline | `CuTaskSpec`, `cu_tasks_run`; [ownership and example](docs/TASKS.md) |
| Listen, accept, connect, read and write TCP with time limits; watch many sockets | `CuSocket`, `CuPoller` |
| Borrow memory or a socket under one read budget | `CuReader`, `cu_socket_read_for` |
| Read bounded HTTP request heads and exact-length bodies | `CuHttpRequest`; [supported profile and complete example](docs/HTTP.md) |
| Parse strict JSON into a tree, look up keys, read integers; write JSON with escaping | `cu_json_parse`, `cu_json_find`, `CuJsonOut` |
| Encode and decode canonical base64 | `cu_sb_base64`, `cu_base64_decode` |

### Push values, then check the vector

Save this second complete program as `values.c`. Use the quick-start
compile command, replacing `hello.c` with `values.c` and `-o hello`
with `-o values`. Run `./values` to print `42` and a newline.

```c
#include "cu.h"

int main(void)
{
	CuArena arena;
	CuVec values;
	int value;
	int result;

	cu_arena_init(&arena);
	CU_VEC_INIT(&values, &arena, int);
	value = 42;
	CU_VEC_PUSH(&values, &value);
	result = 1;
	if (cu_vec_failed(&values))
		goto done;
	if (printf("%d\n", *CU_VEC_AT(&values, int, 0)) < 0)
		goto done;
	if (fflush(stdout) != 0)
		goto done;
	result = 0;
done:
	cu_arena_release(&arena);
	return result;
}
```

`CU_VEC_PUSH` copies the pointed-to value and returns `void`. Check
`cu_vec_failed` after a sequence of pushes. A push or reserve can invalidate
all pointers into the vector, including slices of its elements.

## Ownership and errors

Arena allocations are uninitialized. Initialize every field before use.
There is no per-object free. Memory lasts until its temp scope ends or its
arena is released. End temp scopes exactly once, in reverse order.

For scratch work, pass every result arena as a conflict to
`cu_scratch_begin`, then pair it with `cu_scratch_end`. Release a thread's
scratch arenas with `cu_scratch_release` before exit; threads started by
`cu_thread_start` do this automatically.

Containers remember the scope in which you initialize them. Mutate them
only while that same scope is innermost, even if the operation would not
allocate. Reading under a newer scope is allowed while the owning scope
remains open. Initialize temporary containers inside their temporary scope.
Do not reinitialize an arena with live users; stale users of a reinitialized
arena are not reliably detected.

`CuStr` is a read-only view of `len` bytes, **not a NUL-terminated string**.
Use `fwrite` as above, or check the result of `cu_str_cstr_copy` when a C API
needs a terminated copy. String slices borrow their original storage.
Views returned by `cu_sb_finish` remain valid through later appends, until
the builder's scope ends or its arena is released. Raw builder data pointers
do not have that guarantee.

Resource and input failures are returned as `NULL`, an unsuccessful flag,
or an API-specific status. Arena and container allocation failures are
sticky. Builders report accumulated append failures through `cu_sb_finish`.
Check the relevant API's result or failure flag; not every fallible call
has a compiler-enforced unused-result warning. Contract violations such as
out-of-bounds access, wrong-scope mutation, and out-of-order scope cleanup
call `abort()` rather than returning an error.

Whole-file reads close their file; `cu_stream_read` leaves the supplied
stream open. `CuInput` owns a file opened with `cu_input_open` or borrows a
stream attached with `cu_input_attach`. Check `cu_input_close` for both prior
errors and close failures. `cu_input_errno` gives the first saved errno,
which can be zero even on failure. Borrowed streams remain open; use them
exclusively through `CuInput` while attached. `cu_dir_next` copies entry
names into its arena's active scope.

Arena cleanup does not close OS resources. Close owned inputs and directory
handles explicitly. Join threads and destroy mutexes and conditions **before**
releasing the arenas or ending the scopes that hold their records. There is
no thread detach. Mutex ownership and synchronization remain your responsibility.

### Limits worth keeping in mind

- Raw pointers still require lifetime discipline. Arenas and generational
  handles do not prevent every use-after-free.
- Arenas and containers are not implicitly thread-safe. Use separate worker
  arenas or synchronize shared access.
- Typed container macros check element sizes, not full C types. Different
  types of the same size are not distinguished. Plex links also check kind.
- `CuMap` uses unseeded 32-bit FNV-1a. Deliberate collisions can make a lookup
  linear in the number of colliding keys. Bound untrusted key counts and memory.
- `*_FAST` access macros can evaluate arguments more than once. Do not pass
  expressions with side effects.
- `cu_arena_set_limit` caps held plus cached block bytes, including headers,
  not just payload bytes. Check allocation failure when applying a limit.

Define `CU_ARENA_DEBUG`, build with AddressSanitizer, or initialize an arena
with debug mode enabled to give each push its own allocation. This exposes
object boundaries and ended scopes to memory tools. `cu_arena_fail_at` and
`cu_thread_fail_at` inject failures for tests. Growth, caching, and individual
operation contracts are documented in [cu.h](include/cu.h).

## Tests and platforms

The core targets C89, not a C++ compatibility guarantee. Directory listing
uses POSIX APIs; threads use pthreads. There is no Windows directory or
thread backend. WASI and Cerberus have no thread backend, and Cerberus has
no directory backend. Unsupported initialization returns failure with
`ENOSYS` where available; errno can be zero on other platforms.
See [thread design and restrictions](docs/THREADS.md).

Run the ordinary tests with a C compiler and Make. No `dcheck` is required:

```sh
make test
```

On older glibc, pass the pthread flag through `CFLAGS`:

```sh
make test CC=gcc CFLAGS='-g -pthread'
```

The optional maintainer checks use a separate, private `dcheck` toolchain.
These checks require toolchain access
and the external tools configured for each check; they are not available
from a public library checkout alone. With those prerequisites already present,
set `DCHECK` to your local executable:

```sh
make check DCHECK=/path/to/dcheck
make deep DCHECK=/path/to/dcheck
```

`check` runs the configured subset and lint checks, compiler matrix,
analyzers, sanitizers, optimized build, WASI checks, and coverage gate.
`deep` adds Cerberus, Valgrind, fuzzing, and CBMC proofs. These are test
configurations, not a promise that every platform or input has been proved.
The string proofs cover inputs up to four bytes; the size-add proof covers
all inputs. Size multiplication is fuzzed against compiler overflow checks,
not proved for all 64-bit inputs.

## Further reading

- [Public API and contracts](include/cu.h)
- [Roadmap](ROADMAP.md)
- [Performance review and measurements](docs/PERFORMANCE-REVIEW-20261014.md)
- [Thread design](docs/THREADS.md)
- [cgrep](https://github.com/Trav55555/cgrep), a consumer of the streaming input API
- [MIT license](LICENSE)
