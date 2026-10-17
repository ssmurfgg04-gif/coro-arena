/* canned.h - byte-identical canned responses for every arena contestant.
 *
 * All servers answer the load generator with the same five canned responses
 * (feed / post / like / create / health), loaded once from the same JSON
 * bodies the sink baseline uses, so the only variable across the arena is
 * each library's event loop + connection handling.
 *
 * C and C++ compatible. Thread-safe after canned_init() returns.
 */
#ifndef CANNED_H
#define CANNED_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load the two JSON bodies and build the five canned responses.
 * Returns 0 on success, -1 on failure (message on stderr). Call exactly once
 * per process before serving. */
int canned_init(const char *feed_path, const char *post_path);

/* Route one request line (no trailing CRLF) to its canned response.
 * Returns a shared immutable buffer and sets *out_len. Unknown requests
 * get the health response, same as the sink baseline. */
const char *canned_route(const char *line, int len, int *out_len);

/* The health response, for readiness probes. */
const char *canned_health(int *out_len);

#ifdef __cplusplus
}
#endif
#endif
