/*-
 * Public Domain 2015 MongoDB, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#include <lz4.h>

/*
 * We need to include the configuration file to detect whether this extension
 * is being built into the WiredTiger library.
 */
#include "wiredtiger_config.h"


/*! [WT_COMPRESSOR initialization structure] */
/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */

	unsigned long nop_calls;		/* Count of calls */

} LZ4_COMPRESSOR;
/*! [WT_COMPRESSOR initialization structure] */

/*! [WT_COMPRESSOR compress] */
/*
 * nop_compress --
 *	A simple compression example that passes data through unchanged.
 */
static int
lz4_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	LZ4_COMPRESSOR *nop_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;				/* Unused parameters */

	++nop_compressor->nop_calls;		/* Call count */

    fprintf(stdout, "lz4_compress:  STUBBED OUT %lu %lu\n", src_len, dst_len);

	*compression_failed = 0;
	if (dst_len < src_len) {
		*compression_failed = 1;
		return (0);
	}

	memcpy(dst, src, src_len);
	*result_lenp = src_len;

	return (0);
}
/*! [WT_COMPRESSOR compress] */

/*! [WT_COMPRESSOR decompress] */
/*
 * nop_decompress --
 *	A simple decompression example that passes data through unchanged.
 */
static int
lz4_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	LZ4_COMPRESSOR *nop_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;				/* Unused parameters */
	(void)src_len;

	++nop_compressor->nop_calls;		/* Call count */

    fprintf(stdout, "lz4_decompress:  STUBBED OUT %lu %lu\n", src_len, dst_len);

	/*
	 * The destination length is the number of uncompressed bytes we're
	 * expected to return.
	 */
	memcpy(dst, src, dst_len);
	*result_lenp = dst_len;
	return (0);
}
/*! [WT_COMPRESSOR decompress] */

/*! [WT_COMPRESSOR presize] */
/*
 * nop_pre_size --
 *	A simple pre-size example that returns the source length.
 */
static int
lz4_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	LZ4_COMPRESSOR *nop_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;				/* Unused parameters */
	(void)src;

    fprintf(stdout, "lz4_pre_size: STUBBED OUT %lu\n", src_len);

	++nop_compressor->nop_calls;		/* Call count */

	*result_lenp = src_len;
	return (0);
}
/*! [WT_COMPRESSOR presize] */

/*! [WT_COMPRESSOR terminate] */
/*
 * nop_terminate --
 *	WiredTiger no-op compression termination.
 */
static int
lz4_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	LZ4_COMPRESSOR *nop_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;				/* Unused parameters */

	++nop_compressor->nop_calls;		/* Call count */

    fprintf(stdout, "lz4_terminate: %lu calls\n", nop_compressor->nop_calls);

	/* Free the allocated memory. */
	free(compressor);

	return (0);
}
/*! [WT_COMPRESSOR terminate] */

/*! [WT_COMPRESSOR initialization function] */
/*
 * wiredtiger_extension_init --
 *	A simple shared library compression example.
 */
int
lz4_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	LZ4_COMPRESSOR *lz4_compressor;

	(void)config;				/* Unused parameters */

    int dummy2 = LZ4_versionNumber();
    fprintf(stdout, "lz4_extension_init:  LZ4 library version: %d\n", dummy2);

	if ((lz4_compressor = calloc(1, sizeof(LZ4_COMPRESSOR))) == NULL)
		return (errno);

	/*
	 * Allocate a local compressor structure, with a WT_COMPRESSOR structure
	 * as the first field, allowing us to treat references to either type of
	 * structure as a reference to the other type.
	 *
	 * This could be simplified if only a single database is opened in the
	 * application, we could use a static WT_COMPRESSOR structure, and a
	 * static reference to the WT_EXTENSION_API methods, then we don't need
	 * to allocate memory when the compressor is initialized or free it when
	 * the compressor is terminated.  However, this approach is more general
	 * purpose and supports multiple databases per application.
	 */
	lz4_compressor->compressor.compress = lz4_compress;
	lz4_compressor->compressor.compress_raw = NULL;
	lz4_compressor->compressor.decompress = lz4_decompress;
	lz4_compressor->compressor.pre_size = lz4_pre_size;
	lz4_compressor->compressor.terminate = lz4_terminate;

	lz4_compressor->wt_api = connection->get_extension_api(connection);

	/* Load the compressor */
	return (connection->add_compressor(
	    connection, "lz4", (WT_COMPRESSOR *)lz4_compressor, NULL));
}
/*! [WT_COMPRESSOR initialization function] */



/*
 * We have to remove this symbol when building as a builtin extension otherwise
 * it will conflict with other builtin libraries.
 */
#ifndef	HAVE_BUILTIN_EXTENSION_LZ4
/*
 * wiredtiger_extension_init --
 *	WiredTiger LZ4 compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	return lz4_extension_init(connection, config);
}
#endif
