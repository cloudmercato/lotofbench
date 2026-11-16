/* Upload flow for the LotofBench ingest portal.
 *
 * The pure helpers (JSON field extraction, payload assembly, response
 * parsing) compile on every build and are unit-tested on buffers; the
 * HTTPS flow lives behind HAVE_CURL. On the zero-dependency build the
 * flow degrades to an UPLOAD_UNAVAILABLE stub so the CLI stays fully
 * offline while the rest of the program is untouched. */

#include "upload.h"

#include "pow.h"
#include "../lotofbench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- tiny JSON field extraction ------------------------------------*/

static const char *find_key(const char *body, const char *key)
{
    char pat[64];
    const char *p = body;

    if (snprintf(pat, sizeof pat, "\"%s\"", key) >= (int)sizeof pat)
        return NULL;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == ':')
            return p + 1;
        /* a longer key containing this one as a suffix — keep looking */
    }
    return NULL;
}

int upload_json_find_string(const char *body, const char *key, char *out,
                            size_t cap)
{
    const char *p;
    size_t n = 0;

    if (!body || !key || !out || cap == 0)
        return -1;
    p = find_key(body, key);
    if (!p)
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"') {
        char c = *p;

        if (c == '\\' && p[1]) {
            p++;
            c = *p;
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
            /* \" \\ and the rest pass through as themselves */
        }
        if (n + 1 < cap)
            out[n++] = c;
        p++;
    }
    if (*p != '"')
        return -1;
    out[n] = '\0';
    return 0;
}

int upload_json_find_uint(const char *body, const char *key,
                          unsigned long long *out)
{
    const char *p;
    unsigned long long acc = 0;
    int digits = 0;

    if (!body || !key || !out)
        return -1;
    p = find_key(body, key);
    if (!p)
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    for (; *p >= '0' && *p <= '9'; p++, digits++) {
        if (acc > (~0ULL - (unsigned long long)(*p - '0')) / 10)
            return -1; /* overflow */
        acc = acc * 10 + (unsigned long long)(*p - '0');
    }
    if (digits == 0)
        return -1;
    *out = acc;
    return 0;
}

int upload_parse_session(const char *body, upload_session *out)
{
    unsigned long long difficulty;

    memset(out, 0, sizeof *out);
    if (upload_json_find_string(body, "session_id", out->session_id,
                                sizeof out->session_id) != 0)
        return -1;
    if (upload_json_find_string(body, "signature", out->signature,
                                sizeof out->signature) != 0)
        return -1;
    if (upload_json_find_uint(body, "difficulty", &difficulty) != 0)
        return -1;
    if (difficulty > 64)
        return -1; /* physically unsolvable — treat as a bad response */
    out->difficulty = (unsigned)difficulty;
    if (!out->session_id[0] || !out->signature[0])
        return -1;
    return 0;
}

/* --- payload assembly ----------------------------------------------*/

/* The submit envelope: the session facts followed by the spliced
 * schema-1 document. The exact byte count is reused to size the
 * caller-side buffer. */
static const char PAYLOAD_ENVELOPE[] =
    "{\"session_id\": \"\", \"nonce\": \"\", \"signature\": \"\", "
    "\"document\": }";

/* Heap size a caller must provide for upload_build_payload(). */
size_t upload_payload_capacity(const char *session_id, const char *nonce,
                               const char *signature, size_t doc_len)
{
    return strlen(PAYLOAD_ENVELOPE) + (session_id ? strlen(session_id) : 0) +
           (nonce ? strlen(nonce) : 0) + (signature ? strlen(signature) : 0) +
           doc_len + 2;
}

int upload_build_payload(char *buf, size_t cap, const char *session_id,
                         const char *nonce, const char *signature,
                         const char *doc, size_t doc_len)
{
    size_t need, off;

    if (!buf || !session_id || !nonce || !signature || !doc)
        return -1;
    need = strlen(PAYLOAD_ENVELOPE) + strlen(session_id) + strlen(nonce) +
           strlen(signature) + doc_len;
    if (cap <= need)
        return -1;

    off = (size_t)snprintf(buf, cap,
                           "{\"session_id\": \"%s\", \"nonce\": \"%s\", "
                           "\"signature\": \"%s\", \"document\": ",
                           session_id, nonce, signature);
    memcpy(buf + off, doc, doc_len);
    off += doc_len;
    buf[off] = '}';
    buf[off + 1] = '\0';
    return 0;
}

/* --- submit response → result + URL ---------------------------------*/

/* Site root of the API base: a trailing "/api/v1/" (the documented
 * default shape) is stripped; any other base is used as-is. */
static void site_root(const char *base, char *out, size_t cap)
{
    size_t len = strlen(base);

    if (len >= 8 && !strcmp(base + len - 8, "/api/v1/"))
        len -= 8;
    else if (len && base[len - 1] == '/')
        len -= 1;
    if (len >= cap)
        len = cap - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

/* 1 when `url` already carries a scheme ("<scheme>://"), 0 otherwise
 * (RFC 3986 scheme: ALPHA, then ALPHA / DIGIT / "+" / "-" / "."). */
static int url_is_absolute(const char *url)
{
    size_t i;
    char c;

    if (!((url[0] >= 'a' && url[0] <= 'z') ||
          (url[0] >= 'A' && url[0] <= 'Z')))
        return 0;
    for (i = 1; url[i]; i++) {
        c = url[i];
        if (c == ':')
            return url[i + 1] == '/' && url[i + 2] == '/';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.'))
            return 0;
    }
    return 0;
}

/* The human-facing result URL: the site-relative `result_url` the
 * portal now includes in the 201 body (plan v2.1 §C3 — the real WS-B
 * detail page), joined to the site root; an absolute `result_url` is
 * taken as-is. Returns 0 when the full URL fits, -1 otherwise —
 * `out` is only written on success. */
static int join_result_url(char *out, size_t cap, const char *site,
                           const char *url)
{
    size_t site_len = strlen(site);
    size_t url_len = strlen(url);

    if (url_is_absolute(url))
        site_len = 0;
    if (site_len + url_len + 1 > cap)
        return -1;
    memcpy(out, site, site_len);
    memcpy(out + site_len, url, url_len + 1);
    return 0;
}

int upload_parse_submit_response(const char *body, const char *base_url,
                                 upload_result *out)
{
    unsigned long long id;
    char site[UPLOAD_RESULT_URL_MAX];
    char url[UPLOAD_RESULT_URL_MAX];

    memset(out, 0, sizeof *out);
    if (upload_json_find_uint(body, "id", &id) != 0)
        return -1;
    if (upload_json_find_string(body, "status", out->status,
                                sizeof out->status) != 0)
        return -1;
    out->result_id = id;
    site_root(base_url, site, sizeof site);
    if (upload_json_find_string(body, "result_url", url, sizeof url) == 0 &&
        url[0] != '\0' &&
        join_result_url(out->result_url, sizeof out->result_url, site,
                        url) == 0)
        return 0;
    /* No usable `result_url` in the body (older portal, empty or
     * oversized value): the legacy client-side fallback. */
    if (snprintf(out->result_url, sizeof out->result_url, "%s/result/%llu/",
                 site, id) >= (int)sizeof out->result_url)
        return -1;
    return 0;
}

/* --- configuration ---------------------------------------------------*/

const char *upload_base_url(void)
{
    const char *env = getenv(UPLOAD_BASE_URL_ENV);

    return (env && env[0]) ? env : UPLOAD_DEFAULT_BASE_URL;
}

const char *upload_rc_name(int rc)
{
    switch (rc) {
    case UPLOAD_OK:
        return "ok";
    case UPLOAD_UNAVAILABLE:
        return "unavailable";
    case UPLOAD_ERR_NETWORK:
        return "network error";
    case UPLOAD_ERR_PROTOCOL:
        return "protocol error";
    case UPLOAD_ERR_REJECTED:
        return "rejected";
    default:
        return "unknown";
    }
}

/* --- the network flow ------------------------------------------------*/

/* Diagnostic helper shared by both builds. */
static void set_reason(upload_result *out, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(out->reason, sizeof out->reason, fmt, ap);
    va_end(ap);
}

#ifdef HAVE_CURL

#include <curl/curl.h>

struct mem {
    char *data;
    size_t len;
};

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *vd)
{
    struct mem *m = vd;
    size_t total = size * nmemb;
    char *grown = realloc(m->data, m->len + total + 1);

    if (!grown)
        return 0; /* signals the error to libcurl */
    m->data = grown;
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = '\0';
    return total;
}

/* One POST; fills `body` (NULL on transport failure) and `code`. */
static int post(const char *url, const char *payload, struct mem *body,
                long *code, char *errbuf)
{
    CURL *curl = curl_easy_init();

    body->data = NULL;
    body->len = 0;
    if (!curl)
        return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload ? payload : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     payload ? (long)strlen(payload) : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lotofbench/" LOTOFBENCH_VERSION);
    *code = 0;
    CURLcode rc = curl_easy_perform(curl);

    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        free(body->data);
        body->data = NULL;
        return -1;
    }
    return 0;
}

/* base + path into a fixed buffer with an explicit length check (no
 * silent truncation of the endpoint). */
static int endpoint(char *dst, size_t cap, const char *base,
                    const char *path)
{
    size_t base_len = strlen(base), path_len = strlen(path);

    if (base_len + path_len + 1 > cap)
        return -1;
    memcpy(dst, base, base_len);
    memcpy(dst + base_len, path, path_len);
    dst[base_len + path_len] = '\0';
    return 0;
}

int upload_session_begin(const char *base_url, upload_session *out,
                         upload_result *diag)
{
    char url[512];
    char errbuf[CURL_ERROR_SIZE] = "";
    struct mem body = { NULL, 0 };
    long code = 0;
    int status = UPLOAD_ERR_NETWORK;

    memset(diag, 0, sizeof *diag);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        set_reason(diag, "curl init failed");
        return UPLOAD_ERR_NETWORK;
    }

    if (endpoint(url, sizeof url, base_url, "session/") != 0) {
        set_reason(diag, "base URL too long");
        status = UPLOAD_ERR_PROTOCOL;
        goto done;
    }
    if (post(url, "", &body, &code, errbuf) != 0) {
        set_reason(diag, "session request failed: %s",
                   errbuf[0] ? errbuf : "connection error");
        goto done;
    }
    if (code != 200) {
        set_reason(diag, "session endpoint returned HTTP %ld", code);
        status = UPLOAD_ERR_PROTOCOL;
        goto done;
    }
    if (upload_parse_session(body.data, out) != 0) {
        set_reason(diag, "malformed session response");
        status = UPLOAD_ERR_PROTOCOL;
        goto done;
    }
    fprintf(stderr, "lotofbench: upload: session %s issued, difficulty %u\n",
            out->session_id, out->difficulty);
    status = UPLOAD_OK;

done:
    free(body.data);
    curl_global_cleanup();
    return status;
}

int upload_submit_with_session(const char *base_url,
                               const upload_session *session,
                               const char *doc, size_t doc_len,
                               upload_result *out)
{
    char url[512];
    char errbuf[CURL_ERROR_SIZE] = "";
    struct mem body = { NULL, 0 };
    long code = 0;
    int status = UPLOAD_ERR_NETWORK;

    memset(out, 0, sizeof *out);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        set_reason(out, "curl init failed");
        return UPLOAD_ERR_NETWORK;
    }

    /* proof of work — progress on stderr, never stdout */
    unsigned long long nonce;

    if (pow_solve(session->session_id, session->difficulty, &nonce,
                  stderr) != 0) {
        set_reason(out, "unsolvable proof of work (difficulty %u)",
                   session->difficulty);
        status = UPLOAD_ERR_PROTOCOL;
        goto done;
    }
    char nonce_str[UPLOAD_NONCE_DIGITS_MAX];

    snprintf(nonce_str, sizeof nonce_str, "%llu", nonce);
    fprintf(stderr,
            "lotofbench: upload: proof of work solved, nonce %s\n",
            nonce_str);

    /* submit the wrapped document */
    size_t cap = upload_payload_capacity(session->session_id, nonce_str,
                                         session->signature, doc_len);
    char *payload = malloc(cap);

    if (!payload) {
        set_reason(out, "out of memory");
        status = UPLOAD_ERR_NETWORK;
        goto done;
    }
    if (upload_build_payload(payload, cap, session->session_id, nonce_str,
                             session->signature, doc, doc_len) != 0) {
        free(payload);
        set_reason(out, "payload assembly failed");
        status = UPLOAD_ERR_PROTOCOL;
        goto done;
    }
    if (endpoint(url, sizeof url, base_url, "submit/") != 0) {
        free(payload);
        set_reason(out, "base URL too long");
        status = UPLOAD_ERR_PROTOCOL;
        goto done;
    }
    int pr = post(url, payload, &body, &code, errbuf);

    free(payload);
    if (pr != 0) {
        set_reason(out, "submit request failed: %s",
                   errbuf[0] ? errbuf : "connection error");
        goto done;
    }
    if (code == 201 && upload_parse_submit_response(body.data, base_url,
                                                    out) == 0) {
        status = UPLOAD_OK;
        goto done;
    }
    if (code == 400) {
        upload_json_find_string(body.data, "reason", out->reason,
                                sizeof out->reason);
        if (!out->reason[0])
            set_reason(out, "portal rejected the submission (HTTP 400)");
        status = UPLOAD_ERR_REJECTED;
        goto done;
    }
    set_reason(out, "submit endpoint returned HTTP %ld", code);
    status = UPLOAD_ERR_PROTOCOL;

done:
    free(body.data);
    curl_global_cleanup();
    return status;
}


#else /* !HAVE_CURL — the zero-dependency build stays offline */

static int upload_unavailable(upload_result *out)
{
    memset(out, 0, sizeof *out);
    set_reason(out, "this binary was built without libcurl");
    fprintf(stderr,
            "lotofbench: upload unavailable: this binary was built "
            "without libcurl\n");
    return UPLOAD_UNAVAILABLE;
}

int upload_session_begin(const char *base_url, upload_session *out,
                         upload_result *diag)
{
    (void)base_url;
    (void)out;
    return upload_unavailable(diag);
}

int upload_submit_with_session(const char *base_url,
                               const upload_session *session,
                               const char *doc, size_t doc_len,
                               upload_result *out)
{
    (void)base_url;
    (void)session;
    (void)doc;
    (void)doc_len;
    return upload_unavailable(out);
}

#endif /* HAVE_CURL */

/* One-shot composition — common to both builds. */
int upload_submit(const char *base_url, const char *doc, size_t doc_len,
                  upload_result *out)
{
    upload_session session;
    int rc = upload_session_begin(base_url, &session, out);

    if (rc != UPLOAD_OK)
        return rc;
    return upload_submit_with_session(base_url, &session, doc, doc_len, out);
}
