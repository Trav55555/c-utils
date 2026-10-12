#include <errno.h>
#include <stdlib.h>
#include "cu.h"

int cu_input_open(CuInput *in, const char *name)
{
    if (in == NULL || name == NULL)
        abort();
    in->owns = 0;
    in->state = CU_INPUT_ERROR;
    errno = 0;
    in->stream = fopen(name, "rb");
    in->error = errno;
    if (in->stream == NULL)
        return 0;
    in->owns = 1;
    in->state = CU_INPUT_DATA;
    in->error = 0;
    return 1;
}

void cu_input_attach(CuInput *in, FILE *stream)
{
    if (in == NULL || stream == NULL)
        abort();
    in->stream = stream;
    in->owns = 0;
    in->state = CU_INPUT_DATA;
    in->error = 0;
}

CuInputStatus cu_input_read(CuInput *in, void *buf, size_t cap, size_t *n)
{
    size_t got;
    int saved_errno;

    if (in == NULL || buf == NULL || cap == 0 || n == NULL)
        abort();
    *n = 0;
    if (in->state == CU_INPUT_ERROR) {
        errno = in->error;
        return CU_INPUT_ERROR;
    }
    if (in->stream == NULL)
        abort();
    if (in->state == CU_INPUT_END)
        return CU_INPUT_END;
    saved_errno = errno;
    if (ferror(in->stream)) {
        in->state = CU_INPUT_ERROR;
        in->error = saved_errno;
        errno = saved_errno;
        return CU_INPUT_ERROR;
    }
    if (feof(in->stream)) {
        in->state = CU_INPUT_END;
        return CU_INPUT_END;
    }
    errno = 0;
    got = fread(buf, 1, cap, in->stream);
    saved_errno = errno;
    if (ferror(in->stream)) {
        in->state = CU_INPUT_ERROR;
        in->error = saved_errno;
        errno = saved_errno;
        return CU_INPUT_ERROR;
    }
    if (got == 0 || feof(in->stream))
        in->state = CU_INPUT_END;
    if (got == 0)
        return CU_INPUT_END;
    *n = got;
    return CU_INPUT_DATA;
}

int cu_input_close(CuInput *in)
{
    if (in == NULL)
        abort();
    if (in->owns && in->stream != NULL) {
        errno = 0;
        if (fclose(in->stream) != 0 && in->state != CU_INPUT_ERROR) {
            in->state = CU_INPUT_ERROR;
            in->error = errno;
        }
    }
    in->stream = NULL;
    in->owns = 0;
    if (in->state == CU_INPUT_ERROR) {
        errno = in->error;
        return 0;
    }
    in->state = CU_INPUT_END;
    return 1;
}

int cu_input_errno(const CuInput *in)
{
    if (in == NULL)
        abort();
    return in->error;
}
