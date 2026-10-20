# Filesystem metadata

`cu_path_info` inspects a pathname with explicit link-following behavior.
`cu_fd_info` inspects an already-open file descriptor. Both return a small
`CuFileInfo` without allocating library storage or taking ownership.

## Result and data

```c
CuInfoResult cu_path_info(const char *path, CuPathFollow follow, CuFileInfo *out);
CuInfoResult cu_fd_info(int fd, CuFileInfo *out);
```

Compare the result explicitly. `CU_INFO_ERROR` is -1, so a boolean success test
is wrong.

| Result | Meaning |
|---|---|
| `CU_INFO_OK` | The complete observation is in `out` |
| `CU_INFO_MISSING` | Path lookup failed with ENOENT |
| `CU_INFO_ERROR` | An OS or size-representation error occurred |

Only the path operation returns MISSING. In particular, ENOTDIR, EACCES and
ELOOP are errors, not absence. An invalid descriptor reports ERROR/EBADF on a
supported backend. Non-OK results leave every output field unchanged and
preserve the native errno, including zero. MISSING retains ENOENT in errno.

`CuFileInfo.kind` uses the same `CuDirKind` values as directory entries:
`CU_DIR_FILE`, `CU_DIR_DIR`, `CU_DIR_LINK`, and `CU_DIR_OTHER`.

`size` is a `CuU64` containing the logical byte length of a regular file.
It is zero for directories, links and other objects. That zero does not mean
an empty directory or an empty link target. Size is not allocated disk space,
a promise about readable bytes, or a stable bound for subsequent I/O.

The conversion does not first narrow `off_t` to `size_t` or `unsigned long`.
Eight byte-sized steps preserve sizes above 4 GiB on a 32-bit machine when its
native file ABI can retrieve them. Negative regular sizes and values outside
64 bits produce EOVERFLOW, or ERANGE where EOVERFLOW is unavailable.
A native stat EOVERFLOW is still an error; a wider public record cannot fix a
narrow native ABI. This module does not change global file-offset build flags.

## Path lookup is not a safe-open operation

Choose `CU_PATH_FOLLOW` for stat-style behavior or `CU_PATH_NOFOLLOW` for
lstat-style behavior. There is no implicit default.

NOFOLLOW concerns the final component under native path-resolution rules.
Intermediate symbolic links can still be traversed, and a trailing slash can
require directory resolution through a link. A dangling final link can return
OK/LINK with NOFOLLOW and MISSING with FOLLOW.

A path can change immediately after inspection. Do not use this result as an
access check, sandbox boundary, symlink-free resolver, or proof that a later
open reaches the same object. Metadata operations can also block on an OS or
filesystem; there is no deadline guarantee.

## Inspect the descriptor you actually opened

A descriptor query refers to that open object even after its former pathname
is renamed, replaced or removed. It does not open, close, read, seek, flush or
change descriptor flags. Keep the descriptor open and protected against
concurrent close/reuse during the call. File contents and metadata can still
change through other handles; this API is not a lock or coherent transaction
across every filesystem field.

The concrete cgrep use is:

```text
open(path, O_RDONLY | O_NONBLOCK)
    -> cu_fd_info(the_same_fd)
    -> reject nonregular inputs for parallel scanning
    -> fdopen(the_same_fd)
    -> stream the file
```

Replacing the FD query with a path query would reintroduce a race. Directory
walking and ignore policy stay in cgrep. A private adapter and deterministic
pathname-replacement test validate this use; adoption awaits a published
library revision and consumer dependency-pin update.

## Platforms and arguments

The backend uses POSIX stat/lstat/fstat on supported Unix, Apple and WASI
builds. Cerberus and unsupported targets return ERROR/ENOSYS, or EINVAL where
ENOSYS is unavailable. This takes precedence over descriptor validity when no
backend exists. Runtime support claims remain limited to platforms actually
checked; no native Windows metadata backend is added here.

Required pointers must be valid, strings NUL-terminated and output writable.
NULL required pointers or an unknown follow mode abort. A negative descriptor
is an ordinary OS error, not a programmer abort. No pointer is retained.

## Complete C89 example

This program inspects the final path component without following it and prints
its kind plus a 16-digit hexadecimal size. Nonregular kinds print zero size.
It does not open or read the named object.

```c
#include <stdio.h>
#include "cu.h"

static const char *kind_name(CuDirKind kind)
{
    switch (kind) {
    case CU_DIR_FILE: return "file";
    case CU_DIR_DIR: return "directory";
    case CU_DIR_LINK: return "link";
    default: return "other";
    }
}

int main(int argc, char **argv)
{
    CuFileInfo info;
    CuInfoResult result;

    if (argc != 2)
        return 2;
    result = cu_path_info(argv[1], CU_PATH_NOFOLLOW, &info);
    if (result == CU_INFO_MISSING) {
        if (fputs("missing\n", stderr) == EOF)
            return 2;
        return 1;
    }
    if (result == CU_INFO_ERROR) {
        perror("metadata");
        return 2;
    }
    if (printf("%s %08lx%08lx\n", kind_name(info.kind), info.size.hi, info.size.lo) < 0 ||
        fflush(stdout) != 0)
        return 2;
    return 0;
}
```

Compile with the repository's C89 integration flags and `src/*.c`.

## Scope of checks

Tests use literal binary files, empty files, directories, links, native FIFOs
and pipes, invalid descriptors, and a sparse file above 4 GiB. They compare
an open file with a replacement pathname and verify unchanged offset/flags
and continued readability. A boundary fixture checks syscall selection,
exact errors, unmodified outputs, signed-size rejection, byte boundaries and
the largest representable signed 64-bit offset. The conversion's wider-than-
64-bit rejection branch is not exercised by an ordinary 64-bit off_t host.

No permissions, timestamps, inode identities, directory traversal, lexical
path operations or sandbox guarantees are part of this slice. The verification
receipt records actual gate results and platform limits; no finite test suite
certifies filesystem security or the absence of concurrent changes.
