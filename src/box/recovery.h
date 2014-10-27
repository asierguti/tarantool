#ifndef TARANTOOL_RECOVERY_H_INCLUDED
#define TARANTOOL_RECOVERY_H_INCLUDED
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdbool.h>

#include "trivia/util.h"
#include "third_party/tarantool_ev.h"
#include "log_io.h"
#include "vclock.h"
#include "tt_uuid.h"
#include "replica.h"
#include "fiber.h"
#include "tt_pthread.h"
#include "xrow.h"
#include "small/region.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct fiber;
struct tbuf;

typedef void (row_handler)(void *, struct xrow_header *packet);
typedef void (snapshot_handler)(struct log_io *);
typedef void (join_handler)(const struct tt_uuid *node_uuid);

/** A "condition variable" that allows fibers to wait when a given
 * LSN makes it to disk.
 */

struct wal_write_request {
  STAILQ_ENTRY(wal_write_request) wal_fifo_entry;
  /* Auxiliary. */
  int64_t res;
  struct fiber *fiber;
  struct xrow_header *row;
};

/* Context of the WAL writer thread. */
STAILQ_HEAD(wal_fifo, wal_write_request);

struct wal_writer
{
  struct wal_fifo input;
  struct wal_fifo commit;
  struct cord cord;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  ev_async write_event;
  struct fio_batch *batch;
  bool is_shutdown;
  bool is_rollback;
  ev_loop *txn_loop;
  struct vclock vclock;
};

struct wal_watcher;

enum wal_mode { WAL_NONE = 0, WAL_WRITE, WAL_FSYNC, WAL_MODE_MAX };

/** String constants for the supported modes. */
extern const char *wal_mode_STRS[];

struct recovery_state {
	struct vclock vclock;
	/* The WAL we're currently reading/writing from/to. */
	struct log_io *current_wal;
	struct log_dir snap_dir;
	struct log_dir wal_dir;
	int64_t signature; /* used to find missing xlog files */
	struct wal_writer *writer;
	struct wal_watcher *watcher;
	struct remote remote;
	/**
	 * row_handler is a module callback invoked during initial
	 * recovery and when reading rows from the master.  It is
	 * presented with the most recent format of data.
	 * row_reader is responsible for converting data from old
	 * formats.
	 */
	row_handler *row_handler;
	void *row_handler_param;
	snapshot_handler *snapshot_handler;
	join_handler *join_handler;
	uint64_t snap_io_rate_limit;
	int rows_per_wal;
	enum wal_mode wal_mode;
	struct tt_uuid server_uuid;
	uint32_t server_id;

	bool finalize;
};

extern struct recovery_state *recovery;

void recovery_init(const char *snap_dirname, const char *xlog_dirname,
		   row_handler row_handler, void *row_handler_param,
		   snapshot_handler snapshot_handler, join_handler join_handler,
		   int rows_per_wal);
void recovery_update_mode(struct recovery_state *r, enum wal_mode mode);
void recovery_update_io_rate_limit(struct recovery_state *r,
				   double new_limit);
void recovery_free();

static inline bool
recovery_has_data(struct recovery_state *r)
{
	return vclockset_first(&r->snap_dir.index) != NULL ||
	       vclockset_first(&r->wal_dir.index) != NULL;
}
void recovery_bootstrap(struct recovery_state *r);
void recover_snap(struct recovery_state *r);
void recovery_follow_local(struct recovery_state *r, ev_tstamp wal_dir_rescan_delay);
void recovery_finalize(struct recovery_state *r);

int recover_wal(struct recovery_state *r, struct log_io *l);
void wal_writer_destroy(struct wal_writer *writer);
int wal_write_lsn(struct recovery_state *r, struct xrow_header *packet);
int wal_write(struct wal_writer *writer, struct wal_write_request *req);
void wal_writer_stop(struct recovery_state *r);

void recovery_setup_panic(struct recovery_state *r, bool on_snap_error, bool on_wal_error);
void recovery_process(struct recovery_state *r, struct xrow_header *packet);

struct fio_batch;

void
snapshot_write_row(struct log_io *l, struct xrow_header *packet);
void snapshot_save(struct recovery_state *r);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_RECOVERY_H_INCLUDED */
