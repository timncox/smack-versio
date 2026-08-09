/*
 * versio_alloc — a bump allocator so the vendored engine needs no edits.
 *
 * smack_create() does four calloc()s (a 13.4 MB ring plus per-lane delay and
 * reverb lines) and smack_destroy() frees them. Daisy has no meaningful heap,
 * and rewriting smack_create() would fork the engine.
 *
 * Instead the ARM build compiles smack_core.c with
 *     -Dcalloc=versio_calloc -Dfree=versio_free
 * pointing those two calls at an SDRAM pool. Allocation happens once at boot
 * and is never returned, so free() is a no-op -- correct here, because the
 * module creates exactly one engine and keeps it until power-off.
 *
 * The pool itself is declared in smack_versio.cpp (it needs DSY_SDRAM_BSS
 * from libDaisy); this file stays plain C so it can be unit-tested natively.
 */
#ifndef VERSIO_ALLOC_H
#define VERSIO_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   versio_alloc_init(void *pool, size_t bytes);
void  *versio_calloc(size_t nmemb, size_t size);
void   versio_free(void *ptr);

/* For the boot-time log: how much of the pool the engine actually took. */
size_t versio_alloc_used(void);
size_t versio_alloc_capacity(void);

/* Set when an allocation did not fit. If this is true after smack_create(),
 * the module is broken and must say so rather than run half-initialised. */
int    versio_alloc_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* VERSIO_ALLOC_H */
