/* LotofBench tests — portal upload helpers (upload/upload.[ch]).
 * Pure, buffer-only: the network flow itself is exercised by the live
 * smoke, never here. */

#define _POSIX_C_SOURCE 200809L

#include "tests.h"

#include "upload/upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A session response exactly as the portal's POST /api/v1/session/
 * answers it (views.session_init). */
static const char SESSION_BODY[] =
    "{\"session_id\": \"3f2b8c1a-9d4e-4f0a-8c1b-2d3e4f5a6b7c\", "
    "\"issued_at\": \"2026-11-16T09:00:00.123456Z\", "
    "\"expires_at\": \"2026-11-17T09:00:00.123456Z\", "
    "\"difficulty\": 22, "
    "\"pow\": {\"algorithm\": \"sha256-leading-bits\", "
    "\"prefix\": \"3f2b8c1a-9d4e-4f0a-8c1b-2d3e4f5a6b7c\"}, "
    "\"signature\": \"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293"
    "a4b5c6d7e8f90\"}";

void test_upload_json_find_string(void)
{
    char out[128];

    CHECK_U64(upload_json_find_string(SESSION_BODY, "session_id", out,
                                      sizeof out),
              0);
    CHECK_STR(out, "3f2b8c1a-9d4e-4f0a-8c1b-2d3e4f5a6b7c");

    CHECK_U64(upload_json_find_string(SESSION_BODY, "signature", out,
                                      sizeof out),
              0);
    CHECK_U64(strlen(out), 64);

    /* missing key */
    CHECK_U64(upload_json_find_string(SESSION_BODY, "nope", out,
                                      sizeof out),
              -1);

    /* escapes are decoded, the cap truncates safely */
    CHECK_U64(upload_json_find_string("{\"k\": \"a\\\"b\\\\c\"}", "k", out,
                                      sizeof out),
              0);
    CHECK_STR(out, "a\"b\\c");
    CHECK_U64(upload_json_find_string("{\"k\": \"0123456789\"}", "k", out,
                                      6),
              0);
    CHECK_STR(out, "01234"); /* cap-1 bytes + NUL */
}

void test_upload_json_find_uint(void)
{
    unsigned long long v = 0;

    CHECK_U64(upload_json_find_uint(SESSION_BODY, "difficulty", &v), 0);
    CHECK_U64(v, 22);
    CHECK_U64(upload_json_find_uint("{\"n\": 0007}", "n", &v), 0);
    CHECK_U64(v, 7);
    CHECK_U64(upload_json_find_uint("{\"n\": 123abc}", "n", &v), 0);
    CHECK_U64(v, 123); /* leading digits, stops at the letter */
    CHECK_U64(upload_json_find_uint(SESSION_BODY, "nope", &v), -1);
    CHECK_U64(upload_json_find_uint("{\"n\": \"x\"}", "n", &v), -1);
    CHECK_U64(upload_json_find_uint("{\"n\": }", "n", &v), -1);
}

void test_upload_parse_session(void)
{
    upload_session s;

    CHECK_U64(upload_parse_session(SESSION_BODY, &s), 0);
    CHECK_STR(s.session_id, "3f2b8c1a-9d4e-4f0a-8c1b-2d3e4f5a6b7c");
    CHECK_STR(s.signature,
              "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293"
              "a4b5c6d7e8f90");
    CHECK_U64(s.difficulty, 22);

    CHECK_U64(upload_parse_session("{\"session_id\": \"x\"}", &s), -1);
    CHECK_U64(upload_parse_session("{\"difficulty\": 1}", &s), -1);
    CHECK_U64(upload_parse_session(
                  "{\"session_id\": \"x\", \"signature\": \"y\", "
                  "\"difficulty\": 65}",
                  &s),
              -1); /* difficulty past the 64-bit physical limit */
}

void test_upload_build_payload(void)
{
    char buf[256];
    const char *doc = "{\"benchmark\": \"lotofbench\"}\n";
    size_t cap = upload_payload_capacity("sid-1", "42", "sig-9", strlen(doc));

    CHECK(cap < sizeof buf);
    CHECK_U64(upload_build_payload(buf, sizeof buf, "sid-1", "42", "sig-9",
                                   doc, strlen(doc)),
              0);
    CHECK_STR(buf,
              "{\"session_id\": \"sid-1\", \"nonce\": \"42\", "
              "\"signature\": \"sig-9\", \"document\": "
              "{\"benchmark\": \"lotofbench\"}\n}");

    /* undersized buffer refuses instead of truncating */
    CHECK_U64(upload_build_payload(buf, 40, "sid-1", "42", "sig-9", doc,
                                   strlen(doc)),
              -1);
}

void test_upload_parse_submit_response(void)
{
    upload_result r;
    char body[640];
    char big[2 * UPLOAD_RESULT_URL_MAX];

    CHECK_U64(upload_parse_submit_response(
                  "{\"id\": 42, \"status\": \"verified\", "
                  "\"flags\": [\"multi_lt_single\"]}",
                  "https://lotofbench.cloud-mercato.com/api/v1/", &r),
              0);
    CHECK_U64(r.result_id, 42);
    CHECK_STR(r.status, "verified");
    CHECK_STR(r.result_url,
              "https://lotofbench.cloud-mercato.com/result/42/");

    /* a custom base keeps its root */
    CHECK_U64(upload_parse_submit_response("{\"id\": 7, \"status\": "
                                           "\"verified\"}",
                                           "http://localhost:19187/api/v1/",
                                           &r),
              0);
    CHECK_STR(r.result_url, "http://localhost:19187/result/7/");

    /* the portal-provided result_url wins: a site-relative path is
     * joined to the site root (plan v2.1 §C3 — the real WS-B page) */
    CHECK_U64(upload_parse_submit_response(
                  "{\"id\": 42, \"status\": \"verified\", "
                  "\"result_url\": \"/results/custom:42/\"}",
                  "https://lotofbench.cloud-mercato.com/api/v1/", &r),
              0);
    CHECK_STR(r.result_url,
              "https://lotofbench.cloud-mercato.com/results/custom:42/");

    /* an absolute result_url is taken as-is, from a custom base */
    CHECK_U64(upload_parse_submit_response(
                  "{\"id\": 42, \"status\": \"verified\", \"result_url\": "
                  "\"https://other.example.net/results/x/\"}",
                  "http://localhost:19187/api/v1/", &r),
              0);
    CHECK_STR(r.result_url, "https://other.example.net/results/x/");

    /* an empty result_url: the legacy fallback */
    CHECK_U64(upload_parse_submit_response(
                  "{\"id\": 9, \"status\": \"verified\", "
                  "\"result_url\": \"\"}",
                  "http://localhost:19187/api/v1/", &r),
              0);
    CHECK_STR(r.result_url, "http://localhost:19187/result/9/");

    /* a result_url too long for the field: the legacy fallback */
    memset(big, 'x', sizeof big - 1);
    big[0] = '/';
    big[sizeof big - 2] = '/';
    big[sizeof big - 1] = '\0';
    snprintf(body, sizeof body,
             "{\"id\": 11, \"status\": \"verified\", "
             "\"result_url\": \"%s\"}",
             big);
    CHECK_U64(upload_parse_submit_response(body,
                                           "http://localhost:19187/api/v1/",
                                           &r),
              0);
    CHECK_STR(r.result_url, "http://localhost:19187/result/11/");

    /* malformed body */
    CHECK_U64(upload_parse_submit_response("{\"status\": \"verified\"}",
                                           UPLOAD_DEFAULT_BASE_URL, &r),
              -1);
    CHECK_U64(upload_parse_submit_response("{\"id\": 3}", "x", &r), -1);
}

void test_upload_rejection_reason(void)
{
    char reason[UPLOAD_REASON_MAX];

    CHECK_U64(upload_json_find_string("{\"reason\": \"session already "
                                      "used\"}",
                                      "reason", reason, sizeof reason),
              0);
    CHECK_STR(reason, "session already used");
    CHECK_U64(upload_json_find_string("{\"other\": 1}", "reason", reason,
                                      sizeof reason),
              -1);
    CHECK_U64(upload_rc_name(UPLOAD_OK), "ok");
    CHECK_U64(upload_rc_name(UPLOAD_ERR_REJECTED), "rejected");
}

void test_upload_base_url_env(void)
{
    /* Without the variable: the production default. */
    unsetenv(UPLOAD_BASE_URL_ENV);
    CHECK_STR(upload_base_url(), UPLOAD_DEFAULT_BASE_URL);

    /* With the variable: the override, verbatim. */
    setenv(UPLOAD_BASE_URL_ENV, "http://localhost:19187/api/v1/", 1);
    CHECK_STR(upload_base_url(), "http://localhost:19187/api/v1/");
    unsetenv(UPLOAD_BASE_URL_ENV);
    CHECK_STR(upload_base_url(), UPLOAD_DEFAULT_BASE_URL);

    /* An empty value falls back to the default. */
    setenv(UPLOAD_BASE_URL_ENV, "", 1);
    CHECK_STR(upload_base_url(), UPLOAD_DEFAULT_BASE_URL);
    unsetenv(UPLOAD_BASE_URL_ENV);
}
