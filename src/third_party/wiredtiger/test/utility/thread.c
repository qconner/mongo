/*-
 * Public Domain 2014-2018 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
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

#include "test_util.h"

/*
 * A thread dedicated to appending records into a table. Works with fixed
 * length column stores and variable length column stores.
 * One thread (the first thread created by an application) checks for a
 * terminating condition after each insert.
 */
WT_THREAD_RET
thread_append(void *arg)
{
	TEST_OPTS *opts;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t id, recno;
	char buf[64];

	opts = (TEST_OPTS *)arg;
	conn = opts->conn;

	id = __wt_atomic_fetch_addv64(&opts->next_threadid, 1);
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(
	    session->open_cursor(session, opts->uri, NULL, "append", &cursor));

	buf[0] = '\2';
	for (recno = 1; opts->running; ++recno) {
		if (opts->table_type == TABLE_FIX)
			cursor->set_value(cursor, buf[0]);
		else {
			testutil_check(__wt_snprintf(buf, sizeof(buf),
			    "%" PRIu64 " VALUE ------", recno));
			cursor->set_value(cursor, buf);
		}
		testutil_check(cursor->insert(cursor));
		if (id == 0) {
			testutil_check(
			    cursor->get_key(cursor, &opts->max_inserted_id));
			if (opts->max_inserted_id >= opts->nrecords)
				opts->running = false;
		}
	}

	return (WT_THREAD_RET_VALUE);
}

/*
 * Below are a series of functions originally designed for test/fops. These
 * threads perform a series of simple API access calls, such as opening and
 * closing sessions and cursors. These functions require use of the
 * TEST_PER_THREAD_OPTS structure in test_util.h. Additionally there are two
 * event handler functions that should be used to suppress "expected" errors
 * that these functions generate. An example of the use of these functions and
 * structures is in the csuite test wt3363_checkpoint_op_races.
 */

/*
 * Handle errors that generated by series of functions below that we can safely
 * ignore.
 */
int
handle_op_error(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, int error, const char *errmsg)
{
	(void)(handler);
	(void)(session);

	/*
	 * Ignore complaints about missing files. It's unlikely but possible
	 * that checkpoints and cursor open operations can return this due to
	 * the sequencing of the various ops.
	 */
	if (error == ENOENT)
		return (0);

	/* Ignore complaints about failure to open bulk cursors. */
	if (strstr(
	    errmsg, "bulk-load is only supported on newly created") != NULL)
		return (0);

	return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

/*
 * Handle messages generated by the functions below that we can safely ignore.
 */
int
handle_op_message(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *message)
{
	(void)(handler);
	(void)(session);

	/* Ignore messages about failing to create forced checkpoints. */
	if (strstr(message, "forced or named checkpoint") != NULL)
		return (0);

	return (printf("%s\n", message) < 0 ? -1 : 0);
}

/*
 * Create a table and open a bulk cursor on it.
 */
void
op_bulk(void *arg)
{
	TEST_OPTS *opts;
	TEST_PER_THREAD_OPTS *args;
	WT_CURSOR *c;
	WT_SESSION *session;
	int ret;

	args = (TEST_PER_THREAD_OPTS *)arg;
	opts = args->testopts;

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	if ((ret = session->create(session, opts->uri, NULL)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			testutil_die(ret, "session.create");

	if (ret == 0) {
		__wt_yield();
		if ((ret = session->open_cursor(session,
		    opts->uri, NULL, "bulk,checkpoint_wait=false", &c)) == 0) {
			 testutil_check(c->close(c));
		} else if (ret != ENOENT && ret != EBUSY && ret != EINVAL)
			testutil_die(ret, "session.open_cursor bulk");
	}

	testutil_check(session->close(session, NULL));
	args->thread_counter++;
}

/*
 * Create a guaranteed unique table and open and close a bulk cursor on it.
 */
void
op_bulk_unique(void *arg)
{
	TEST_OPTS *opts;
	TEST_PER_THREAD_OPTS *args;
	WT_CURSOR *c;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	int ret;
	char new_uri[64];

	args = (TEST_PER_THREAD_OPTS *)arg;
	opts = args->testopts;
	__wt_random_init_seed(NULL, &rnd);

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	/* Generate a unique object name. */
	testutil_check(__wt_snprintf(
	    new_uri, sizeof(new_uri), "%s.%" PRIu64,
	    opts->uri, __wt_atomic_add64(&opts->unique_id, 1)));
	testutil_check(session->create(session, new_uri, NULL));

	__wt_yield();

	/*
	 * Opening a bulk cursor may have raced with a forced checkpoint
	 * which created a checkpoint of the empty file, and triggers an EINVAL.
	 */
	if ((ret = session->open_cursor(
	    session, new_uri, NULL, "bulk,checkpoint_wait=false", &c)) == 0) {
		testutil_check(c->close(c));
	} else if (ret != EINVAL && ret != EBUSY)
		testutil_die(ret,
		    "session.open_cursor bulk unique: %s", new_uri);

	while ((ret = session->drop(session, new_uri, __wt_random(&rnd) & 1 ?
	    "force,checkpoint_wait=false" : "checkpoint_wait=false")) != 0)
		if (ret != EBUSY)
			testutil_die(ret, "session.drop: %s", new_uri);
		else
			/*
			 * The EBUSY is expected when we run with
			 * checkpoint_wait set to false, so we increment the
			 * counter while in this loop to avoid false positives.
			 */
			args->thread_counter++;

	testutil_check(session->close(session, NULL));
	args->thread_counter++;
}

/*
 * Open and close cursor on a table.
 */
void
op_cursor(void *arg)
{
	TEST_OPTS *opts;
	TEST_PER_THREAD_OPTS *args;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	args = (TEST_PER_THREAD_OPTS *)arg;
	opts = args->testopts;

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	if ((ret = session->open_cursor(
	    session, opts->uri, NULL, NULL, &cursor)) != 0) {
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.open_cursor");
	} else
		testutil_check(cursor->close(cursor));

	testutil_check(session->close(session, NULL));
	args->thread_counter++;
}

/*
 * Create a table.
 */
void
op_create(void *arg)
{
	TEST_OPTS *opts;
	TEST_PER_THREAD_OPTS *args;
	WT_SESSION *session;
	int ret;

	args = (TEST_PER_THREAD_OPTS *)arg;
	opts = args->testopts;

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	if ((ret = session->create(session, opts->uri, NULL)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			testutil_die(ret, "session.create");

	testutil_check(session->close(session, NULL));
	args->thread_counter++;
}

/*
 * Create and drop a unique guaranteed table.
 */
void
op_create_unique(void *arg)
{
	TEST_OPTS *opts;
	TEST_PER_THREAD_OPTS *args;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	int ret;
	char new_uri[64];

	args = (TEST_PER_THREAD_OPTS *)arg;
	opts = args->testopts;
	__wt_random_init_seed(NULL, &rnd);

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	/* Generate a unique object name. */
	testutil_check(__wt_snprintf(
	    new_uri, sizeof(new_uri), "%s.%" PRIu64,
	    opts->uri, __wt_atomic_add64(&opts->unique_id, 1)));
	testutil_check(session->create(session, new_uri, NULL));

	__wt_yield();
	while ((ret = session->drop(session, new_uri, __wt_random(&rnd) & 1 ?
	    "force,checkpoint_wait=false" : "checkpoint_wait=false")) != 0)
		if (ret != EBUSY)
			testutil_die(ret, "session.drop: %s", new_uri);
		else
			/*
			 * The EBUSY is expected when we run with
			 * checkpoint_wait set to false, so we increment the
			 * counter while in this loop to avoid false positives.
			 */
			args->thread_counter++;

	testutil_check(session->close(session, NULL));
	args->thread_counter++;
}

/*
 * Drop a table.
 */
void
op_drop(void *arg)
{
	TEST_OPTS *opts;
	TEST_PER_THREAD_OPTS *args;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	int ret;

	args = (TEST_PER_THREAD_OPTS *)arg;
	opts = args->testopts;
	__wt_random_init_seed(NULL, &rnd);

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	if ((ret = session->drop(session, opts->uri, __wt_random(&rnd) & 1 ?
	    "force,checkpoint_wait=false" : "checkpoint_wait=false")) != 0)
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.drop");

	testutil_check(session->close(session, NULL));
	args->thread_counter++;
}
