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

/*
 * We use the https://github.com/Cyan4973/lz4 LZ4 implementation in C.
 * The code is licensed under the new BSD 3-Clause License.
 * From the MongoDB scons build perspective, it is "vendored"
 * and lives in src/third_party/lz4-r127/
 */
#include <lz4.h>

/*
 * We need to include the configuration file to detect whether this extension
 * is being built into the WiredTiger library.
 */
#include "wiredtiger_config.h"


/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */

  //unsigned long lz4_calls;		/* Count of calls */

} LZ4_COMPRESSOR;


/*
 *  lz4_compress --
 *	WiredTiger LZ4 compression.
 */
static int
lz4_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
    //LZ4_COMPRESSOR *lz4_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;    /* Unused parameters */

	//++lz4_compressor->lz4_calls;		/* Call count */

    //fprintf(stdout, "lz4_compress:  src data length: %lu  dest buffer length: %lu\n", src_len, dst_len);

	*compression_failed = 0;
	if (dst_len < src_len + 8) {
        // do not attempt but should not happen with prior call to lz4_pre_size
        // TODO: consider change to assert
        fprintf(stdout, "lz4_compress:  not attempting compression due to small destination buffer\n");
		*compression_failed = 1;
		return (0);
	}

    /*
     *  Store the length of the compressed block in the first sizeof(size_t) bytes.
     *  We will skip past the length value to store the compressed bytes.
	 */
	char *buf = (char *)dst + sizeof(size_t);

    /*
     * Call LZ4 to compress
     */
    int lz4_len = LZ4_compress((const char *)src, buf, src_len);
    //fprintf(stdout, "lz4_compress:  dest data length: %d\n", lz4_len);
	*result_lenp = lz4_len;
    *(size_t *)dst = (size_t)lz4_len;

    // return the compressed data length, including our size_t compressed data byte count
    *result_lenp = lz4_len + sizeof(size_t);

	return (0);
}



/*
 * lz4_decompress --
 *	WiredTiger LZ4 decompression.
 */
static int
lz4_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
    //LZ4_COMPRESSOR *lz4_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;				/* Unused parameters */
	(void)src_len;

	//++lz4_compressor->lz4_calls;		/* Call count */

    //fprintf(stdout, "lz4_decompress:  compressed data buffer size: %lu   dest buffer size: %lu\n", src_len, dst_len);

    /* retrieve compressed data length from start of compressed data buffer */
    size_t src_data_len = *(size_t *)src;
    //fprintf(stdout, "lz4_decompress:  compressed data length: %lu\n", src_data_len);

    /* skip over sizeof(size_t) bytes for actual start of compressed data */
    char *compressed_data = (char *)src + sizeof(size_t);

    // the destination buffer length should always be sufficient
    // because WT keeps track of the byte count before compression

    /* Call LZ4 to decompress */
    int decoded = LZ4_decompress_safe(compressed_data, (char *)dst, src_data_len, dst_len);
    //fprintf(stdout, "lz4_decompress:  decompressed data length: %d\n", decoded);
    if (decoded < 0) {
      fprintf(stdout, "lz4_decompress:  ERROR in LZ4_decompress_safe(): %d\n", decoded);
      return(1);
    }

    size_t decompressed_data_len = decoded;
    // return the uncompressed data length
    *result_lenp = decompressed_data_len;
    
    return(0);
}




/*
 * lz4_pre_size --
 *	WiredTiger LZ4 destination buffer sizing for compression.
 */
static int
lz4_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
    //LZ4_COMPRESSOR *lz4_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;    /* Unused parameters */
	(void)src;

	//++lz4_compressor->lz4_calls;		/* Call count */

    //  we must reserve a little extra space for our compressed data length
    //  value stored at the start of the compressed data buffer.  Random
    //  data doesn't compress well and we could overflow the destination buffer.

    size_t dst_buffer_len_needed = src_len + sizeof(size_t);
    //fprintf(stdout, "lz4_pre_size: dest buffer size needed: %lu\n", dst_buffer_len_needed);

	*result_lenp = dst_buffer_len_needed;
	return (0);
}




/*
 * lz4_terminate --
 *	WiredTiger LZ4 compression termination.
 */
static int
lz4_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    //LZ4_COMPRESSOR *lz4_compressor = (LZ4_COMPRESSOR *)compressor;

	(void)session;    /* Unused parameters */

	//++lz4_compressor->lz4_calls;		/* Call count */

    //fprintf(stdout, "lz4_terminate:  %lu calls\n", lz4_compressor->lz4_calls);

	/* Free the allocated memory. */
	free(compressor);

	return (0);
}




/*
 * wiredtiger_extension_init --
 *	A simple shared library compression example.
 */
int
lz4_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	LZ4_COMPRESSOR *lz4_compressor;

	(void)config;    /* Unused parameters */

    //int dummy2 = LZ4_versionNumber();
    //fprintf(stdout, "lz4_extension_init:  LZ4 library version: %d\n", dummy2);

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
