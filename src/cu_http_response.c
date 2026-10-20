#include "cu.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int response_token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int response_name(CuStr field, const char *name)
{
    size_t i;

    if (field.len != strlen(name))
        return 0;
    for (i = 0; i < field.len; i++) {
        if (cu_ascii_to_lower(field.ptr[i]) != name[i])
            return 0;
    }
    return 1;
}

static int response_owned(CuStr name)
{
    return response_name(name, "content-length") || response_name(name, "transfer-encoding") ||
           response_name(name, "connection") || response_name(name, "trailer") ||
           response_name(name, "upgrade") || response_name(name, "te") ||
           response_name(name, "keep-alive") || response_name(name, "proxy-connection");
}

static const char *response_reason(unsigned status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

static int response_field(const CuHttpHeader *field)
{
    size_t i;
    unsigned char c;

    if ((field->name.ptr == NULL && field->name.len != 0) ||
        (field->value.ptr == NULL && field->value.len != 0))
        abort();
    if (field->name.len == 0)
        return 0;
    for (i = 0; i < field->name.len; i++) {
        if (!response_token((unsigned char)field->name.ptr[i]))
            return 0;
    }
    if (response_owned(field->name))
        return 0;
    if (field->value.len != 0) {
        c = (unsigned char)field->value.ptr[0];
        if (c == ' ' || c == '\t')
            return 0;
        c = (unsigned char)field->value.ptr[field->value.len - 1];
        if (c == ' ' || c == '\t')
            return 0;
    }
    for (i = 0; i < field->value.len; i++) {
        c = (unsigned char)field->value.ptr[i];
        if ((c < 32 && c != '\t') || c >= 127)
            return 0;
    }
    return 1;
}

static void response_copy(char *out, size_t *pos, const char *text, size_t n)
{
    if (n != 0)
        memcpy(out + *pos, text, n);
    *pos += n;
}

static CuHttpResponseResult response_head(char *head, size_t cap,
                                          unsigned status, size_t body_length,
                                          const CuHttpHeader *headers, size_t count,
                                          size_t *written)
{
    char digits[sizeof(size_t) * CHAR_BIT + 1];
    size_t digit_count;
    size_t value;
    size_t needed;
    size_t pos;
    size_t i;
    const char *reason;
    int has_length;

    if (written == NULL || (head == NULL && cap != 0) || (headers == NULL && count != 0))
        abort();
    *written = 0;
    if (status < 200 || status > 599)
        return CU_HTTP_RESPONSE_INVALID;
    if ((status == 204 || status == 205 || status == 304) && body_length != 0)
        return CU_HTTP_RESPONSE_INVALID;
    reason = response_reason(status);
    has_length = status != 204 && status != 304;
    digit_count = 0;
    value = body_length;
    do {
        digits[digit_count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    needed = sizeof("HTTP/1.1 ") - 1 + 3 + 1 + strlen(reason) + 2;
    if (!cu_size_add(needed, sizeof("Connection: close\r\n") - 1, &needed))
        return CU_HTTP_RESPONSE_LIMIT;
    if (has_length && (!cu_size_add(needed, sizeof("Content-Length: ") - 1, &needed) ||
                       !cu_size_add(needed, digit_count, &needed) ||
                       !cu_size_add(needed, 2, &needed)))
        return CU_HTTP_RESPONSE_LIMIT;
    for (i = 0; i < count; i++) {
        if (!response_field(&headers[i]))
            return CU_HTTP_RESPONSE_INVALID;
        if (!cu_size_add(needed, headers[i].name.len, &needed) ||
            !cu_size_add(needed, 2, &needed) ||
            !cu_size_add(needed, headers[i].value.len, &needed) ||
            !cu_size_add(needed, 2, &needed))
            return CU_HTTP_RESPONSE_LIMIT;
    }
    if (!cu_size_add(needed, 2, &needed) || needed > cap)
        return CU_HTTP_RESPONSE_LIMIT;
    /* Validation and all checked sizing precede the first output byte. */
    pos = 0;
    response_copy(head, &pos, "HTTP/1.1 ", sizeof("HTTP/1.1 ") - 1);
    head[pos++] = (char)('0' + status / 100);
    head[pos++] = (char)('0' + status / 10 % 10);
    head[pos++] = (char)('0' + status % 10);
    head[pos++] = ' ';
    response_copy(head, &pos, reason, strlen(reason));
    response_copy(head, &pos, "\r\nConnection: close\r\n", sizeof("\r\nConnection: close\r\n") - 1);
    if (has_length) {
        response_copy(head, &pos, "Content-Length: ", sizeof("Content-Length: ") - 1);
        while (digit_count != 0)
            head[pos++] = digits[--digit_count];
        response_copy(head, &pos, "\r\n", 2);
    }
    for (i = 0; i < count; i++) {
        response_copy(head, &pos, headers[i].name.ptr, headers[i].name.len);
        response_copy(head, &pos, ": ", 2);
        response_copy(head, &pos, headers[i].value.ptr, headers[i].value.len);
        response_copy(head, &pos, "\r\n", 2);
    }
    response_copy(head, &pos, "\r\n", 2);
    *written = pos;
    return CU_HTTP_RESPONSE_OK;
}

CuHttpResponseResult cu_http_response_head(char *head, size_t cap,
                                           unsigned status, size_t body_length,
                                           const CuHttpHeader *headers, size_t count,
                                           size_t *written)
{
    CuHttpResponseResult result;
    int saved;

    saved = errno;
    result = response_head(head, cap, status, body_length, headers, count, written);
    errno = saved;
    return result;
}
