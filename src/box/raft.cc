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
#define MH_SOURCE 1
#include "raft.h"

#include <list>
#include <iterator>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "crc32.h"
#include "cfg.h"
#include "fio.h"
#include "coio.h"
#include "scoped_guard.h"
#include "msgpuck/msgpuck.h"
#include "box.h"
#include "request.h"
#include "port.h"
#include "raft_common.h"
#include "raft_session.h"
#include "box/log_io.h"
#include "coio_buf.h"

/** REWRITED based on C-style tarantool internal modules sync master-master */
static void* bsync_thread(void*);
static struct cord bsync_cord;
static struct coio_service bsync_coio;

static struct wal_writer* wal_local_writer = NULL;
static struct wal_writer proxy_wal_writer;
static pthread_once_t raft_writer_once = PTHREAD_ONCE_INIT;
static ev_async local_write_event;

static std::list<xrow_header> raft_local_input;
static std::list<xrow_header> raft_local_commit;
static std::list<fiber*> raft_local_f;

/** WAL writer thread routine. */
static void* raft_writer_thread(void*);
static void raft_writer_push(uint64_t gsn, bool result);
static void raft_read_cfg();
static void raft_init_state(const struct vclock* vclock);
static void raft_proceed_queue();
static uint32_t initial_op_crc;


/**
* A pthread_atfork() callback for a child process. Today we only
* fork the master process to save a snapshot, and in the child
* the WAL writer thread is not necessary and not present.
*/
static void
raft_writer_child()
{
  log_io_atfork(&recovery->current_wal);
  if (proxy_wal_writer.batch) {
    free(proxy_wal_writer.batch);
    proxy_wal_writer.batch = NULL;
  }
  /*
   * Make sure that atexit() handlers in the child do
   * not try to stop the non-existent thread.
   * The writer is not used in the child.
   */
  recovery->writer = NULL;
}

/**
* Today a WAL writer is started once at start of the
* server.  Nevertheless, use pthread_once() to make
* sure we can start/stop the writer many times.
*/
static void
raft_writer_init_once()
{
  (void) tt_pthread_atfork(NULL, NULL, raft_writer_child);
}

/**
* A commit watcher callback is invoked whenever there
* are requests in wal_writer->commit. This callback is
* associated with an internal WAL writer watcher and is
* invoked in the front-end main event loop.
*
* A rollback watcher callback is invoked only when there is
* a rollback request and commit is empty.
* We roll back the entire input queue.
*
* ev_async, under the hood, is a simple pipe. The WAL
* writer thread writes to that pipe whenever it's done
* handling a pack of requests (look for ev_async_send()
* call in the writer thread loop).
*/
static void
raft_schedule_queue(struct wal_fifo *queue)
{
  /*
   * Can't use STAILQ_FOREACH since fiber_call()
   * destroys the list entry.
   */
  struct wal_write_request *req, *tmp;
  STAILQ_FOREACH_SAFE(req, queue, wal_fifo_entry, tmp)
    fiber_call(req->fiber);
}

static void
raft_schedule(ev_loop * /* loop */, ev_async *watcher, int /* event */)
{
  struct wal_writer *writer = (struct wal_writer *) watcher->data;
  struct wal_fifo commit = STAILQ_HEAD_INITIALIZER(commit);
  struct wal_fifo rollback = STAILQ_HEAD_INITIALIZER(rollback);

  (void) tt_pthread_mutex_lock(&writer->mutex);
  STAILQ_CONCAT(&commit, &writer->commit);
  if (writer->is_rollback) {
    STAILQ_CONCAT(&rollback, &writer->input);
    writer->is_rollback = false;
  }
  (void) tt_pthread_mutex_unlock(&writer->mutex);
  raft_schedule_queue(&commit);
  /*
   * Perform a cascading abort of all transactions which
   * depend on the transaction which failed to get written
   * to the write ahead log. Abort transactions
   * in reverse order, performing a playback of the
   * in-memory database state.
   */
  STAILQ_REVERSE(&rollback, wal_write_request, wal_fifo_entry);
  raft_schedule_queue(&rollback);
}

static void raft_local_write(ev_loop * /* loop */, ev_async * /* watcher */, int /* event */) {
  (void) tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  raft_local_commit.splice(raft_local_commit.end(), raft_local_input);
  (void) tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
  if (raft_local_commit.empty()) return;
  while(!raft_local_f.empty() && !raft_local_commit.empty()) {
    fiber_call(raft_local_f.front());
    raft_local_f.pop_front();
  }
}

static void raft_local_write_fiber(va_list /* ap */) {
  while (true) {
    while (!raft_local_commit.empty()) {
      xrow_header row = raft_local_commit.front();
      raft_local_commit.pop_front();
      struct request req;
      request_create(&req, row.type);
      request_decode(&req, (const char*)row.body[0].iov_base, row.body[0].iov_len);
      req.header = &row;
      box_process(&null_port, &req);
      for (int i = 0; i < row.bodycnt; ++i) {
        free(row.body[i].iov_base);
      }
    }
    raft_local_f.push_back(fiber());
    fiber_yield();
  }
}

wal_writer* raft_init(wal_writer* initial, struct vclock *vclock) {
  assert (initial != NULL);
  if (cfg_geti("enable_raft") <= 0) {
    say_info("enable_raft=%d\n", cfg_geti("enable_raft"));
    return initial;
  }
  wal_local_writer = initial;

  assert(! proxy_wal_writer.is_shutdown);
  assert(STAILQ_EMPTY(&proxy_wal_writer.input));
  assert(STAILQ_EMPTY(&proxy_wal_writer.commit));

  /* I. Initialize the state. */
  pthread_mutexattr_t errorcheck;

  (void) tt_pthread_mutexattr_init(&errorcheck);

#ifndef NDEBUG
  (void) tt_pthread_mutexattr_settype(&errorcheck, PTHREAD_MUTEX_ERRORCHECK);
#endif
  /* Initialize queue lock mutex. */
  (void) tt_pthread_mutex_init(&proxy_wal_writer.mutex, &errorcheck);
  (void) tt_pthread_mutexattr_destroy(&errorcheck);

  (void) tt_pthread_cond_init(&proxy_wal_writer.cond, NULL);

  STAILQ_INIT(&proxy_wal_writer.input);
  STAILQ_INIT(&proxy_wal_writer.commit);

  ev_async_init(&proxy_wal_writer.write_event, raft_schedule);
  ev_async_init(&local_write_event, raft_local_write);
  proxy_wal_writer.write_event.data = &proxy_wal_writer;
  proxy_wal_writer.txn_loop = loop();

  (void) tt_pthread_once(&raft_writer_once, raft_writer_init_once);

  proxy_wal_writer.batch = fio_batch_alloc(sysconf(_SC_IOV_MAX));

  if (proxy_wal_writer.batch == NULL)
    panic_syserror("fio_batch_alloc");

  /* Create and fill writer->cluster hash */
  vclock_create(&proxy_wal_writer.vclock);
  vclock_copy(&proxy_wal_writer.vclock, vclock);

  ev_async_start(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
  ev_async_start(proxy_wal_writer.txn_loop, &local_write_event);

  /* II. Start the thread. */
  raft_read_cfg();
  RAFT_LOCAL_DATA.last_op_crc = initial_op_crc;
  for (auto i = 0; i < raft_state.fiber_num; ++i) {
    raft_local_f.push_back(fiber_new("raft local dump", raft_local_write_fiber));
  }
  raft_init_state(vclock);
  if (cord_start(&bsync_cord, "bsync", bsync_thread, NULL)) {
    say_error("cant start bsync thread");
    if (cord_start(&proxy_wal_writer.cord, "raft", raft_writer_thread, NULL)) {
      wal_writer_destroy(&proxy_wal_writer);
      return 0;
    }
  } else {
		wal_local_writer = NULL;
		return initial;
  }
  return &proxy_wal_writer;
}

int raft_write(struct recovery_state *r, struct xrow_header *row) {
  if (row && row->server_id == RAFT_SERVER_ID)
  {
    if (raft_state.local_id < 0) {
      initial_op_crc = crc32_calc(initial_op_crc, (const char*)row->body[0].iov_base, row->body[0].iov_len);
    } else {
      RAFT_LOCAL_DATA.last_op_crc = crc32_calc(RAFT_LOCAL_DATA.last_op_crc, (const char*)row->body[0].iov_base, row->body[0].iov_len);
    }
  }
  if (wal_local_writer == NULL) {
    return wal_write_lsn(r, row);
  }
  /* try to sync transaction with other hosts, call wal_write and return result */
  struct wal_writer *writer = r->writer;

  struct wal_write_request *req = (struct wal_write_request *)
    region_alloc(&fiber()->gc, sizeof(struct wal_write_request));
  req->fiber = fiber();
  req->res = -1;
  req->row = row;
  row->tm = ev_now(loop());
  row->sync = 0;
  if (row->server_id == RAFT_SERVER_ID && raft_state.leader_id != raft_state.local_id) {
    req->res = wal_write(wal_local_writer, req);
    if (req->res < 0) {
      raft_state.io_service.post(boost::bind(&raft_writer_push, row->lsn, false));
      return -1;
    } else {
      raft_state.io_service.post(boost::bind(&raft_writer_push, row->lsn, true));
      return 0;
    }
  } else {
    if (row->server_id != RAFT_SERVER_ID) row->server_id = raft_state.local_id;
    (void) tt_pthread_mutex_lock(&writer->mutex);
    bool input_was_empty = STAILQ_EMPTY(&writer->input);
    STAILQ_INSERT_TAIL(&writer->input, req, wal_fifo_entry);

    if (input_was_empty) {
      raft_state.io_service.post(&raft_proceed_queue);
    }

    (void) tt_pthread_mutex_unlock(&writer->mutex);

    fiber_yield(); /* Request was inserted. */

    /* req->res is -1 on error */
    if (req->res < 0)
      return -1; /* error */

    if (raft_state.leader_id == raft_state.local_id) {
      return wal_write(wal_local_writer, req); /* success, send to local wal writer */
    } else {
      if (wal_write(wal_local_writer, req) < 0) {
        raft_state.io_service.post(boost::bind(&raft_writer_push, row->lsn, false));
        return -1;
      } else {
        raft_state.io_service.post(boost::bind(&raft_writer_push, row->lsn, true));
      }
      fiber_yield(); /* Request was inserted. */
      return req->res;
    }
  }
}

static boost::posix_time::time_duration raft_get_timeout(const char* name, int def) {
  int v = cfg_geti(name);
  if (v < 1) v = def;
  if (v < 1) {
    return boost::posix_time::not_a_date_time;
  } else {
    return boost::posix_time::milliseconds(v);
  }
}

static void raft_init_state(const struct vclock* vclock) {
  RAFT_LOCAL_DATA.gsn = vclock->lsn[RAFT_SERVER_ID] == -1 ? 0 : vclock->lsn[RAFT_SERVER_ID];
}

static void raft_election_timeout(std::shared_ptr<boost::asio::deadline_timer> /*timer*/, boost::system::error_code ec) {
  if (ec == boost::asio::error::operation_aborted) return;
  if (raft_state.state == raft_state_started) {
    raft_state.state = raft_state_initial;
    raft_leader_promise();
  }
}

static void raft_read_cfg() {
  raft_state.read_timeout = raft_get_timeout("raft_read_timeout", 3100);
  raft_state.write_timeout = raft_get_timeout("raft_write_timeout", 3100);
  raft_state.connect_timeout = raft_get_timeout("raft_connect_timeout", 3100);
  raft_state.resolve_timeout = raft_get_timeout("raft_resolve_timeout", 3100);
  raft_state.reconnect_timeout = raft_get_timeout("raft_reconnect_timeout", 3100);
  raft_state.operation_timeout = raft_get_timeout("raft_operation_timeout", 3500);
  raft_state.fiber_num = std::max((int)raft_state.fiber_num, cfg_geti("raft_fiber_num"));
  raft_state.host_queue_len = std::max((int)raft_state.host_queue_len, cfg_geti("raft_queue_len"));
  auto election_timeout = raft_get_timeout("raft_election_timeout", -1);
  if (!election_timeout.is_special()) {
    raft_state.start_election_time += election_timeout;
  }
  const char* hosts = cfg_gets("raft_replica");
  if (hosts == NULL) {
    tnt_raise(ClientError, ER_CFG, "raft replica: expected host:port[;host_port]*");
  }
  std::string hosts_str(hosts);
  auto i_host_begin = hosts_str.begin();
  auto i_host_end = hosts_str.end();
  std::map<std::string, raft_host_data> local_buff;
  while (i_host_begin != hosts_str.end()) {
    i_host_end = std::find(i_host_begin, hosts_str.end(), ';');
    auto i_url = std::find(i_host_begin, i_host_end, ':');
    raft_host_data nhost;
    nhost.host.assign(i_host_begin, i_url);
    try {
      nhost.port = boost::lexical_cast<unsigned short>(std::string(i_url + 1, i_host_end));
    } catch (...) {
      tnt_raise(ClientError, ER_CFG, "raft replica: invalid port in raft url");
    }
    nhost.full_name.assign(i_host_begin, i_host_end);
    local_buff.emplace(nhost.full_name, std::move(nhost));
    i_host_begin = (i_host_end != hosts_str.end() ? i_host_end + 1 : i_host_end);
  }
  const char* localhost = cfg_gets("raft_local");
  if (localhost == NULL) {
    tnt_raise(ClientError, ER_CFG, "raft replica: raft_local param not found");
  }
  auto i_local = local_buff.find(localhost);
  if (i_local == local_buff.end()) {
    tnt_raise(ClientError, ER_CFG, "raft replica: raft_local param contains unknown host");
  }
  i_local->second.local = true;
  uint8_t host_id = 0;
  raft_host_index.reserve(local_buff.size());
  for (auto& i : local_buff) {
    i.second.id = host_id++;
    raft_host_index.emplace_back(std::move(i.second));
    if (i.second.local) {
      raft_state.local_id = i.second.id;
    }
  }
  if (!election_timeout.is_special()) {
    auto timer = std::make_shared<boost::asio::deadline_timer>(raft_state.io_service);
    timer->expires_from_now(election_timeout);
    timer->async_wait(boost::bind(&raft_election_timeout, timer, _1));
  }
}

class raft_server {
public:
  raft_server(const tcp::endpoint& endpoint)
    : acceptor_(raft_state.io_service, endpoint), socket_(raft_state.io_service)
  {
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept(socket_,
      [this](boost::system::error_code ec) {
        if (!ec) {
          new raft_session(std::move(socket_));
        }
        do_accept();
      });
  }

  tcp::acceptor acceptor_;
  tcp::socket socket_;
};

static void* raft_writer_thread(void*) {
  return NULL;
  for (auto& host : raft_host_index) {
    if (!host.local) {
      host.out_session.reset(new raft_session(host.id));
    }
  }
  raft_state.max_connected_id = raft_state.local_id;
  raft_host_index[raft_state.local_id].gsn = RAFT_LOCAL_DATA.gsn;
  raft_state.host_state.insert(raft_host_state({(uint32_t)raft_state.local_id}));
  assert(raft_state.local_id >= 0);
  // start to listen port
  raft_server acceptor(tcp::endpoint(
    boost::asio::ip::tcp::v4(),
    raft_host_index[raft_state.local_id].port
  ));
  // start to make sessions to other hosts
  while (!proxy_wal_writer.is_shutdown) {
#ifdef NDEBUG
    try {
      raft_state.io_service.run();
    } catch (const std::exception& e) {
      say_error("unhandled std::exception from network, what='%s'", e.what());
    } catch (const Exception& e) {
      say_error("unhandled tarantool exception from network, what='%s'", e.errmsg());
    } catch (const boost::system::error_code& e) {
      say_error("unhandled boost::system::error_code exception from network, code=%d, what='%s'",
        e.value(), e.message().c_str());
    } catch (...) {
      say_error("unhandled unknown exception from network");
    }
#else
    raft_state.io_service.run();
#endif
  }
  return NULL;
}

void raft_write_wal_remote(uint64_t gsn, uint32_t server_id) {
  raft_host_index[server_id].buffer.server_id = RAFT_SERVER_ID;
  raft_host_index[server_id].buffer.lsn = gsn;
  tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  raft_local_input.push_back(raft_host_index[server_id].buffer);
  ev_async_send(proxy_wal_writer.txn_loop, &local_write_event);
  tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
}

void raft_write_wal_local_leader(const raft_local_state::operation& op) {
  op.req->res = 0;
  op.req->row->lsn = op.gsn;
  op.req->row->server_id = RAFT_SERVER_ID;
  op.timeout->cancel();
  tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  STAILQ_INSERT_TAIL(&proxy_wal_writer.commit, op.req, wal_fifo_entry);
  ev_async_send(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
  tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
}

void raft_write_wal_local_slave(uint64_t lsn, uint64_t gsn) {
  auto i_req = raft_state.local_operation_index.find(lsn);
  assert(i_req != raft_state.local_operation_index.end());
  i_req->second->res = 0;
  i_req->second->row->lsn = gsn;
  i_req->second->row->server_id = RAFT_SERVER_ID;
  tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  STAILQ_INSERT_TAIL(&proxy_wal_writer.commit, i_req->second, wal_fifo_entry);
  ev_async_send(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
  tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
}

void raft_rollback_local(uint64_t gsn) {
  struct wal_fifo rollback = STAILQ_HEAD_INITIALIZER(rollback);
  auto& index = raft_state.operation_index.get<raft_local_state::gsn_hash>();
  for (auto i_op = index.find(gsn); i_op != index.end(); ) {
    i_op->req->res = -1;
    STAILQ_INSERT_HEAD(&rollback, i_op->req, wal_fifo_entry);
    i_op->timeout->cancel();
    i_op = index.erase(i_op);
  }
  tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  proxy_wal_writer.is_rollback = true;
  STAILQ_CONCAT(&proxy_wal_writer.input, &rollback);
  ev_async_send(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
  tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
}

static void raft_operation_timeout(uint64_t gsn, boost::system::error_code ec) {
  if (ec == boost::asio::error::operation_aborted) return;
  auto& index = raft_state.operation_index.get<raft_local_state::gsn_hash>();
  auto i_op = index.find(gsn);
  assert(i_op != index.end());
  for (raft_host_data& host : raft_host_index) {
    if (!host.local) host.out_session->send(raft_mtype_reject, gsn);
  }
  raft_rollback_local(gsn);
}

static void raft_recover_wal(struct log_io* l, struct vclock *res, int64_t gsn, uint32_t* crc) {
  struct log_io_cursor i;
  log_io_cursor_open(&i, l);
  struct xrow_header row;
  while (log_io_cursor_next(&i, &row) == 0) {
    if (row.server_id == RAFT_SERVER_ID) {
      *crc = crc32_calc(*crc, (const char*)row.body[0].iov_base, row.body[0].iov_len);
    }
    res->lsn[row.server_id] = row.lsn;
    if (row.server_id != RAFT_SERVER_ID || row.lsn < gsn) continue;
    raft_msg_body msg = { row.lsn, &row };
    for (auto &host : raft_host_index) {
      if (!host.local && host.connected == 2 && raft_state.hosts_for_recover.test(host.id)) {
        if (host.gsn < row.lsn) {
          host.out_session->send(raft_mtype_body, msg);
        } else if (host.gsn == row.lsn && host.last_op_crc != *crc) {
          host.out_session->fault(boost::system::error_code());
          host.in_session->fault(boost::system::error_code());
        }
      }
    }
  }
  if (!i.eof_read && !l->is_inprogress)
    panic("can't read full snapshot");
  log_io_cursor_close(&i);
}

void raft_recover_node(int64_t gsn) {
  uint32_t crc = 0;
  struct log_dir snap_dir, wal_dir;
  log_dir_create(&snap_dir, cfg_gets("snap_dir"), SNAP);
  log_dir_create(&wal_dir, cfg_gets("wal_dir"), XLOG);
  if (log_dir_scan(&snap_dir) != 0)
    panic("can't scan snapshot directory");
  if (log_dir_scan(&wal_dir) != 0)
    panic("can't scan WAL directory");
  struct vclock *res = vclockset_last(&snap_dir.index);
  if (res == NULL)
    panic("can't find snapshot");
  int64_t sign = vclock_signature(res);
  struct log_io* snap = log_io_open_for_read(&snap_dir, sign, NULL, NONE);
  struct log_io* wal = NULL;
  if (snap == NULL)
    panic("can't open snapshot");
  raft_recover_wal(snap, res, gsn, &crc);
  while (1) {
    res = vclockset_isearch(&wal_dir.index, res);
    if (res == NULL)
      break; /* No more WALs */

    sign = vclock_signature(res);
    wal = log_io_open_for_read(&wal_dir, sign, &snap->server_uuid, NONE);
    if (wal == NULL) {
      wal = log_io_open_for_read(&wal_dir, sign, &snap->server_uuid, INPROGRESS);
      if (wal == NULL)
        break;
    }
    say_info("recover from `%s'", wal->filename);
    raft_recover_wal(wal, res, gsn, &crc);
    log_io_close(&wal);
  }
  log_io_close(&snap);
}

static void raft_proceed_queue() {
  if (raft_state.state == raft_state_started) return;
/*  if (raft_state.leader_id != raft_state.local_id) {
    tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
    proxy_wal_writer.is_rollback = true;
    ev_async_send(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
    tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
    return;
  }*/
  struct wal_fifo input = STAILQ_HEAD_INITIALIZER(input);
  struct wal_fifo rollback = STAILQ_HEAD_INITIALIZER(rollback);
  tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  STAILQ_CONCAT(&input, &proxy_wal_writer.input);
  tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
  if (STAILQ_EMPTY(&input)) {
    return;
  }
  wal_write_request* wreq = STAILQ_FIRST(&input);
  if (raft_state.leader_id == raft_state.local_id) {
    while (wreq) {
      say_debug("raft_proceed_queue [%tX] %ld", (ptrdiff_t)wreq, pthread_self());
      auto key = boost::make_iterator_range(
          (const uint8_t*)wreq->row->body[0].iov_base,
          (const uint8_t*)wreq->row->body[0].iov_base + wreq->row->body[0].iov_len);
      auto& key_index = raft_state.operation_index.get<raft_local_state::key_hash>();
      if (key_index.find(key) != key_index.end()) {
        break;
      }
      raft_local_state::operation op(raft_state.io_service);
      if (wreq->row->server_id == RAFT_SERVER_ID) {
        auto i_proxy = raft_state.proxy_requests.find(wreq->row->lsn);
        op.gsn = i_proxy->first;
        wreq->row->server_id = op.server_id = i_proxy->second.first;
        wreq->row->lsn = op.lsn = i_proxy->second.second;
      } else {
        op.gsn = ++RAFT_LOCAL_DATA.gsn;
        op.server_id = raft_state.local_id;
        op.lsn = ++raft_state.lsn;
      }
      op.key = key;
      op.submitted = 1;
      op.rejected = 0;
      op.req = wreq;
      op.timeout->expires_from_now(raft_state.operation_timeout);
      op.timeout->async_wait(boost::bind(&raft_operation_timeout, op.gsn, _1));
      raft_state.operation_index.insert(op);
      raft_msg_body msg = {op.gsn, op.req->row };
      wreq = STAILQ_NEXT(wreq, wal_fifo_entry);
      for (auto &host : raft_host_index) {
        if (!host.local && host.connected == 2) {
          if (host.active_ops.size() == raft_state.host_queue_len) {
            // slow host found, disconnect
          }
          host.active_ops.push_back(op.gsn);
          host.out_session->send(raft_mtype_body, msg);
        }
      }
    }
    STAILQ_SPLICE(&input, wreq, wal_fifo_entry, &rollback);
  } else {
    while (wreq) {
      raft_state.local_operation_index.emplace(++raft_state.lsn, wreq);
      raft_host_index[raft_state.leader_id].out_session->send(raft_mtype_proxy_request, raft_msg_body({0, wreq->row}));
      wreq = STAILQ_NEXT(wreq, wal_fifo_entry);
    }
  }
  if (STAILQ_EMPTY(&rollback)) {
    return;
  }
  tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
  proxy_wal_writer.is_rollback = true;
  STAILQ_CONCAT(&proxy_wal_writer.input, &rollback);
  ev_async_send(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
  tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
}

void raft_leader_promise() {
  say_info("leader status: num_connected=%d, state=%d, local_id=%d, max_id=%d",
      (int)raft_state.num_connected, (int)raft_state.state, (int)raft_state.local_id, (int)raft_state.max_connected_id
  );
  if (has_consensus() && raft_state.local_id == raft_state.max_connected_id &&
      (raft_state.state == raft_state_initial || raft_state.state == raft_state_started))
  {
    if (raft_state.state == raft_state_started && raft_state.num_connected < raft_host_index.size() &&
        (raft_state.start_election_time > boost::posix_time::microsec_clock::universal_time())) {
      say_info("wait election timeout");
      return;
    }
    raft_state.state = raft_state_leader_accept;
    say_info("raft state changed to leader_accept, possible leader are '%s'",
        raft_host_index[raft_state.max_connected_id].full_name.c_str());
    raft_state.num_leader_accept = 1;
    for (auto& host : raft_host_index) {
      if (!host.local && host.connected == 2)
        host.out_session->send(raft_mtype_leader_promise, RAFT_LOCAL_DATA.gsn);
    }
  }
}

static void raft_writer_push(uint64_t gsn, bool result) {
  raft_host_index[raft_state.leader_id].out_session->send((result ? raft_mtype_submit : raft_mtype_reject), gsn);
}

void raft_writer_stop(struct recovery_state *r) {
  if (wal_local_writer != NULL) {
    (void) tt_pthread_mutex_lock(&proxy_wal_writer.mutex);
    proxy_wal_writer.is_shutdown= true;
    (void) tt_pthread_cond_signal(&proxy_wal_writer.cond);
    raft_state.io_service.stop();
    (void) tt_pthread_mutex_unlock(&proxy_wal_writer.mutex);
    if (cord_join(&proxy_wal_writer.cord)) {
      /* We can't recover from this in any reasonable way. */
      panic_syserror("RAFT writer: thread join failed");
    }
    ev_async_stop(proxy_wal_writer.txn_loop, &proxy_wal_writer.write_event);
    ev_async_stop(proxy_wal_writer.txn_loop, &local_write_event);
    wal_writer_destroy(&proxy_wal_writer);
    r->writer = wal_local_writer;
    wal_local_writer = NULL;
  }
  wal_writer_stop(r);
}

/* ******************************************************************************** */

#define BSYNC_SYSBUFFER_SIZE 64
#define BSYNC_MAX_HOSTS 16

struct bsync_host_data {
	uint8_t connected;
	uint64_t gsn;
	struct fiber* fiber_out;
	struct fiber* fiber_in;
	struct io_buf* buffer;
	const void* buffer_out;
	ssize_t buffer_out_size;
};
static struct bsync_host_data bsync_index[BSYNC_MAX_HOSTS];

static struct bsync_state_ {
	uint8_t local_id;
	uint8_t leader_id;
	uint64_t lsn;
	uint8_t num_hosts;
	uint8_t num_connected;
	uint8_t state;
	uint8_t num_accepted;

	ev_tstamp connect_timeout;
	ev_tstamp read_timeout;
	ev_tstamp write_timeout;
	ev_tstamp reconnect_timeout;
} bsync_state;

#define BSYNC_TRACE say_debug("%s:%d current state: nconnected=%d, state=%d, naccepted=%d\n", \
	__PRETTY_FUNCTION__, __LINE__, bsync_state.num_connected, bsync_state.state, bsync_state.num_accepted);

#define BSYNC_LOCAL bsync_index[bsync_state.local_id]

static char bsync_system_out[BSYNC_SYSBUFFER_SIZE];
static char bsync_system_in[BSYNC_SYSBUFFER_SIZE];

static const char* bsync_mtype_name[] = {
	"bsync_hello",
	"bsync_promise",
	"bsync_leader_accept",
	"bsync_leader_submit",
	"bsync_leader_reject",
	"bsync_body",
	"bsync_submit",
	"bsync_reject",
	"bsync_proxy_request",
	"bsync_proxy_submit",
	"bsync_ping"
};

static uint64_t
bsync_max_host()
{BSYNC_TRACE
	uint8_t max_host_id = 0;
	for (uint8_t i = 1; i < bsync_state.num_hosts; ++i) {
		if (bsync_index[i].gsn >= bsync_index[max_host_id].gsn &&
			bsync_index[i].connected == 2)
		{
			max_host_id = i;
		}
	}
	return max_host_id;
}

static void
bsync_connected(uint8_t host_id)
{BSYNC_TRACE
	if (bsync_state.leader_id == bsync_state.local_id) {
		char* pos = bsync_system_out;
		pos = mp_encode_uint(pos, mp_sizeof_uint(raft_mtype_leader_submit));
		pos = mp_encode_uint(pos, raft_mtype_leader_submit);
		bsync_index[host_id].buffer_out = bsync_system_out;
		bsync_index[host_id].buffer_out_size = pos - bsync_system_out;
		if (bsync_index[host_id].fiber_out != fiber()) {
			fiber_call(bsync_index[host_id].fiber_out);
		}
		return;
	}
	if (2 * bsync_state.num_connected <= bsync_state.num_hosts ||
		bsync_state.state > raft_state_initial)
	{
		return;
	}
	if (bsync_state.state > raft_state_initial) return;
	uint8_t max_host_id = bsync_max_host();
	if (max_host_id != bsync_state.local_id) return;
	bsync_state.num_accepted = 1;
	bsync_state.state = raft_state_leader_accept;
	ssize_t msize = mp_sizeof_uint(BSYNC_LOCAL.gsn) +
		mp_sizeof_uint(raft_mtype_leader_promise);
	char* pos = bsync_system_out;
	pos = mp_encode_uint(pos, msize);
	pos = mp_encode_uint(pos, raft_mtype_leader_promise);
	pos = mp_encode_uint(pos, BSYNC_LOCAL.gsn);
	for (uint8_t i = 0; i < bsync_state.num_hosts; ++i) {
		if (i == max_host_id) continue;
		bsync_index[i].buffer_out = bsync_system_out;
		bsync_index[i].buffer_out_size = pos - bsync_system_out;
		if (bsync_index[i].fiber_out != fiber()) {
			fiber_call(bsync_index[i].fiber_out);
		}
	}
}

static void
bsync_disconnected(uint8_t host_id) {
	--bsync_state.num_connected;
	if (2 * bsync_state.num_connected <= bsync_state.num_hosts ||
		host_id == bsync_state.leader_id)
	{
		bsync_state.leader_id = -1;
		bsync_state.state = raft_state_initial;
		bsync_connected(host_id);
	}
}

static void
bsync_leader_promise(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
	bsync_index[host_id].gsn = mp_decode_uint(ipos);
	char* pos = bsync_system_out;
	uint8_t max_host_id = bsync_max_host();
	if (host_id != max_host_id || bsync_state.state > raft_state_initial) {
		ssize_t msize = mp_sizeof_uint(raft_mtype_leader_reject) +
			mp_sizeof_uint(max_host_id) +
			mp_sizeof_uint(bsync_index[max_host_id].gsn);
		pos = mp_encode_uint(pos, msize);
		pos = mp_encode_uint(pos, raft_mtype_leader_reject);
		pos = mp_encode_uint(pos, max_host_id);
		pos = mp_encode_uint(pos, bsync_index[max_host_id].gsn);
	} else {
		ssize_t msize = mp_sizeof_uint(raft_mtype_leader_accept);
		pos = mp_encode_uint(pos, msize);
		pos = mp_encode_uint(pos, raft_mtype_leader_accept);
		bsync_state.state = raft_state_leader_accept;
	}
	bsync_index[host_id].buffer_out = bsync_system_out;
	bsync_index[host_id].buffer_out_size = pos - bsync_system_out;
	fiber_call(bsync_index[host_id].fiber_out);
}

static void
bsync_leader_accept(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
	if (bsync_state.state != raft_state_leader_accept) return;
	if (2 * ++bsync_state.num_accepted <= bsync_state.num_hosts) return;
	say_info("new leader are %s", raft_host_index[bsync_state.local_id].full_name.c_str());
	bsync_state.state = raft_state_ready;
	bsync_state.leader_id = bsync_state.local_id;
	char* pos = bsync_system_out;
	pos = mp_encode_uint(pos, mp_sizeof_uint(raft_mtype_leader_submit));
	pos = mp_encode_uint(pos, raft_mtype_leader_submit);
	for (uint8_t i = 0; i < bsync_state.num_hosts; ++i) {
		if (i == bsync_state.local_id) continue;
		bsync_index[i].buffer_out = bsync_system_out;
		bsync_index[i].buffer_out_size = pos - bsync_system_out;
		fiber_call(bsync_index[i].fiber_out);
	}
}

static void
bsync_leader_submit(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
	bsync_state.leader_id = host_id;
	bsync_state.state = raft_state_ready;
	say_info("new leader are %s", raft_host_index[host_id].full_name.c_str());
}

static void
bsync_leader_reject(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
	uint8_t max_id = mp_decode_uint(ipos);
	bsync_index[max_id].gsn = mp_decode_uint(ipos);
	bsync_connected(host_id);
}

static void
bsync_body(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
}

static void
bsync_submit(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
}

static void
bsync_reject(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
}

static void
bsync_proxy_request(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
}

static void
bsync_proxy_submit(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
}

static void
bsync_ping(uint8_t host_id, const char** ipos)
{BSYNC_TRACE
	(void)host_id; (void)ipos;
}

typedef void (*bsync_handler_t)(uint8_t host_id, const char** ipos);
static bsync_handler_t bsync_handlers[] = {
	0,
	bsync_leader_promise,
	bsync_leader_accept,
	bsync_leader_submit,
	bsync_leader_reject,
	bsync_body,
	bsync_submit,
	bsync_reject,
	bsync_proxy_request,
	bsync_proxy_submit,
	bsync_ping
};

static void
bsync_incoming(struct ev_io* coio, struct iobuf* iobuf, uint8_t host_id) {
	auto coio_guard = make_scoped_guard([&]() {
		iobuf_delete(iobuf);
	});
	struct ibuf *in = &iobuf->in;
	while (true) {
		/* Read fixed header */
		if (ibuf_size(in) < 1)
			coio_breadn(coio, in, 1);
		/* Read length */
		if (mp_typeof(*in->pos) != MP_UINT) {
			tnt_raise(ClientError, ER_INVALID_MSGPACK,
				  "packet length");
		}
		ssize_t to_read = mp_check_uint(in->pos, in->end);
		if (to_read > 0)
			coio_breadn(coio, in, to_read);
		uint32_t len = mp_decode_uint((const char **) &in->pos);
		/* Read header and body */
		to_read = len - ibuf_size(in);
		if (to_read > 0)
			coio_breadn(coio, in, to_read);
		/* proceed message */
		const char **ipos = (const char **)&in->pos;
		uint32_t type = mp_decode_uint(ipos);
		say_debug("receive message from %s, type %s, length %d",
			raft_host_index[host_id].full_name.c_str(),
			bsync_mtype_name[type], len);
		assert(type < sizeof(bsync_handlers));
		(*bsync_handlers[type])(host_id, ipos);
		/* cleanup buffer */
		iobuf_reset(iobuf);
		fiber_gc();
	}
}

static void
bsync_handler(va_list ap)
{BSYNC_TRACE
	struct ev_io coio = va_arg(ap, struct ev_io);
	struct sockaddr *addr = va_arg(ap, struct sockaddr *);
	socklen_t addrlen = va_arg(ap, socklen_t);
	struct iobuf *iobuf = va_arg(ap, struct iobuf *);
	(void)addr; (void)addrlen;
	coio_read_timeout(&coio, bsync_system_in, BSYNC_SYSBUFFER_SIZE,
			  bsync_state.read_timeout);
	const char* pos = bsync_system_in;
	uint64_t host_id = mp_decode_uint(&pos);
	assert(host_id < raft_host_index.size());
	bsync_index[host_id].gsn = mp_decode_uint(&pos);
	bsync_index[host_id].fiber_in = fiber();
	say_info("receive incoming connection from %s, gsn=%ld",
		 raft_host_index[host_id].full_name.c_str(),
		 bsync_index[host_id].gsn);
	if (++bsync_index[host_id].connected == 1) {
		fiber_call(bsync_index[host_id].fiber_out);
	}
	if (bsync_index[host_id].connected == 2) {
		++bsync_state.num_connected;
		bsync_connected(host_id);
	}
	try {
		bsync_incoming(&coio, iobuf, host_id);
	} catch(...) {
		if (--bsync_index[host_id].connected == 1) {BSYNC_TRACE
			bsync_disconnected(host_id);
			fiber_call(bsync_index[host_id].fiber_out);
		}
		throw;
	}
}

static void
bsync_out_fiber(va_list ap)
{BSYNC_TRACE
	uint8_t host_id = *va_arg(ap, uint8_t*);
	const char* uri = raft_host_index[host_id].full_name.c_str();
	bsync_index[host_id].fiber_out = fiber();
	say_info("send outgoing connection to %s", uri);
	struct remote remote;
	/* First, stop the reader, then set the source */
	snprintf(remote.source, sizeof(remote.source), "%s", uri);
	int rc = uri_parse(&remote.uri, remote.source);
	/* URI checked by box_check_replication_source() */
	assert(rc == 0 && remote.uri.service != NULL);
	(void) rc;
	remote.addr_len = sizeof(remote.addrstorage);
	char host[URI_MAXHOST] = { '\0' };
	if (remote.uri.host) {
		snprintf(host, sizeof(host), "%.*s", (int) remote.uri.host_len,
			remote.uri.host);
	}
	char service[URI_MAXSERVICE];
	snprintf(service, sizeof(service), "%.*s",
		(int) remote.uri.service_len, remote.uri.service);
	bool connected = false;
	while (true) try {BSYNC_TRACE
		connected = false;
		struct ev_io coio;
		coio_init(&coio);
		struct iobuf *iobuf = iobuf_new(service);
		auto coio_guard = make_scoped_guard([&] {
			evio_close(loop(), &coio);
			iobuf_delete(iobuf);
		});
		int r = coio_connect_timeout(&coio, host, service, &remote.addr,
				&remote.addr_len, bsync_state.connect_timeout,
				remote.uri.host_hint);
		if (r == -1) {
			say_warn("connection timeout to %s", uri);
			continue;
		}
		char* pos = bsync_system_out;
		pos = mp_encode_uint(pos, bsync_state.local_id);
		pos = mp_encode_uint(pos, BSYNC_LOCAL.gsn);
		coio_write_timeout(&coio, bsync_system_out, BSYNC_SYSBUFFER_SIZE,
				   bsync_state.write_timeout);
		connected = true;
		if (++bsync_index[host_id].connected == 2) {
			++bsync_state.num_connected;
			bsync_connected(host_id);
		}
		while(true) {BSYNC_TRACE
			if (bsync_index[host_id].buffer_out == NULL) {
				char* pos = bsync_system_out;
				pos = mp_encode_uint(pos, mp_sizeof_uint(raft_mtype_ping));
				pos = mp_encode_uint(pos, raft_mtype_ping);
				bsync_index[host_id].buffer_out = bsync_system_out;
				bsync_index[host_id].buffer_out_size = pos - bsync_system_out;
			}
			const void* buffer_out = bsync_index[host_id].buffer_out;
			ssize_t send_size = bsync_index[host_id].buffer_out_size;
			bsync_index[host_id].buffer_out = NULL;
			ssize_t ssize = coio_write_timeout(&coio, buffer_out,
				send_size, bsync_state.write_timeout);
			if (ssize < send_size) {BSYNC_TRACE
				tnt_raise(SocketError, coio.fd, "timeout");
			}
			BSYNC_TRACE
			fiber_yield_timeout(1); /* wait data for sending */
		}
	} catch (...) {BSYNC_TRACE
		if (connected && --bsync_index[host_id].connected == 1) {
			bsync_disconnected(host_id);
		}
		BSYNC_TRACE
		fiber_yield_timeout(bsync_state.reconnect_timeout);
		BSYNC_TRACE
	}
}

static void*
bsync_thread(void*)
{
	/* initialization */
	bsync_state.local_id = raft_state.local_id;
	bsync_state.leader_id = -1;
	bsync_state.connect_timeout = raft_state.connect_timeout.total_seconds();
	bsync_state.read_timeout = raft_state.read_timeout.total_seconds();
	bsync_state.write_timeout = raft_state.write_timeout.total_seconds();
	bsync_state.num_hosts = raft_host_index.size();
	bsync_state.num_connected = 1;
	bsync_state.state = raft_state_started;
	bsync_state.reconnect_timeout = raft_state.reconnect_timeout.total_seconds();
	BSYNC_LOCAL.gsn = raft_host_index[raft_state.local_id].gsn;
	BSYNC_LOCAL.connected = 2;

	coio_service_init(&bsync_coio, "bsync",
		raft_host_index[raft_state.local_id].full_name.c_str(),
		bsync_handler, NULL);
	evio_service_start(&bsync_coio.evio_service);
	for (auto& host : raft_host_index) {
		if (host.local) continue;
		bsync_index[host.id].connected = 0;
		fiber_call(
			fiber_new(host.full_name.c_str(), bsync_out_fiber),
			&host.id);
	}
	ev_run(loop(), 0);
	say_info("bsync stopped");
	return NULL;
}
