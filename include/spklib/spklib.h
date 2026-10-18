/* spklib.h -- the SpektraLab library SDK's C ABI.
 *
 * The whole surface is one call: a method name and a JSON object in, a JSON
 * object out. The same method table is served over stdio by `spklib-host`
 * (docs/PROTOCOL.md), so an app that links this library and an app that
 * talks to the helper see the same methods, the same replies and the same
 * errors.
 *
 * Boundary: spklib says where images are and keeps the data around them --
 * libraries, variants, ratings, colour tags, collections, SpektraLab's own
 * edits as opaque JSON. It never decodes an image, renders a preview or
 * writes a cache, and it never writes to a Capture One catalog.
 *
 * Thread safety: a context serialises its calls; use one per thread for
 * parallelism. Strings are UTF-8. Every string returned through `out_json`
 * is owned by the caller and released with spklib_free.
 */

#ifndef SPKLIB_H
#define SPKLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#define SPKLIB_ABI_VERSION 1

typedef struct spklib_context spklib_context;

/* Returned by spklib_call. */
enum {
    SPKLIB_OK = 0,
    /* `out_json` is {"code": "...", "message": "...", "data": ...}. Codes:
     * not_found, invalid_params, read_only, conflict, source_busy,
     * unsupported, exists, io, internal, unknown_method. */
    SPKLIB_ERROR = 1,
};

/* `options_json` may be NULL or {"store": "<dir>"} -- where libraries that
 * mirror a Capture One catalog are kept when no path is given (the app passes
 * ~/Library/Application Support/SpektraLab/Libraries). */
spklib_context* spklib_create(const char* options_json, char** out_error);
void spklib_destroy(spklib_context* ctx);

/* `params_json` may be NULL for {}. On return `*out_json` is the result
 * (SPKLIB_OK) or the error (SPKLIB_ERROR); never NULL. */
int spklib_call(spklib_context* ctx, const char* method, const char* params_json, char** out_json);

void spklib_free(char* s);

/* "spklib <semver> (abi N, schema M)" */
const char* spklib_version(void);

#ifdef __cplusplus
}
#endif

#endif /* SPKLIB_H */
