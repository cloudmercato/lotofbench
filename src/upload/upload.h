#ifndef LOTOFBENCH_UPLOAD_H
#define LOTOFBENCH_UPLOAD_H

#include <stddef.h>

/* Upload of a finished LotofBench document to the ingest portal
 * (plan v2.1 §WS-C/C3). The flow mirrors the portal API, with one
 * timing rule: the portal cross-checks the claimed runtime_seconds
 * against the session age, so the session handshake happens BEFORE
 * the benchmark passes run and the submit after — the CLI drives the
 * two phases itself:
 *
 *   upload_session_begin()   → POST <base>/session/ (signed single-use
 *                              session: id, difficulty, signature)
 *   … the passes run, the document is built …
 *   upload_submit_with_session() → pow_solve() (decimal nonce,
 *                              progress on stderr) then
 *                              POST <base>/submit/ with
 *                              {"session_id","nonce","signature",
 *                              "document"} → 201 {"id","status",
 *                              "result_url"}
 *
 * upload_submit() is the one-shot composition of both phases (fine
 * for short documents, impossible for real runs because the age
 * check would reject them). The base URL is $LOTOFBENCH_UPLOAD_URL
 * or the production default. HTTPS goes through libcurl; the
 * zero-dependency build (no HAVE_CURL) compiles every pure helper
 * and degrades the network flows to UPLOAD_UNAVAILABLE stubs — the
 * CLI stays fully offline. */

#define UPLOAD_DEFAULT_BASE_URL "https://lotofbench.cloud-mercato.com/api/v1/"
#define UPLOAD_BASE_URL_ENV "LOTOFBENCH_UPLOAD_URL"

#define UPLOAD_SESSION_ID_MAX 48   /* uuid4 text form + NUL */
#define UPLOAD_SIGNATURE_MAX  65   /* 64 hex digits + NUL */
#define UPLOAD_NONCE_DIGITS_MAX 24 /* 2^64 is 20 digits + margin */
#define UPLOAD_REASON_MAX     256
#define UPLOAD_RESULT_URL_MAX 256

/* 1 when the binary was built with curl support: the upload flag
 * defaults to ON there and to OFF on the zero-dependency build. */
#ifdef HAVE_CURL
#define UPLOAD_DEFAULT_ENABLED 1
#else
#define UPLOAD_DEFAULT_ENABLED 0
#endif

/* Result codes of upload_submit(). */
#define UPLOAD_OK           0
#define UPLOAD_UNAVAILABLE (-1) /* built without libcurl */
#define UPLOAD_ERR_NETWORK (-2) /* transport failure */
#define UPLOAD_ERR_PROTOCOL (-3) /* unexpected status/body */
#define UPLOAD_ERR_REJECTED (-4) /* portal answered 400 with a reason */

typedef struct {
    char session_id[UPLOAD_SESSION_ID_MAX];
    char signature[UPLOAD_SIGNATURE_MAX];
    unsigned difficulty;
} upload_session;

typedef struct {
    unsigned long long result_id;
    char status[16];
    char result_url[UPLOAD_RESULT_URL_MAX]; /* portal result_url, else
                                             * site/result/<id>/ */
    char reason[UPLOAD_REASON_MAX];         /* rejection / diagnostic */
} upload_result;

/* --- pure helpers (compiled everywhere, unit-tested on buffers) --- */

/* Extract `"key": "<string>"` from a small JSON body (portal-shaped;
 * escapes \" and \\ are decoded, other escapes pass through literally).
 * Returns 0 on success, -1 when the key is missing or malformed. */
int upload_json_find_string(const char *body, const char *key, char *out,
                            size_t cap);

/* Extract `"key": <unsigned integer>`. 0 on success, -1 otherwise. */
int upload_json_find_uint(const char *body, const char *key,
                          unsigned long long *out);

/* Parse the session handshake response (session_id, difficulty,
 * signature). 0 on success, -1 on a malformed body. */
int upload_parse_session(const char *body, upload_session *out);

/* Heap size the caller must provide for upload_build_payload(). */
size_t upload_payload_capacity(const char *session_id, const char *nonce,
                               const char *signature, size_t doc_len);

/* Wrap the raw schema-1 document into the submit payload
 * {"session_id","nonce","signature","document"}. `doc` must be valid
 * JSON (the CLI prints it) and is spliced verbatim. 0 on success, -1
 * when the buffer is too small. */
int upload_build_payload(char *buf, size_t cap, const char *session_id,
                         const char *nonce, const char *signature,
                         const char *doc, size_t doc_len);

/* Parse the 201 submit response and build the human-facing result
 * URL: the portal-provided `result_url` — site-relative paths joined
 * to the site root (trailing "/api/v1/" stripped from the API base),
 * absolute URLs kept as-is — with the legacy site "/result/<id>/"
 * fallback when the body carries no usable value (older portals).
 * 0 on success, -1 otherwise. */
int upload_parse_submit_response(const char *body, const char *base_url,
                                 upload_result *out);

/* Effective base URL: $LOTOFBENCH_UPLOAD_URL when set, else the
 * production default. */
const char *upload_base_url(void);

/* Short human-readable name of a result code (tests, diagnostics). */
const char *upload_rc_name(int rc);

/* --- the network flow: full under HAVE_CURL, stub otherwise --- */

/* Phase 1 — request the single-use session. Call it BEFORE the passes
 * run so the portal's runtime-vs-age cross-check can pass. On failure
 * the diagnostic is copied into diag->reason. */
int upload_session_begin(const char *base_url, upload_session *out,
                         upload_result *diag);

/* Phase 2 — solve the proof of work for `session` (progress lines on
 * stderr, never stdout) and submit the wrapped document. On
 * UPLOAD_ERR_REJECTED the portal's reason is copied into
 * out->reason. */
int upload_submit_with_session(const char *base_url,
                               const upload_session *session,
                               const char *doc, size_t doc_len,
                               upload_result *out);

/* One-shot composition of both phases (session right before the
 * submit — only viable for documents whose runtime is below the proof
 * of work duration; the CLI never uses it). */
int upload_submit(const char *base_url, const char *doc, size_t doc_len,
                  upload_result *out);

#endif /* LOTOFBENCH_UPLOAD_H */
