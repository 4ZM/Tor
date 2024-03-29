/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2011, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file connection_or.c
 * \brief Functions to handle OR connections, TLS handshaking, and
 * cells on the network.
 **/

#include "or.h"
#include "buffers.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "command.h"
#include "config.h"
#include "connection.h"
#include "connection_or.h"
#include "control.h"
#include "dirserv.h"
#include "geoip.h"
#include "main.h"
#include "networkstatus.h"
#include "nodelist.h"
#include "reasons.h"
#include "relay.h"
#include "rephist.h"
#include "router.h"
#include "routerlist.h"

#ifdef USE_BUFFEREVENTS
#include <event2/bufferevent_ssl.h>
#endif

static int connection_tls_finish_handshake(or_connection_t *conn);
static int connection_or_launch_v3_or_handshake(or_connection_t *conn);
static int connection_or_process_cells_from_inbuf(or_connection_t *conn);
static int connection_or_check_valid_tls_handshake(or_connection_t *conn,
                                                   int started_here,
                                                   char *digest_rcvd_out);

static void connection_or_tls_renegotiated_cb(tor_tls_t *tls, void *_conn);

#ifdef USE_BUFFEREVENTS
static void connection_or_handle_event_cb(struct bufferevent *bufev,
                                          short event, void *arg);
#include <event2/buffer.h>/*XXXX REMOVE */
#endif

/**************************************************************/

/** Map from identity digest of connected OR or desired OR to a connection_t
 * with that identity digest.  If there is more than one such connection_t,
 * they form a linked list, with next_with_same_id as the next pointer. */
static digestmap_t *orconn_identity_map = NULL;

/** If conn is listed in orconn_identity_map, remove it, and clear
 * conn->identity_digest.  Otherwise do nothing. */
void
connection_or_remove_from_identity_map(or_connection_t *conn)
{
  or_connection_t *tmp;
  tor_assert(conn);
  if (!orconn_identity_map)
    return;
  tmp = digestmap_get(orconn_identity_map, conn->identity_digest);
  if (!tmp) {
    if (!tor_digest_is_zero(conn->identity_digest)) {
      log_warn(LD_BUG, "Didn't find connection '%s' on identity map when "
               "trying to remove it.",
               conn->nickname ? conn->nickname : "NULL");
    }
    return;
  }
  if (conn == tmp) {
    if (conn->next_with_same_id)
      digestmap_set(orconn_identity_map, conn->identity_digest,
                    conn->next_with_same_id);
    else
      digestmap_remove(orconn_identity_map, conn->identity_digest);
  } else {
    while (tmp->next_with_same_id) {
      if (tmp->next_with_same_id == conn) {
        tmp->next_with_same_id = conn->next_with_same_id;
        break;
      }
      tmp = tmp->next_with_same_id;
    }
  }
  memset(conn->identity_digest, 0, DIGEST_LEN);
  conn->next_with_same_id = NULL;
}

/** Remove all entries from the identity-to-orconn map, and clear
 * all identities in OR conns.*/
void
connection_or_clear_identity_map(void)
{
  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type == CONN_TYPE_OR) {
      or_connection_t *or_conn = TO_OR_CONN(conn);
      memset(or_conn->identity_digest, 0, DIGEST_LEN);
      or_conn->next_with_same_id = NULL;
    }
  });

  digestmap_free(orconn_identity_map, NULL);
  orconn_identity_map = NULL;
}

/** Change conn->identity_digest to digest, and add conn into
 * orconn_digest_map. */
static void
connection_or_set_identity_digest(or_connection_t *conn, const char *digest)
{
  or_connection_t *tmp;
  tor_assert(conn);
  tor_assert(digest);

  if (!orconn_identity_map)
    orconn_identity_map = digestmap_new();
  if (tor_memeq(conn->identity_digest, digest, DIGEST_LEN))
    return;

  /* If the identity was set previously, remove the old mapping. */
  if (! tor_digest_is_zero(conn->identity_digest))
    connection_or_remove_from_identity_map(conn);

  memcpy(conn->identity_digest, digest, DIGEST_LEN);

  /* If we're setting the ID to zero, don't add a mapping. */
  if (tor_digest_is_zero(digest))
    return;

  tmp = digestmap_set(orconn_identity_map, digest, conn);
  conn->next_with_same_id = tmp;

#if 1
  /* Testing code to check for bugs in representation. */
  for (; tmp; tmp = tmp->next_with_same_id) {
    tor_assert(tor_memeq(tmp->identity_digest, digest, DIGEST_LEN));
    tor_assert(tmp != conn);
  }
#endif
}

/**************************************************************/

/** Map from a string describing what a non-open OR connection was doing when
 * failed, to an intptr_t describing the count of connections that failed that
 * way.  Note that the count is stored _as_ the pointer.
 */
static strmap_t *broken_connection_counts;

/** If true, do not record information in <b>broken_connection_counts</b>. */
static int disable_broken_connection_counts = 0;

/** Record that an OR connection failed in <b>state</b>. */
static void
note_broken_connection(const char *state)
{
  void *ptr;
  intptr_t val;
  if (disable_broken_connection_counts)
    return;

  if (!broken_connection_counts)
    broken_connection_counts = strmap_new();

  ptr = strmap_get(broken_connection_counts, state);
  val = (intptr_t)ptr;
  val++;
  ptr = (void*)val;
  strmap_set(broken_connection_counts, state, ptr);
}

/** Forget all recorded states for failed connections.  If
 * <b>stop_recording</b> is true, don't record any more. */
void
clear_broken_connection_map(int stop_recording)
{
  if (broken_connection_counts)
    strmap_free(broken_connection_counts, NULL);
  broken_connection_counts = NULL;
  if (stop_recording)
    disable_broken_connection_counts = 1;
}

/** Write a detailed description the state of <b>orconn</b> into the
 * <b>buflen</b>-byte buffer at <b>buf</b>.  This description includes not
 * only the OR-conn level state but also the TLS state.  It's useful for
 * diagnosing broken handshakes. */
static void
connection_or_get_state_description(or_connection_t *orconn,
                                    char *buf, size_t buflen)
{
  connection_t *conn = TO_CONN(orconn);
  const char *conn_state;
  char tls_state[256];

  tor_assert(conn->type == CONN_TYPE_OR);

  conn_state = conn_state_to_string(conn->type, conn->state);
  tor_tls_get_state_description(orconn->tls, tls_state, sizeof(tls_state));

  tor_snprintf(buf, buflen, "%s with SSL state %s", conn_state, tls_state);
}

/** Record the current state of <b>orconn</b> as the state of a broken
 * connection. */
static void
connection_or_note_state_when_broken(or_connection_t *orconn)
{
  char buf[256];
  if (disable_broken_connection_counts)
    return;
  connection_or_get_state_description(orconn, buf, sizeof(buf));
  log_info(LD_HANDSHAKE,"Connection died in state '%s'", buf);
  note_broken_connection(buf);
}

/** Helper type used to sort connection states and find the most frequent. */
typedef struct broken_state_count_t {
  intptr_t count;
  const char *state;
} broken_state_count_t;

/** Helper function used to sort broken_state_count_t by frequency. */
static int
broken_state_count_compare(const void **a_ptr, const void **b_ptr)
{
  const broken_state_count_t *a = *a_ptr, *b = *b_ptr;
  if (b->count < a->count)
    return -1;
  else if (b->count == a->count)
    return 0;
  else
    return 1;
}

/** Upper limit on the number of different states to report for connection
 * failure. */
#define MAX_REASONS_TO_REPORT 10

/** Report a list of the top states for failed OR connections at log level
 * <b>severity</b>, in log domain <b>domain</b>. */
void
connection_or_report_broken_states(int severity, int domain)
{
  int total = 0;
  smartlist_t *items;

  if (!broken_connection_counts || disable_broken_connection_counts)
    return;

  items = smartlist_create();
  STRMAP_FOREACH(broken_connection_counts, state, void *, countptr) {
    broken_state_count_t *c = tor_malloc(sizeof(broken_state_count_t));
    c->count = (intptr_t)countptr;
    total += (int)c->count;
    c->state = state;
    smartlist_add(items, c);
  } STRMAP_FOREACH_END;

  smartlist_sort(items, broken_state_count_compare);

  log(severity, domain, "%d connections have failed%s", total,
      smartlist_len(items) > MAX_REASONS_TO_REPORT ? ". Top reasons:" : ":");

  SMARTLIST_FOREACH_BEGIN(items, const broken_state_count_t *, c) {
    if (c_sl_idx > MAX_REASONS_TO_REPORT)
      break;
    log(severity, domain,
        " %d connections died in state %s", (int)c->count, c->state);
  } SMARTLIST_FOREACH_END(c);

  SMARTLIST_FOREACH(items, broken_state_count_t *, c, tor_free(c));
  smartlist_free(items);
}

/**************************************************************/

/** Pack the cell_t host-order structure <b>src</b> into network-order
 * in the buffer <b>dest</b>. See tor-spec.txt for details about the
 * wire format.
 *
 * Note that this function doesn't touch <b>dst</b>-\>next: the caller
 * should set it or clear it as appropriate.
 */
void
cell_pack(packed_cell_t *dst, const cell_t *src)
{
  char *dest = dst->body;
  set_uint16(dest, htons(src->circ_id));
  *(uint8_t*)(dest+2) = src->command;
  memcpy(dest+3, src->payload, CELL_PAYLOAD_SIZE);
}

/** Unpack the network-order buffer <b>src</b> into a host-order
 * cell_t structure <b>dest</b>.
 */
static void
cell_unpack(cell_t *dest, const char *src)
{
  dest->circ_id = ntohs(get_uint16(src));
  dest->command = *(uint8_t*)(src+2);
  memcpy(dest->payload, src+3, CELL_PAYLOAD_SIZE);
}

/** Write the header of <b>cell</b> into the first VAR_CELL_HEADER_SIZE
 * bytes of <b>hdr_out</b>. */
void
var_cell_pack_header(const var_cell_t *cell, char *hdr_out)
{
  set_uint16(hdr_out, htons(cell->circ_id));
  set_uint8(hdr_out+2, cell->command);
  set_uint16(hdr_out+3, htons(cell->payload_len));
}

/** Allocate and return a new var_cell_t with <b>payload_len</b> bytes of
 * payload space. */
var_cell_t *
var_cell_new(uint16_t payload_len)
{
  size_t size = STRUCT_OFFSET(var_cell_t, payload) + payload_len;
  var_cell_t *cell = tor_malloc(size);
  cell->payload_len = payload_len;
  cell->command = 0;
  cell->circ_id = 0;
  return cell;
}

/** Release all space held by <b>cell</b>. */
void
var_cell_free(var_cell_t *cell)
{
  tor_free(cell);
}

/** We've received an EOF from <b>conn</b>. Mark it for close and return. */
int
connection_or_reached_eof(or_connection_t *conn)
{
  log_info(LD_OR,"OR connection reached EOF. Closing.");
  connection_mark_for_close(TO_CONN(conn));
  return 0;
}

/** Handle any new bytes that have come in on connection <b>conn</b>.
 * If conn is in 'open' state, hand it to
 * connection_or_process_cells_from_inbuf()
 * (else do nothing).
 */
int
connection_or_process_inbuf(or_connection_t *conn)
{
  int ret;
  tor_assert(conn);

  switch (conn->_base.state) {
    case OR_CONN_STATE_PROXY_HANDSHAKING:
      ret = connection_read_proxy_handshake(TO_CONN(conn));

      /* start TLS after handshake completion, or deal with error */
      if (ret == 1) {
        tor_assert(TO_CONN(conn)->proxy_state == PROXY_CONNECTED);
        if (connection_tls_start_handshake(conn, 0) < 0)
          ret = -1;
      }
      if (ret < 0) {
        connection_mark_for_close(TO_CONN(conn));
      }

      return ret;
    case OR_CONN_STATE_TLS_SERVER_RENEGOTIATING:
#ifdef USE_BUFFEREVENTS
      if (tor_tls_server_got_renegotiate(conn->tls))
        connection_or_tls_renegotiated_cb(conn->tls, conn);
      if (conn->_base.marked_for_close)
        return 0;
      /* fall through. */
#endif
    case OR_CONN_STATE_OPEN:
    case OR_CONN_STATE_OR_HANDSHAKING_V2:
    case OR_CONN_STATE_OR_HANDSHAKING_V3:
      return connection_or_process_cells_from_inbuf(conn);
    default:
      return 0; /* don't do anything */
  }
}

/** When adding cells to an OR connection's outbuf, keep adding until the
 * outbuf is at least this long, or we run out of cells. */
#define OR_CONN_HIGHWATER (32*1024)

/** Add cells to an OR connection's outbuf whenever the outbuf's data length
 * drops below this size. */
#define OR_CONN_LOWWATER (16*1024)

/** Called whenever we have flushed some data on an or_conn: add more data
 * from active circuits. */
int
connection_or_flushed_some(or_connection_t *conn)
{
  size_t datalen = connection_get_outbuf_len(TO_CONN(conn));
  /* If we're under the low water mark, add cells until we're just over the
   * high water mark. */
  if (datalen < OR_CONN_LOWWATER) {
    ssize_t n = CEIL_DIV(OR_CONN_HIGHWATER - datalen, CELL_NETWORK_SIZE);
    time_t now = approx_time();
    while (conn->active_circuits && n > 0) {
      int flushed;
      flushed = connection_or_flush_from_first_active_circuit(conn, 1, now);
      n -= flushed;
    }
  }
  return 0;
}

/** Connection <b>conn</b> has finished writing and has no bytes left on
 * its outbuf.
 *
 * Otherwise it's in state "open": stop writing and return.
 *
 * If <b>conn</b> is broken, mark it for close and return -1, else
 * return 0.
 */
int
connection_or_finished_flushing(or_connection_t *conn)
{
  tor_assert(conn);
  assert_connection_ok(TO_CONN(conn),0);

  switch (conn->_base.state) {
    case OR_CONN_STATE_PROXY_HANDSHAKING:
    case OR_CONN_STATE_OPEN:
    case OR_CONN_STATE_OR_HANDSHAKING_V2:
    case OR_CONN_STATE_OR_HANDSHAKING_V3:
      break;
    default:
      log_err(LD_BUG,"Called in unexpected state %d.", conn->_base.state);
      tor_fragile_assert();
      return -1;
  }
  return 0;
}

/** Connected handler for OR connections: begin the TLS handshake.
 */
int
connection_or_finished_connecting(or_connection_t *or_conn)
{
  const int proxy_type = or_conn->proxy_type;
  connection_t *conn;
  tor_assert(or_conn);
  conn = TO_CONN(or_conn);
  tor_assert(conn->state == OR_CONN_STATE_CONNECTING);

  log_debug(LD_HANDSHAKE,"OR connect() to router at %s:%u finished.",
            conn->address,conn->port);
  control_event_bootstrap(BOOTSTRAP_STATUS_HANDSHAKE, 0);

  if (proxy_type != PROXY_NONE) {
    /* start proxy handshake */
    if (connection_proxy_connect(conn, proxy_type) < 0) {
      connection_mark_for_close(conn);
      return -1;
    }

    connection_start_reading(conn);
    conn->state = OR_CONN_STATE_PROXY_HANDSHAKING;
    return 0;
  }

  if (connection_tls_start_handshake(or_conn, 0) < 0) {
    /* TLS handshaking error of some kind. */
    connection_mark_for_close(conn);
    return -1;
  }
  return 0;
}

/* Called when we're about to finally unlink and free an OR connection:
 * perform necessary accounting and cleanup */
void
connection_or_about_to_close(or_connection_t *or_conn)
{
  time_t now = time(NULL);
  connection_t *conn = TO_CONN(or_conn);

  /* Remember why we're closing this connection. */
  if (conn->state != OR_CONN_STATE_OPEN) {
    /* Inform any pending (not attached) circs that they should
     * give up. */
    circuit_n_conn_done(TO_OR_CONN(conn), 0);
    /* now mark things down as needed */
    if (connection_or_nonopen_was_started_here(or_conn)) {
      const or_options_t *options = get_options();
      connection_or_note_state_when_broken(or_conn);
      rep_hist_note_connect_failed(or_conn->identity_digest, now);
      entry_guard_register_connect_status(or_conn->identity_digest,0,
                                          !options->HTTPSProxy, now);
      if (conn->state >= OR_CONN_STATE_TLS_HANDSHAKING) {
        int reason = tls_error_to_orconn_end_reason(or_conn->tls_error);
        control_event_or_conn_status(or_conn, OR_CONN_EVENT_FAILED,
                                     reason);
        if (!authdir_mode_tests_reachability(options))
          control_event_bootstrap_problem(
                orconn_end_reason_to_control_string(reason), reason);
      }
    }
  } else if (conn->hold_open_until_flushed) {
    /* We only set hold_open_until_flushed when we're intentionally
     * closing a connection. */
    rep_hist_note_disconnect(or_conn->identity_digest, now);
    control_event_or_conn_status(or_conn, OR_CONN_EVENT_CLOSED,
                tls_error_to_orconn_end_reason(or_conn->tls_error));
  } else if (!tor_digest_is_zero(or_conn->identity_digest)) {
    rep_hist_note_connection_died(or_conn->identity_digest, now);
    control_event_or_conn_status(or_conn, OR_CONN_EVENT_CLOSED,
                tls_error_to_orconn_end_reason(or_conn->tls_error));
  }
  /* Now close all the attached circuits on it. */
  circuit_unlink_all_from_or_conn(TO_OR_CONN(conn),
                                  END_CIRC_REASON_OR_CONN_CLOSED);
}

/** Return 1 if identity digest <b>id_digest</b> is known to be a
 * currently or recently running relay. Otherwise return 0. */
int
connection_or_digest_is_known_relay(const char *id_digest)
{
  if (router_get_consensus_status_by_id(id_digest))
    return 1; /* It's in the consensus: "yes" */
  if (router_get_by_id_digest(id_digest))
    return 1; /* Not in the consensus, but we have a descriptor for
               * it. Probably it was in a recent consensus. "Yes". */
  return 0;
}

/** Set the per-conn read and write limits for <b>conn</b>. If it's a known
 * relay, we will rely on the global read and write buckets, so give it
 * per-conn limits that are big enough they'll never matter. But if it's
 * not a known relay, first check if we set PerConnBwRate/Burst, then
 * check if the consensus sets them, else default to 'big enough'.
 *
 * If <b>reset</b> is true, set the bucket to be full.  Otherwise, just
 * clip the bucket if it happens to be <em>too</em> full.
 */
static void
connection_or_update_token_buckets_helper(or_connection_t *conn, int reset,
                                          const or_options_t *options)
{
  int rate, burst; /* per-connection rate limiting params */
  if (connection_or_digest_is_known_relay(conn->identity_digest)) {
    /* It's in the consensus, or we have a descriptor for it meaning it
     * was probably in a recent consensus. It's a recognized relay:
     * give it full bandwidth. */
    rate = (int)options->BandwidthRate;
    burst = (int)options->BandwidthBurst;
  } else {
    /* Not a recognized relay. Squeeze it down based on the suggested
     * bandwidth parameters in the consensus, but allow local config
     * options to override. */
    rate = options->PerConnBWRate ? (int)options->PerConnBWRate :
        networkstatus_get_param(NULL, "perconnbwrate",
                                (int)options->BandwidthRate, 1, INT32_MAX);
    burst = options->PerConnBWBurst ? (int)options->PerConnBWBurst :
        networkstatus_get_param(NULL, "perconnbwburst",
                                (int)options->BandwidthBurst, 1, INT32_MAX);
  }

  conn->bandwidthrate = rate;
  conn->bandwidthburst = burst;
#ifdef USE_BUFFEREVENTS
  {
    const struct timeval *tick = tor_libevent_get_one_tick_timeout();
    struct ev_token_bucket_cfg *cfg, *old_cfg;
    int64_t rate64 = (((int64_t)rate) * options->TokenBucketRefillInterval)
      / 1000;
    /* This can't overflow, since TokenBucketRefillInterval <= 1000,
     * and rate started out less than INT_MAX. */
    int rate_per_tick = (int) rate64;

    cfg = ev_token_bucket_cfg_new(rate_per_tick, burst, rate_per_tick,
                                  burst, tick);
    old_cfg = conn->bucket_cfg;
    if (conn->_base.bufev)
      tor_set_bufferevent_rate_limit(conn->_base.bufev, cfg);
    if (old_cfg)
      ev_token_bucket_cfg_free(old_cfg);
    conn->bucket_cfg = cfg;
    (void) reset; /* No way to do this with libevent yet. */
  }
#else
  if (reset) { /* set up the token buckets to be full */
    conn->read_bucket = conn->write_bucket = burst;
    return;
  }
  /* If the new token bucket is smaller, take out the extra tokens.
   * (If it's larger, don't -- the buckets can grow to reach the cap.) */
  if (conn->read_bucket > burst)
    conn->read_bucket = burst;
  if (conn->write_bucket > burst)
    conn->write_bucket = burst;
#endif
}

/** Either our set of relays or our per-conn rate limits have changed.
 * Go through all the OR connections and update their token buckets to make
 * sure they don't exceed their maximum values. */
void
connection_or_update_token_buckets(smartlist_t *conns,
                                   const or_options_t *options)
{
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (connection_speaks_cells(conn))
      connection_or_update_token_buckets_helper(TO_OR_CONN(conn), 0, options);
  });
}

/** If we don't necessarily know the router we're connecting to, but we
 * have an addr/port/id_digest, then fill in as much as we can. Start
 * by checking to see if this describes a router we know.
 * <b>started_here</b> is 1 if we are the initiator of <b>conn</b> and
 * 0 if it's an incoming connection.  */
void
connection_or_init_conn_from_address(or_connection_t *conn,
                                     const tor_addr_t *addr, uint16_t port,
                                     const char *id_digest,
                                     int started_here)
{
  const node_t *r = node_get_by_id(id_digest);
  connection_or_set_identity_digest(conn, id_digest);
  connection_or_update_token_buckets_helper(conn, 1, get_options());

  conn->_base.port = port;
  tor_addr_copy(&conn->_base.addr, addr);
  tor_addr_copy(&conn->real_addr, addr);
  if (r) {
    tor_addr_port_t node_ap;
    node_get_pref_orport(r, &node_ap);
    /* XXXX proposal 186 is making this more complex.  For now, a conn
       is canonical when it uses the _preferred_ address. */
    if (tor_addr_eq(&conn->_base.addr, &node_ap.addr))
      conn->is_canonical = 1;
    if (!started_here) {
      /* Override the addr/port, so our log messages will make sense.
       * This is dangerous, since if we ever try looking up a conn by
       * its actual addr/port, we won't remember. Careful! */
      /* XXXX arma: this is stupid, and it's the reason we need real_addr
       * to track is_canonical properly.  What requires it? */
      /* XXXX <arma> i believe the reason we did this, originally, is because
       * we wanted to log what OR a connection was to, and if we logged the
       * right IP address and port 56244, that wouldn't be as helpful. now we
       * log the "right" port too, so we know if it's moria1 or moria2.
       */
      tor_addr_copy(&conn->_base.addr, &node_ap.addr);
      conn->_base.port = node_ap.port;
    }
    conn->nickname = tor_strdup(node_get_nickname(r));
    tor_free(conn->_base.address);
    conn->_base.address = tor_dup_addr(&node_ap.addr);
  } else {
    const char *n;
    /* If we're an authoritative directory server, we may know a
     * nickname for this router. */
    n = dirserv_get_nickname_by_digest(id_digest);
    if (n) {
      conn->nickname = tor_strdup(n);
    } else {
      conn->nickname = tor_malloc(HEX_DIGEST_LEN+2);
      conn->nickname[0] = '$';
      base16_encode(conn->nickname+1, HEX_DIGEST_LEN+1,
                    conn->identity_digest, DIGEST_LEN);
    }
    tor_free(conn->_base.address);
    conn->_base.address = tor_dup_addr(addr);
  }
}

/** Return true iff <b>a</b> is "better" than <b>b</b> for new circuits.
 *
 * A more canonical connection is always better than a less canonical
 * connection.  That aside, a connection is better if it has circuits and the
 * other does not, or if it was created more recently.
 *
 * Requires that both input connections are open; not is_bad_for_new_circs,
 * and not impossibly non-canonical.
 *
 * If <b>forgive_new_connections</b> is true, then we do not call
 * <b>a</b>better than <b>b</b> simply because b has no circuits,
 * unless b is also relatively old.
 */
static int
connection_or_is_better(time_t now,
                        const or_connection_t *a,
                        const or_connection_t *b,
                        int forgive_new_connections)
{
  int newer;
/** Do not definitively deprecate a new connection with no circuits on it
 * until this much time has passed. */
#define NEW_CONN_GRACE_PERIOD (15*60)

  if (b->is_canonical && !a->is_canonical)
    return 0; /* A canonical connection is better than a non-canonical
               * one, no matter how new it is or which has circuits. */

  newer = b->_base.timestamp_created < a->_base.timestamp_created;

  if (
      /* We prefer canonical connections regardless of newness. */
      (!b->is_canonical && a->is_canonical) ||
      /* If both have circuits we prefer the newer: */
      (b->n_circuits && a->n_circuits && newer) ||
      /* If neither has circuits we prefer the newer: */
      (!b->n_circuits && !a->n_circuits && newer))
    return 1;

  /* If one has no circuits and the other does... */
  if (!b->n_circuits && a->n_circuits) {
    /* Then it's bad, unless it's in its grace period and we're forgiving. */
    if (forgive_new_connections &&
        now < b->_base.timestamp_created + NEW_CONN_GRACE_PERIOD)
      return 0;
    else
      return 1;
  }

  return 0;
}

/** Return the OR connection we should use to extend a circuit to the router
 * whose identity is <b>digest</b>, and whose address we believe (or have been
 * told in an extend cell) is <b>target_addr</b>.  If there is no good
 * connection, set *<b>msg_out</b> to a message describing the connection's
 * state and our next action, and set <b>launch_out</b> to a boolean for
 * whether we should launch a new connection or not.
 */
or_connection_t *
connection_or_get_for_extend(const char *digest,
                             const tor_addr_t *target_addr,
                             const char **msg_out,
                             int *launch_out)
{
  or_connection_t *conn, *best=NULL;
  int n_inprogress_goodaddr = 0, n_old = 0, n_noncanonical = 0, n_possible = 0;
  time_t now = approx_time();

  tor_assert(msg_out);
  tor_assert(launch_out);

  if (!orconn_identity_map) {
    *msg_out = "Router not connected (nothing is).  Connecting.";
    *launch_out = 1;
    return NULL;
  }

  conn = digestmap_get(orconn_identity_map, digest);

  for (; conn; conn = conn->next_with_same_id) {
    tor_assert(conn->_base.magic == OR_CONNECTION_MAGIC);
    tor_assert(conn->_base.type == CONN_TYPE_OR);
    tor_assert(tor_memeq(conn->identity_digest, digest, DIGEST_LEN));
    if (conn->_base.marked_for_close)
      continue;
    /* Never return a connection on which the other end appears to be
     * a client. */
    if (conn->is_connection_with_client) {
      continue;
    }
    /* Never return a non-open connection. */
    if (conn->_base.state != OR_CONN_STATE_OPEN) {
      /* If the address matches, don't launch a new connection for this
       * circuit. */
      if (!tor_addr_compare(&conn->real_addr, target_addr, CMP_EXACT))
        ++n_inprogress_goodaddr;
      continue;
    }
    /* Never return a connection that shouldn't be used for circs. */
    if (conn->is_bad_for_new_circs) {
      ++n_old;
      continue;
    }
    /* Never return a non-canonical connection using a recent link protocol
     * if the address is not what we wanted.
     *
     * (For old link protocols, we can't rely on is_canonical getting
     * set properly if we're talking to the right address, since we might
     * have an out-of-date descriptor, and we will get no NETINFO cell to
     * tell us about the right address.) */
    if (!conn->is_canonical && conn->link_proto >= 2 &&
        tor_addr_compare(&conn->real_addr, target_addr, CMP_EXACT)) {
      ++n_noncanonical;
      continue;
    }

    ++n_possible;

    if (!best) {
      best = conn; /* If we have no 'best' so far, this one is good enough. */
      continue;
    }

    if (connection_or_is_better(now, conn, best, 0))
      best = conn;
  }

  if (best) {
    *msg_out = "Connection is fine; using it.";
    *launch_out = 0;
    return best;
  } else if (n_inprogress_goodaddr) {
    *msg_out = "Connection in progress; waiting.";
    *launch_out = 0;
    return NULL;
  } else if (n_old || n_noncanonical) {
    *msg_out = "Connections all too old, or too non-canonical. "
      " Launching a new one.";
    *launch_out = 1;
    return NULL;
  } else {
    *msg_out = "Not connected. Connecting.";
    *launch_out = 1;
    return NULL;
  }
}

/** How old do we let a connection to an OR get before deciding it's
 * too old for new circuits? */
#define TIME_BEFORE_OR_CONN_IS_TOO_OLD (60*60*24*7)

/** Given the head of the linked list for all the or_connections with a given
 * identity, set elements of that list as is_bad_for_new_circs as
 * appropriate. Helper for connection_or_set_bad_connections().
 *
 * Specifically, we set the is_bad_for_new_circs flag on:
 *    - all connections if <b>force</b> is true.
 *    - all connections that are too old.
 *    - all open non-canonical connections for which a canonical connection
 *      exists to the same router.
 *    - all open canonical connections for which a 'better' canonical
 *      connection exists to the same router.
 *    - all open non-canonical connections for which a 'better' non-canonical
 *      connection exists to the same router at the same address.
 *
 * See connection_or_is_better() for our idea of what makes one OR connection
 * better than another.
 */
static void
connection_or_group_set_badness(or_connection_t *head, int force)
{
  or_connection_t *or_conn = NULL, *best = NULL;
  int n_old = 0, n_inprogress = 0, n_canonical = 0, n_other = 0;
  time_t now = time(NULL);

  /* Pass 1: expire everything that's old, and see what the status of
   * everything else is. */
  for (or_conn = head; or_conn; or_conn = or_conn->next_with_same_id) {
    if (or_conn->_base.marked_for_close ||
        or_conn->is_bad_for_new_circs)
      continue;
    if (force ||
        or_conn->_base.timestamp_created + TIME_BEFORE_OR_CONN_IS_TOO_OLD
          < now) {
      log_info(LD_OR,
               "Marking OR conn to %s:%d as too old for new circuits "
               "(fd %d, %d secs old).",
               or_conn->_base.address, or_conn->_base.port, or_conn->_base.s,
               (int)(now - or_conn->_base.timestamp_created));
      or_conn->is_bad_for_new_circs = 1;
    }

    if (or_conn->is_bad_for_new_circs) {
      ++n_old;
    } else if (or_conn->_base.state != OR_CONN_STATE_OPEN) {
      ++n_inprogress;
    } else if (or_conn->is_canonical) {
      ++n_canonical;
    } else {
      ++n_other;
    }
  }

  /* Pass 2: We know how about how good the best connection is.
   * expire everything that's worse, and find the very best if we can. */
  for (or_conn = head; or_conn; or_conn = or_conn->next_with_same_id) {
    if (or_conn->_base.marked_for_close ||
        or_conn->is_bad_for_new_circs)
      continue; /* This one doesn't need to be marked bad. */
    if (or_conn->_base.state != OR_CONN_STATE_OPEN)
      continue; /* Don't mark anything bad until we have seen what happens
                 * when the connection finishes. */
    if (n_canonical && !or_conn->is_canonical) {
      /* We have at least one open canonical connection to this router,
       * and this one is open but not canonical.  Mark it bad. */
      log_info(LD_OR,
               "Marking OR conn to %s:%d as unsuitable for new circuits: "
               "(fd %d, %d secs old).  It is not canonical, and we have "
               "another connection to that OR that is.",
               or_conn->_base.address, or_conn->_base.port, or_conn->_base.s,
               (int)(now - or_conn->_base.timestamp_created));
      or_conn->is_bad_for_new_circs = 1;
      continue;
    }

    if (!best || connection_or_is_better(now, or_conn, best, 0))
      best = or_conn;
  }

  if (!best)
    return;

  /* Pass 3: One connection to OR is best.  If it's canonical, mark as bad
   * every other open connection.  If it's non-canonical, mark as bad
   * every other open connection to the same address.
   *
   * XXXX This isn't optimal; if we have connections to an OR at multiple
   *   addresses, we'd like to pick the best _for each address_, and mark as
   *   bad every open connection that isn't best for its address.  But this
   *   can only occur in cases where the other OR is old (so we have no
   *   canonical connection to it), or where all the connections to the OR are
   *   at noncanonical addresses and we have no good direct connection (which
   *   means we aren't at risk of attaching circuits to it anyway).  As
   *   0.1.2.x dies out, the first case will go away, and the second one is
   *   "mostly harmless", so a fix can wait until somebody is bored.
   */
  for (or_conn = head; or_conn; or_conn = or_conn->next_with_same_id) {
    if (or_conn->_base.marked_for_close ||
        or_conn->is_bad_for_new_circs ||
        or_conn->_base.state != OR_CONN_STATE_OPEN)
      continue;
    if (or_conn != best && connection_or_is_better(now, best, or_conn, 1)) {
      /* This isn't the best conn, _and_ the best conn is better than it,
         even when we're being forgiving. */
      if (best->is_canonical) {
        log_info(LD_OR,
                 "Marking OR conn to %s:%d as unsuitable for new circuits: "
                 "(fd %d, %d secs old).  We have a better canonical one "
                 "(fd %d; %d secs old).",
                 or_conn->_base.address, or_conn->_base.port, or_conn->_base.s,
                 (int)(now - or_conn->_base.timestamp_created),
                 best->_base.s, (int)(now - best->_base.timestamp_created));
        or_conn->is_bad_for_new_circs = 1;
      } else if (!tor_addr_compare(&or_conn->real_addr,
                                   &best->real_addr, CMP_EXACT)) {
        log_info(LD_OR,
                 "Marking OR conn to %s:%d as unsuitable for new circuits: "
                 "(fd %d, %d secs old).  We have a better one with the "
                 "same address (fd %d; %d secs old).",
                 or_conn->_base.address, or_conn->_base.port, or_conn->_base.s,
                 (int)(now - or_conn->_base.timestamp_created),
                 best->_base.s, (int)(now - best->_base.timestamp_created));
        or_conn->is_bad_for_new_circs = 1;
      }
    }
  }
}

/** Go through all the OR connections (or if <b>digest</b> is non-NULL, just
 * the OR connections with that digest), and set the is_bad_for_new_circs
 * flag based on the rules in connection_or_group_set_badness() (or just
 * always set it if <b>force</b> is true).
 */
void
connection_or_set_bad_connections(const char *digest, int force)
{
  if (!orconn_identity_map)
    return;

  DIGESTMAP_FOREACH(orconn_identity_map, identity, or_connection_t *, conn) {
    if (!digest || tor_memeq(digest, conn->identity_digest, DIGEST_LEN))
      connection_or_group_set_badness(conn, force);
  } DIGESTMAP_FOREACH_END;
}

/** <b>conn</b> is in the 'connecting' state, and it failed to complete
 * a TCP connection. Send notifications appropriately.
 *
 * <b>reason</b> specifies the or_conn_end_reason for the failure;
 * <b>msg</b> specifies the strerror-style error message.
 */
void
connection_or_connect_failed(or_connection_t *conn,
                             int reason, const char *msg)
{
  control_event_or_conn_status(conn, OR_CONN_EVENT_FAILED, reason);
  if (!authdir_mode_tests_reachability(get_options()))
    control_event_bootstrap_problem(msg, reason);
}

/** Launch a new OR connection to <b>addr</b>:<b>port</b> and expect to
 * handshake with an OR with identity digest <b>id_digest</b>.
 *
 * If <b>id_digest</b> is me, do nothing. If we're already connected to it,
 * return that connection. If the connect() is in progress, set the
 * new conn's state to 'connecting' and return it. If connect() succeeds,
 * call connection_tls_start_handshake() on it.
 *
 * This function is called from router_retry_connections(), for
 * ORs connecting to ORs, and circuit_establish_circuit(), for
 * OPs connecting to ORs.
 *
 * Return the launched conn, or NULL if it failed.
 */
or_connection_t *
connection_or_connect(const tor_addr_t *_addr, uint16_t port,
                      const char *id_digest)
{
  or_connection_t *conn;
  const or_options_t *options = get_options();
  int socket_error = 0;
  tor_addr_t addr;

  int r;
  tor_addr_t proxy_addr;
  uint16_t proxy_port;
  int proxy_type;

  tor_assert(_addr);
  tor_assert(id_digest);
  tor_addr_copy(&addr, _addr);

  if (server_mode(options) && router_digest_is_me(id_digest)) {
    log_info(LD_PROTOCOL,"Client asked me to connect to myself. Refusing.");
    return NULL;
  }

  conn = or_connection_new(tor_addr_family(&addr));

  /* set up conn so it's got all the data we need to remember */
  connection_or_init_conn_from_address(conn, &addr, port, id_digest, 1);
  conn->_base.state = OR_CONN_STATE_CONNECTING;
  control_event_or_conn_status(conn, OR_CONN_EVENT_LAUNCHED, 0);

  conn->is_outgoing = 1;

  /* If we are using a proxy server, find it and use it. */
  r = get_proxy_addrport(&proxy_addr, &proxy_port, &proxy_type, TO_CONN(conn));
  if (r == 0) {
    conn->proxy_type = proxy_type;
    if (proxy_type != PROXY_NONE) {
      tor_addr_copy(&addr, &proxy_addr);
      port = proxy_port;
      conn->_base.proxy_state = PROXY_INFANT;
    }
  } else {
    log_warn(LD_GENERAL, "Tried to connect through proxy, but proxy address "
             "could not be found.");
    connection_free(TO_CONN(conn));
    return NULL;
  }

  switch (connection_connect(TO_CONN(conn), conn->_base.address,
                             &addr, port, &socket_error)) {
    case -1:
      /* If the connection failed immediately, and we're using
       * a proxy, our proxy is down. Don't blame the Tor server. */
      if (conn->_base.proxy_state == PROXY_INFANT)
        entry_guard_register_connect_status(conn->identity_digest,
                                            0, 1, time(NULL));
      connection_or_connect_failed(conn,
                                   errno_to_orconn_end_reason(socket_error),
                                   tor_socket_strerror(socket_error));
      connection_free(TO_CONN(conn));
      return NULL;
    case 0:
      connection_watch_events(TO_CONN(conn), READ_EVENT | WRITE_EVENT);
      /* writable indicates finish, readable indicates broken link,
         error indicates broken link on windows */
      return conn;
    /* case 1: fall through */
  }

  if (connection_or_finished_connecting(conn) < 0) {
    /* already marked for close */
    return NULL;
  }
  return conn;
}

/** Begin the tls handshake with <b>conn</b>. <b>receiving</b> is 0 if
 * we initiated the connection, else it's 1.
 *
 * Assign a new tls object to conn->tls, begin reading on <b>conn</b>, and
 * pass <b>conn</b> to connection_tls_continue_handshake().
 *
 * Return -1 if <b>conn</b> is broken, else return 0.
 */
int
connection_tls_start_handshake(or_connection_t *conn, int receiving)
{
  conn->_base.state = OR_CONN_STATE_TLS_HANDSHAKING;
  tor_assert(!conn->tls);
  conn->tls = tor_tls_new(conn->_base.s, receiving);
  if (!conn->tls) {
    log_warn(LD_BUG,"tor_tls_new failed. Closing.");
    return -1;
  }
  tor_tls_set_logged_address(conn->tls, // XXX client and relay?
      escaped_safe_str(conn->_base.address));

#ifdef USE_BUFFEREVENTS
  if (connection_type_uses_bufferevent(TO_CONN(conn))) {
    const int filtering = get_options()->_UseFilteringSSLBufferevents;
    struct bufferevent *b =
      tor_tls_init_bufferevent(conn->tls, conn->_base.bufev, conn->_base.s,
                               receiving, filtering);
    if (!b) {
      log_warn(LD_BUG,"tor_tls_init_bufferevent failed. Closing.");
      return -1;
    }
    conn->_base.bufev = b;
    if (conn->bucket_cfg)
      tor_set_bufferevent_rate_limit(conn->_base.bufev, conn->bucket_cfg);
    connection_enable_rate_limiting(TO_CONN(conn));

    connection_configure_bufferevent_callbacks(TO_CONN(conn));
    bufferevent_setcb(b,
                      connection_handle_read_cb,
                      connection_handle_write_cb,
                      connection_or_handle_event_cb,/* overriding this one*/
                      TO_CONN(conn));
  }
#endif
  connection_start_reading(TO_CONN(conn));
  log_debug(LD_HANDSHAKE,"starting TLS handshake on fd %d", conn->_base.s);
  note_crypto_pk_op(receiving ? TLS_HANDSHAKE_S : TLS_HANDSHAKE_C);

  IF_HAS_BUFFEREVENT(TO_CONN(conn), {
    /* ???? */;
  }) ELSE_IF_NO_BUFFEREVENT {
    if (connection_tls_continue_handshake(conn) < 0)
      return -1;
  }
  return 0;
}

/** Invoked on the server side from inside tor_tls_read() when the server
 * gets a successful TLS renegotiation from the client. */
static void
connection_or_tls_renegotiated_cb(tor_tls_t *tls, void *_conn)
{
  or_connection_t *conn = _conn;
  (void)tls;

  /* Don't invoke this again. */
  tor_tls_set_renegotiate_callback(tls, NULL, NULL);
  tor_tls_block_renegotiation(tls);

  if (connection_tls_finish_handshake(conn) < 0) {
    /* XXXX_TLS double-check that it's ok to do this from inside read. */
    /* XXXX_TLS double-check that this verifies certificates. */
    connection_mark_for_close(TO_CONN(conn));
  }
}

/** Move forward with the tls handshake. If it finishes, hand
 * <b>conn</b> to connection_tls_finish_handshake().
 *
 * Return -1 if <b>conn</b> is broken, else return 0.
 */
int
connection_tls_continue_handshake(or_connection_t *conn)
{
  int result;
  check_no_tls_errors();
 again:
  if (conn->_base.state == OR_CONN_STATE_TLS_CLIENT_RENEGOTIATING) {
    // log_notice(LD_OR, "Renegotiate with %p", conn->tls);
    result = tor_tls_renegotiate(conn->tls);
    // log_notice(LD_OR, "Result: %d", result);
  } else {
    tor_assert(conn->_base.state == OR_CONN_STATE_TLS_HANDSHAKING);
    // log_notice(LD_OR, "Continue handshake with %p", conn->tls);
    result = tor_tls_handshake(conn->tls);
    // log_notice(LD_OR, "Result: %d", result);
  }
  switch (result) {
    CASE_TOR_TLS_ERROR_ANY:
    log_info(LD_OR,"tls error [%s]. breaking connection.",
             tor_tls_err_to_string(result));
      return -1;
    case TOR_TLS_DONE:
      if (! tor_tls_used_v1_handshake(conn->tls)) {
        if (!tor_tls_is_server(conn->tls)) {
          if (conn->_base.state == OR_CONN_STATE_TLS_HANDSHAKING) {
            if (tor_tls_received_v3_certificate(conn->tls)) {
              log_info(LD_OR, "Client got a v3 cert!  Moving on to v3 "
                         "handshake.");
              return connection_or_launch_v3_or_handshake(conn);
            } else {
              log_debug(LD_OR, "Done with initial SSL handshake (client-side)."
                        " Requesting renegotiation.");
              conn->_base.state = OR_CONN_STATE_TLS_CLIENT_RENEGOTIATING;
              goto again;
            }
          }
          // log_notice(LD_OR,"Done. state was %d.", conn->_base.state);
        } else {
          /* v2/v3 handshake, but not a client. */
          log_debug(LD_OR, "Done with initial SSL handshake (server-side). "
                           "Expecting renegotiation or VERSIONS cell");
          tor_tls_set_renegotiate_callback(conn->tls,
                                           connection_or_tls_renegotiated_cb,
                                           conn);
          conn->_base.state = OR_CONN_STATE_TLS_SERVER_RENEGOTIATING;
          connection_stop_writing(TO_CONN(conn));
          connection_start_reading(TO_CONN(conn));
          return 0;
        }
      }
      return connection_tls_finish_handshake(conn);
    case TOR_TLS_WANTWRITE:
      connection_start_writing(TO_CONN(conn));
      log_debug(LD_OR,"wanted write");
      return 0;
    case TOR_TLS_WANTREAD: /* handshaking conns are *always* reading */
      log_debug(LD_OR,"wanted read");
      return 0;
    case TOR_TLS_CLOSE:
      log_info(LD_OR,"tls closed. breaking connection.");
      return -1;
  }
  return 0;
}

#ifdef USE_BUFFEREVENTS
static void
connection_or_handle_event_cb(struct bufferevent *bufev, short event,
                              void *arg)
{
  struct or_connection_t *conn = TO_OR_CONN(arg);

  /* XXXX cut-and-paste code; should become a function. */
  if (event & BEV_EVENT_CONNECTED) {
    if (conn->_base.state == OR_CONN_STATE_TLS_HANDSHAKING) {
      if (tor_tls_finish_handshake(conn->tls) < 0) {
        log_warn(LD_OR, "Problem finishing handshake");
        connection_mark_for_close(TO_CONN(conn));
        return;
      }
    }

    if (! tor_tls_used_v1_handshake(conn->tls)) {
      if (!tor_tls_is_server(conn->tls)) {
        if (conn->_base.state == OR_CONN_STATE_TLS_HANDSHAKING) {
          if (tor_tls_received_v3_certificate(conn->tls)) {
            log_info(LD_OR, "Client got a v3 cert!");
            if (connection_or_launch_v3_or_handshake(conn) < 0)
              connection_mark_for_close(TO_CONN(conn));
            return;
          } else {
            conn->_base.state = OR_CONN_STATE_TLS_CLIENT_RENEGOTIATING;
            tor_tls_unblock_renegotiation(conn->tls);
            if (bufferevent_ssl_renegotiate(conn->_base.bufev)<0) {
              log_warn(LD_OR, "Start_renegotiating went badly.");
              connection_mark_for_close(TO_CONN(conn));
            }
            tor_tls_unblock_renegotiation(conn->tls);
            return; /* ???? */
          }
        }
      } else if (tor_tls_get_num_server_handshakes(conn->tls) == 1) {
        /* v2 or v3 handshake, as a server. Only got one handshake, so
         * wait for the next one. */
        tor_tls_set_renegotiate_callback(conn->tls,
                                         connection_or_tls_renegotiated_cb,
                                         conn);
        conn->_base.state = OR_CONN_STATE_TLS_SERVER_RENEGOTIATING;
        /* return 0; */
        return; /* ???? */
      } else {
        const int handshakes = tor_tls_get_num_server_handshakes(conn->tls);
        tor_assert(handshakes >= 2);
        if (handshakes == 2) {
          /* v2 handshake, as a server.  Two handshakes happened already,
           * so we treat renegotiation as done.
           */
          connection_or_tls_renegotiated_cb(conn->tls, conn);
        } else {
          log_warn(LD_OR, "More than two handshakes done on connection. "
                   "Closing.");
          connection_mark_for_close(TO_CONN(conn));
        }
        return;
      }
    }
    connection_watch_events(TO_CONN(conn), READ_EVENT|WRITE_EVENT);
    if (connection_tls_finish_handshake(conn) < 0)
      connection_mark_for_close(TO_CONN(conn)); /* ???? */
    return;
  }

  if (event & BEV_EVENT_ERROR) {
    unsigned long err;
    while ((err = bufferevent_get_openssl_error(bufev))) {
      tor_tls_log_one_error(conn->tls, err, LOG_WARN, LD_OR,
                            "handshaking (with bufferevent)");
    }
  }

  connection_handle_event_cb(bufev, event, arg);
}
#endif

/** Return 1 if we initiated this connection, or 0 if it started
 * out as an incoming connection.
 */
int
connection_or_nonopen_was_started_here(or_connection_t *conn)
{
  tor_assert(conn->_base.type == CONN_TYPE_OR);
  if (!conn->tls)
    return 1; /* it's still in proxy states or something */
  if (conn->handshake_state)
    return conn->handshake_state->started_here;
  return !tor_tls_is_server(conn->tls);
}

/** Set the circid_type field of <b>conn</b> (which determines which part of
 * the circuit ID space we're willing to use) based on comparing our ID to
 * <b>identity_rcvd</b> */
void
connection_or_set_circid_type(or_connection_t *conn,
                              crypto_pk_env_t *identity_rcvd)
{
  const int started_here = connection_or_nonopen_was_started_here(conn);
  crypto_pk_env_t *our_identity =
    started_here ? get_tlsclient_identity_key() :
                   get_server_identity_key();

  if (identity_rcvd) {
    if (crypto_pk_cmp_keys(our_identity, identity_rcvd)<0) {
      conn->circ_id_type = CIRC_ID_TYPE_LOWER;
    } else {
      conn->circ_id_type = CIRC_ID_TYPE_HIGHER;
    }
  } else {
    conn->circ_id_type = CIRC_ID_TYPE_NEITHER;
  }
}

/** <b>Conn</b> just completed its handshake. Return 0 if all is well, and
 * return -1 if he is lying, broken, or otherwise something is wrong.
 *
 * If we initiated this connection (<b>started_here</b> is true), make sure
 * the other side sent a correctly formed certificate. If I initiated the
 * connection, make sure it's the right guy.
 *
 * Otherwise (if we _didn't_ initiate this connection), it's okay for
 * the certificate to be weird or absent.
 *
 * If we return 0, and the certificate is as expected, write a hash of the
 * identity key into <b>digest_rcvd_out</b>, which must have DIGEST_LEN
 * space in it.
 * If the certificate is invalid or missing on an incoming connection,
 * we return 0 and set <b>digest_rcvd_out</b> to DIGEST_LEN NUL bytes.
 * (If we return -1, the contents of this buffer are undefined.)
 *
 * As side effects,
 * 1) Set conn->circ_id_type according to tor-spec.txt.
 * 2) If we're an authdirserver and we initiated the connection: drop all
 *    descriptors that claim to be on that IP/port but that aren't
 *    this guy; and note that this guy is reachable.
 * 3) If this is a bridge and we didn't configure its identity
 *    fingerprint, remember the keyid we just learned.
 */
static int
connection_or_check_valid_tls_handshake(or_connection_t *conn,
                                        int started_here,
                                        char *digest_rcvd_out)
{
  crypto_pk_env_t *identity_rcvd=NULL;
  const or_options_t *options = get_options();
  int severity = server_mode(options) ? LOG_PROTOCOL_WARN : LOG_WARN;
  const char *safe_address =
    started_here ? conn->_base.address :
                   safe_str_client(conn->_base.address);
  const char *conn_type = started_here ? "outgoing" : "incoming";
  int has_cert = 0;

  check_no_tls_errors();
  has_cert = tor_tls_peer_has_cert(conn->tls);
  if (started_here && !has_cert) {
    log_info(LD_HANDSHAKE,"Tried connecting to router at %s:%d, but it didn't "
             "send a cert! Closing.",
             safe_address, conn->_base.port);
    return -1;
  } else if (!has_cert) {
    log_debug(LD_HANDSHAKE,"Got incoming connection with no certificate. "
              "That's ok.");
  }
  check_no_tls_errors();

  if (has_cert) {
    int v = tor_tls_verify(started_here?severity:LOG_INFO,
                           conn->tls, &identity_rcvd);
    if (started_here && v<0) {
      log_fn(severity,LD_HANDSHAKE,"Tried connecting to router at %s:%d: It"
             " has a cert but it's invalid. Closing.",
             safe_address, conn->_base.port);
        return -1;
    } else if (v<0) {
      log_info(LD_HANDSHAKE,"Incoming connection gave us an invalid cert "
               "chain; ignoring.");
    } else {
      log_debug(LD_HANDSHAKE,
                "The certificate seems to be valid on %s connection "
                "with %s:%d", conn_type, safe_address, conn->_base.port);
    }
    check_no_tls_errors();
  }

  if (identity_rcvd) {
    crypto_pk_get_digest(identity_rcvd, digest_rcvd_out);
  } else {
    memset(digest_rcvd_out, 0, DIGEST_LEN);
  }

  connection_or_set_circid_type(conn, identity_rcvd);
  crypto_free_pk_env(identity_rcvd);

  if (started_here)
    return connection_or_client_learned_peer_id(conn,
                                     (const uint8_t*)digest_rcvd_out);

  return 0;
}

/** Called when we (as a connection initiator) have definitively,
 * authenticatedly, learned that ID of the Tor instance on the other
 * side of <b>conn</b> is <b>peer_id</b>.  For v1 and v2 handshakes,
 * this is right after we get a certificate chain in a TLS handshake
 * or renegotiation.  For v3 handshakes, this is right after we get a
 * certificate chain in a CERTS cell.
 *
 * If we want any particular ID before, record the one we got.
 *
 * If we wanted an ID, but we didn't get it, log a warning and return -1.
 *
 * If we're testing reachability, remember what we learned.
 *
 * Return 0 on success, -1 on failure.
 */
int
connection_or_client_learned_peer_id(or_connection_t *conn,
                                     const uint8_t *peer_id)
{
  int as_expected = 1;
  const or_options_t *options = get_options();
  int severity = server_mode(options) ? LOG_PROTOCOL_WARN : LOG_WARN;

  if (tor_digest_is_zero(conn->identity_digest)) {
    connection_or_set_identity_digest(conn, (const char*)peer_id);
    tor_free(conn->nickname);
    conn->nickname = tor_malloc(HEX_DIGEST_LEN+2);
    conn->nickname[0] = '$';
    base16_encode(conn->nickname+1, HEX_DIGEST_LEN+1,
                  conn->identity_digest, DIGEST_LEN);
    log_info(LD_HANDSHAKE, "Connected to router %s at %s:%d without knowing "
                    "its key. Hoping for the best.",
                    conn->nickname, conn->_base.address, conn->_base.port);
    /* if it's a bridge and we didn't know its identity fingerprint, now
     * we do -- remember it for future attempts. */
    learned_router_identity(&conn->_base.addr, conn->_base.port,
                            (const char*)peer_id);
  }

  if (tor_memneq(peer_id, conn->identity_digest, DIGEST_LEN)) {
    /* I was aiming for a particular digest. I didn't get it! */
    char seen[HEX_DIGEST_LEN+1];
    char expected[HEX_DIGEST_LEN+1];
    base16_encode(seen, sizeof(seen), (const char*)peer_id, DIGEST_LEN);
    base16_encode(expected, sizeof(expected), conn->identity_digest,
                  DIGEST_LEN);
    log_fn(severity, LD_HANDSHAKE,
           "Tried connecting to router at %s:%d, but identity key was not "
           "as expected: wanted %s but got %s.",
           conn->_base.address, conn->_base.port, expected, seen);
    entry_guard_register_connect_status(conn->identity_digest, 0, 1,
                                        time(NULL));
    control_event_or_conn_status(conn, OR_CONN_EVENT_FAILED,
                                 END_OR_CONN_REASON_OR_IDENTITY);
    if (!authdir_mode_tests_reachability(options))
      control_event_bootstrap_problem(
                                "Unexpected identity in router certificate",
                                END_OR_CONN_REASON_OR_IDENTITY);
    as_expected = 0;
  }
  if (authdir_mode_tests_reachability(options)) {
    dirserv_orconn_tls_done(conn->_base.address, conn->_base.port,
                            (const char*)peer_id, as_expected);
  }
  if (!as_expected)
    return -1;

  return 0;
}

/** The v1/v2 TLS handshake is finished.
 *
 * Make sure we are happy with the person we just handshaked with.
 *
 * If he initiated the connection, make sure he's not already connected,
 * then initialize conn from the information in router.
 *
 * If all is successful, call circuit_n_conn_done() to handle events
 * that have been pending on the <tls handshake completion. Also set the
 * directory to be dirty (only matters if I'm an authdirserver).
 *
 * If this is a v2 TLS handshake, send a versions cell.
 */
static int
connection_tls_finish_handshake(or_connection_t *conn)
{
  char digest_rcvd[DIGEST_LEN];
  int started_here = connection_or_nonopen_was_started_here(conn);

  log_debug(LD_HANDSHAKE,"%s tls handshake on %p with %s done. verifying.",
            started_here?"outgoing":"incoming",
            conn,
            safe_str_client(conn->_base.address));

  directory_set_dirty();

  if (connection_or_check_valid_tls_handshake(conn, started_here,
                                              digest_rcvd) < 0)
    return -1;

  circuit_build_times_network_is_live(&circ_times);

  if (tor_tls_used_v1_handshake(conn->tls)) {
    conn->link_proto = 1;
    if (!started_here) {
      connection_or_init_conn_from_address(conn, &conn->_base.addr,
                                           conn->_base.port, digest_rcvd, 0);
    }
    tor_tls_block_renegotiation(conn->tls);
    return connection_or_set_state_open(conn);
  } else {
    conn->_base.state = OR_CONN_STATE_OR_HANDSHAKING_V2;
    if (connection_init_or_handshake_state(conn, started_here) < 0)
      return -1;
    if (!started_here) {
      connection_or_init_conn_from_address(conn, &conn->_base.addr,
                                           conn->_base.port, digest_rcvd, 0);
    }
    return connection_or_send_versions(conn, 0);
  }
}

/**
 * Called as client when initial TLS handshake is done, and we notice
 * that we got a v3-handshake signalling certificate from the server.
 * Set up structures, do bookkeeping, and send the versions cell.
 * Return 0 on success and -1 on failure.
 */
static int
connection_or_launch_v3_or_handshake(or_connection_t *conn)
{
  tor_assert(connection_or_nonopen_was_started_here(conn));
  tor_assert(tor_tls_received_v3_certificate(conn->tls));

  circuit_build_times_network_is_live(&circ_times);

  conn->_base.state = OR_CONN_STATE_OR_HANDSHAKING_V3;
  if (connection_init_or_handshake_state(conn, 1) < 0)
    return -1;

  return connection_or_send_versions(conn, 1);
}

/** Allocate a new connection handshake state for the connection
 * <b>conn</b>.  Return 0 on success, -1 on failure. */
int
connection_init_or_handshake_state(or_connection_t *conn, int started_here)
{
  or_handshake_state_t *s;
  s = conn->handshake_state = tor_malloc_zero(sizeof(or_handshake_state_t));
  s->started_here = started_here ? 1 : 0;
  s->digest_sent_data = 1;
  s->digest_received_data = 1;
  return 0;
}

/** Free all storage held by <b>state</b>. */
void
or_handshake_state_free(or_handshake_state_t *state)
{
  if (!state)
    return;
  crypto_free_digest_env(state->digest_sent);
  crypto_free_digest_env(state->digest_received);
  tor_cert_free(state->auth_cert);
  tor_cert_free(state->id_cert);
  memset(state, 0xBE, sizeof(or_handshake_state_t));
  tor_free(state);
}

/**
 * Remember that <b>cell</b> has been transmitted (if <b>incoming</b> is
 * false) or received (if <b>incoming is true) during a V3 handshake using
 * <b>state</b>.
 *
 * (We don't record the cell, but we keep a digest of everything sent or
 * received during the v3 handshake, and the client signs it in an
 * authenticate cell.)
 */
void
or_handshake_state_record_cell(or_handshake_state_t *state,
                               const cell_t *cell,
                               int incoming)
{
  crypto_digest_env_t *d, **dptr;
  packed_cell_t packed;
  if (incoming) {
    if (!state->digest_received_data)
      return;
  } else {
    if (!state->digest_sent_data)
      return;
  }
  if (!incoming) {
    log_warn(LD_BUG, "We shouldn't be sending any non-variable-length cells "
             "while making a handshake digest.  But we think we are sending "
             "one with type %d.", (int)cell->command);
  }
  dptr = incoming ? &state->digest_received : &state->digest_sent;
  if (! *dptr)
    *dptr = crypto_new_digest256_env(DIGEST_SHA256);

  d = *dptr;
  /* Re-packing like this is a little inefficient, but we don't have to do
     this very often at all. */
  cell_pack(&packed, cell);
  crypto_digest_add_bytes(d, packed.body, sizeof(packed.body));
  memset(&packed, 0, sizeof(packed));
}

/** Remember that a variable-length <b>cell</b> has been transmitted (if
 * <b>incoming</b> is false) or received (if <b>incoming is true) during a V3
 * handshake using <b>state</b>.
 *
 * (We don't record the cell, but we keep a digest of everything sent or
 * received during the v3 handshake, and the client signs it in an
 * authenticate cell.)
 */
void
or_handshake_state_record_var_cell(or_handshake_state_t *state,
                                   const var_cell_t *cell,
                                   int incoming)
{
  crypto_digest_env_t *d, **dptr;
  char buf[VAR_CELL_HEADER_SIZE];
  if (incoming) {
    if (!state->digest_received_data)
      return;
  } else {
    if (!state->digest_sent_data)
      return;
  }
  dptr = incoming ? &state->digest_received : &state->digest_sent;
  if (! *dptr)
    *dptr = crypto_new_digest256_env(DIGEST_SHA256);

  d = *dptr;

  var_cell_pack_header(cell, buf);
  crypto_digest_add_bytes(d, buf, sizeof(buf));
  crypto_digest_add_bytes(d, (const char *)cell->payload, cell->payload_len);

  memset(buf, 0, sizeof(buf));
}

/** Set <b>conn</b>'s state to OR_CONN_STATE_OPEN, and tell other subsystems
 * as appropriate.  Called when we are done with all TLS and OR handshaking.
 */
int
connection_or_set_state_open(or_connection_t *conn)
{
  int started_here = connection_or_nonopen_was_started_here(conn);
  time_t now = time(NULL);
  conn->_base.state = OR_CONN_STATE_OPEN;
  control_event_or_conn_status(conn, OR_CONN_EVENT_CONNECTED, 0);

  if (started_here) {
    circuit_build_times_network_is_live(&circ_times);
    rep_hist_note_connect_succeeded(conn->identity_digest, now);
    if (entry_guard_register_connect_status(conn->identity_digest,
                                            1, 0, now) < 0) {
      /* Close any circuits pending on this conn. We leave it in state
       * 'open' though, because it didn't actually *fail* -- we just
       * chose not to use it. (Otherwise
       * connection_about_to_close_connection() will call a big pile of
       * functions to indicate we shouldn't try it again.) */
      log_debug(LD_OR, "New entry guard was reachable, but closing this "
                "connection so we can retry the earlier entry guards.");
      circuit_n_conn_done(conn, 0);
      return -1;
    }
    router_set_status(conn->identity_digest, 1);
  } else {
    /* only report it to the geoip module if it's not a known router */
    if (!router_get_by_id_digest(conn->identity_digest)) {
      if (tor_addr_family(&TO_CONN(conn)->addr) == AF_INET) {
        /*XXXX IP6 support ipv6 geoip.*/
        uint32_t a = tor_addr_to_ipv4h(&TO_CONN(conn)->addr);
        geoip_note_client_seen(GEOIP_CLIENT_CONNECT, a, now);
      }
    }
  }

  or_handshake_state_free(conn->handshake_state);
  conn->handshake_state = NULL;
  IF_HAS_BUFFEREVENT(TO_CONN(conn), {
    connection_watch_events(TO_CONN(conn), READ_EVENT|WRITE_EVENT);
  }) ELSE_IF_NO_BUFFEREVENT {
    connection_start_reading(TO_CONN(conn));
  }

  circuit_n_conn_done(conn, 1); /* send the pending creates, if any. */

  return 0;
}

/** Pack <b>cell</b> into wire-format, and write it onto <b>conn</b>'s outbuf.
 * For cells that use or affect a circuit, this should only be called by
 * connection_or_flush_from_first_active_circuit().
 */
void
connection_or_write_cell_to_buf(const cell_t *cell, or_connection_t *conn)
{
  packed_cell_t networkcell;

  tor_assert(cell);
  tor_assert(conn);

  cell_pack(&networkcell, cell);

  connection_write_to_buf(networkcell.body, CELL_NETWORK_SIZE, TO_CONN(conn));

  if (conn->_base.state == OR_CONN_STATE_OR_HANDSHAKING_V3)
    or_handshake_state_record_cell(conn->handshake_state, cell, 0);

  if (cell->command != CELL_PADDING)
    conn->timestamp_last_added_nonpadding = approx_time();
}

/** Pack a variable-length <b>cell</b> into wire-format, and write it onto
 * <b>conn</b>'s outbuf.  Right now, this <em>DOES NOT</em> support cells that
 * affect a circuit.
 */
void
connection_or_write_var_cell_to_buf(const var_cell_t *cell,
                                    or_connection_t *conn)
{
  char hdr[VAR_CELL_HEADER_SIZE];
  tor_assert(cell);
  tor_assert(conn);
  var_cell_pack_header(cell, hdr);
  connection_write_to_buf(hdr, sizeof(hdr), TO_CONN(conn));
  connection_write_to_buf((char*)cell->payload,
                          cell->payload_len, TO_CONN(conn));
  if (conn->_base.state == OR_CONN_STATE_OR_HANDSHAKING_V3)
    or_handshake_state_record_var_cell(conn->handshake_state, cell, 0);
  if (cell->command != CELL_PADDING)
    conn->timestamp_last_added_nonpadding = approx_time();
}

/** See whether there's a variable-length cell waiting on <b>or_conn</b>'s
 * inbuf.  Return values as for fetch_var_cell_from_buf(). */
static int
connection_fetch_var_cell_from_buf(or_connection_t *or_conn, var_cell_t **out)
{
  connection_t *conn = TO_CONN(or_conn);
  IF_HAS_BUFFEREVENT(conn, {
    struct evbuffer *input = bufferevent_get_input(conn->bufev);
    return fetch_var_cell_from_evbuffer(input, out, or_conn->link_proto);
  }) ELSE_IF_NO_BUFFEREVENT {
    return fetch_var_cell_from_buf(conn->inbuf, out, or_conn->link_proto);
  }
}

/** Process cells from <b>conn</b>'s inbuf.
 *
 * Loop: while inbuf contains a cell, pull it off the inbuf, unpack it,
 * and hand it to command_process_cell().
 *
 * Always return 0.
 */
static int
connection_or_process_cells_from_inbuf(or_connection_t *conn)
{
  var_cell_t *var_cell;

  while (1) {
    log_debug(LD_OR,
              "%d: starting, inbuf_datalen %d (%d pending in tls object).",
              conn->_base.s,(int)connection_get_inbuf_len(TO_CONN(conn)),
              tor_tls_get_pending_bytes(conn->tls));
    if (connection_fetch_var_cell_from_buf(conn, &var_cell)) {
      if (!var_cell)
        return 0; /* not yet. */
      circuit_build_times_network_is_live(&circ_times);
      command_process_var_cell(var_cell, conn);
      var_cell_free(var_cell);
    } else {
      char buf[CELL_NETWORK_SIZE];
      cell_t cell;
      if (connection_get_inbuf_len(TO_CONN(conn))
          < CELL_NETWORK_SIZE) /* whole response available? */
        return 0; /* not yet */

      circuit_build_times_network_is_live(&circ_times);
      connection_fetch_from_buf(buf, CELL_NETWORK_SIZE, TO_CONN(conn));

      /* retrieve cell info from buf (create the host-order struct from the
       * network-order string) */
      cell_unpack(&cell, buf);

      command_process_cell(&cell, conn);
    }
  }
}

/** Write a destroy cell with circ ID <b>circ_id</b> and reason <b>reason</b>
 * onto OR connection <b>conn</b>.  Don't perform range-checking on reason:
 * we may want to propagate reasons from other cells.
 *
 * Return 0.
 */
int
connection_or_send_destroy(circid_t circ_id, or_connection_t *conn, int reason)
{
  cell_t cell;

  tor_assert(conn);

  memset(&cell, 0, sizeof(cell_t));
  cell.circ_id = circ_id;
  cell.command = CELL_DESTROY;
  cell.payload[0] = (uint8_t) reason;
  log_debug(LD_OR,"Sending destroy (circID %d).", circ_id);

  connection_or_write_cell_to_buf(&cell, conn);
  return 0;
}

/** Array of recognized link protocol versions. */
static const uint16_t or_protocol_versions[] = { 1, 2, 3 };
/** Number of versions in <b>or_protocol_versions</b>. */
static const int n_or_protocol_versions =
  (int)( sizeof(or_protocol_versions)/sizeof(uint16_t) );

/** Return true iff <b>v</b> is a link protocol version that this Tor
 * implementation believes it can support. */
int
is_or_protocol_version_known(uint16_t v)
{
  int i;
  for (i = 0; i < n_or_protocol_versions; ++i) {
    if (or_protocol_versions[i] == v)
      return 1;
  }
  return 0;
}

/** Send a VERSIONS cell on <b>conn</b>, telling the other host about the
 * link protocol versions that this Tor can support.
 *
 * If <b>v3_plus</b>, this is part of a V3 protocol handshake, so only
 * allow protocol version v3 or later.  If not <b>v3_plus</b>, this is
 * not part of a v3 protocol handshake, so don't allow protocol v3 or
 * later.
 **/
int
connection_or_send_versions(or_connection_t *conn, int v3_plus)
{
  var_cell_t *cell;
  int i;
  int n_versions = 0;
  const int min_version = v3_plus ? 3 : 0;
  const int max_version = v3_plus ? UINT16_MAX : 2;
  tor_assert(conn->handshake_state &&
             !conn->handshake_state->sent_versions_at);
  cell = var_cell_new(n_or_protocol_versions * 2);
  cell->command = CELL_VERSIONS;
  for (i = 0; i < n_or_protocol_versions; ++i) {
    uint16_t v = or_protocol_versions[i];
    if (v < min_version || v > max_version)
      continue;
    set_uint16(cell->payload+(2*n_versions), htons(v));
    ++n_versions;
  }
  cell->payload_len = n_versions * 2;

  connection_or_write_var_cell_to_buf(cell, conn);
  conn->handshake_state->sent_versions_at = time(NULL);

  var_cell_free(cell);
  return 0;
}

/** Send a NETINFO cell on <b>conn</b>, telling the other server what we know
 * about their address, our address, and the current time. */
int
connection_or_send_netinfo(or_connection_t *conn)
{
  cell_t cell;
  time_t now = time(NULL);
  const routerinfo_t *me;
  int len;
  uint8_t *out;

  tor_assert(conn->handshake_state);

  memset(&cell, 0, sizeof(cell_t));
  cell.command = CELL_NETINFO;

  /* Timestamp. */
  set_uint32(cell.payload, htonl((uint32_t)now));

  /* Their address. */
  out = cell.payload + 4;
  /* We use &conn->real_addr below, unless it hasn't yet been set. If it
   * hasn't yet been set, we know that _base.addr hasn't been tampered with
   * yet either. */
  len = append_address_to_payload(out, !tor_addr_is_null(&conn->real_addr)
                                       ? &conn->real_addr : &conn->_base.addr);
  if (len<0)
    return -1;
  out += len;

  /* My address -- only include it if I'm a public relay, or if I'm a
   * bridge and this is an incoming connection. If I'm a bridge and this
   * is an outgoing connection, act like a normal client and omit it. */
  if ((public_server_mode(get_options()) || !conn->is_outgoing) &&
      (me = router_get_my_routerinfo())) {
    tor_addr_t my_addr;
    *out++ = 1; /* only one address is supported. */

    tor_addr_from_ipv4h(&my_addr, me->addr);
    len = append_address_to_payload(out, &my_addr);
    if (len < 0)
      return -1;
  } else {
    *out = 0;
  }

  conn->handshake_state->digest_sent_data = 0;
  connection_or_write_cell_to_buf(&cell, conn);

  return 0;
}

/** Send a CERTS cell on the connection <b>conn</b>.  Return 0 on success, -1
 * on failure. */
int
connection_or_send_certs_cell(or_connection_t *conn)
{
  const tor_cert_t *link_cert = NULL, *id_cert = NULL;
  const uint8_t *link_encoded = NULL, *id_encoded = NULL;
  size_t link_len, id_len;
  var_cell_t *cell;
  size_t cell_len;
  ssize_t pos;
  int server_mode;

  tor_assert(conn->_base.state == OR_CONN_STATE_OR_HANDSHAKING_V3);

  if (! conn->handshake_state)
    return -1;
  server_mode = ! conn->handshake_state->started_here;
  if (tor_tls_get_my_certs(server_mode, &link_cert, &id_cert) < 0)
    return -1;
  tor_cert_get_der(link_cert, &link_encoded, &link_len);
  tor_cert_get_der(id_cert, &id_encoded, &id_len);

  cell_len = 1 /* 1 byte: num certs in cell */ +
             2 * ( 1 + 2 ) /* For each cert: 1 byte for type, 2 for length */ +
             link_len + id_len;
  cell = var_cell_new(cell_len);
  cell->command = CELL_CERTS;
  cell->payload[0] = 2;
  pos = 1;

  if (server_mode)
    cell->payload[pos] = OR_CERT_TYPE_TLS_LINK; /* Link cert  */
  else
    cell->payload[pos] = OR_CERT_TYPE_AUTH_1024; /* client authentication */
  set_uint16(&cell->payload[pos+1], htons(link_len));
  memcpy(&cell->payload[pos+3], link_encoded, link_len);
  pos += 3 + link_len;

  cell->payload[pos] = OR_CERT_TYPE_ID_1024; /* ID cert */
  set_uint16(&cell->payload[pos+1], htons(id_len));
  memcpy(&cell->payload[pos+3], id_encoded, id_len);
  pos += 3 + id_len;

  tor_assert(pos == (int)cell_len); /* Otherwise we just smashed the heap */

  connection_or_write_var_cell_to_buf(cell, conn);
  var_cell_free(cell);

  return 0;
}

/** Send an AUTH_CHALLENGE cell on the connection <b>conn</b>. Return 0
 * on success, -1 on failure. */
int
connection_or_send_auth_challenge_cell(or_connection_t *conn)
{
  var_cell_t *cell;
  uint8_t *cp;
  uint8_t challenge[OR_AUTH_CHALLENGE_LEN];
  tor_assert(conn->_base.state == OR_CONN_STATE_OR_HANDSHAKING_V3);

  if (! conn->handshake_state)
    return -1;

  if (crypto_rand((char*)challenge, OR_AUTH_CHALLENGE_LEN) < 0)
    return -1;
  cell = var_cell_new(OR_AUTH_CHALLENGE_LEN + 4);
  cell->command = CELL_AUTH_CHALLENGE;
  memcpy(cell->payload, challenge, OR_AUTH_CHALLENGE_LEN);
  cp = cell->payload + OR_AUTH_CHALLENGE_LEN;
  set_uint16(cp, htons(1)); /* We recognize one authentication type. */
  set_uint16(cp+2, htons(AUTHTYPE_RSA_SHA256_TLSSECRET));

  connection_or_write_var_cell_to_buf(cell, conn);
  var_cell_free(cell);
  memset(challenge, 0, sizeof(challenge));

  return 0;
}

/** Compute the main body of an AUTHENTICATE cell that a client can use
 * to authenticate itself on a v3 handshake for <b>conn</b>.  Write it to the
 * <b>outlen</b>-byte buffer at <b>out</b>.
 *
 * If <b>server</b> is true, only calculate the first
 * V3_AUTH_FIXED_PART_LEN bytes -- the part of the authenticator that's
 * determined by the rest of the handshake, and which match the provided value
 * exactly.
 *
 * If <b>server</b> is false and <b>signing_key</b> is NULL, calculate the
 * first V3_AUTH_BODY_LEN bytes of the authenticator (that is, everything
 * that should be signed), but don't actually sign it.
 *
 * If <b>server</b> is false and <b>signing_key</b> is provided, calculate the
 * entire authenticator, signed with <b>signing_key</b>.
 * DOCDOC return value
 */
int
connection_or_compute_authenticate_cell_body(or_connection_t *conn,
                                             uint8_t *out, size_t outlen,
                                             crypto_pk_env_t *signing_key,
                                             int server)
{
  uint8_t *ptr;

  /* assert state is reasonable XXXX */

  if (outlen < V3_AUTH_FIXED_PART_LEN ||
      (!server && outlen < V3_AUTH_BODY_LEN))
    return -1;

  ptr = out;

  /* Type: 8 bytes. */
  memcpy(ptr, "AUTH0001", 8);
  ptr += 8;

  {
    const tor_cert_t *id_cert=NULL, *link_cert=NULL;
    const digests_t *my_digests, *their_digests;
    const uint8_t *my_id, *their_id, *client_id, *server_id;
    if (tor_tls_get_my_certs(0, &link_cert, &id_cert))
      return -1;
    my_digests = tor_cert_get_id_digests(id_cert);
    their_digests = tor_cert_get_id_digests(conn->handshake_state->id_cert);
    tor_assert(my_digests);
    tor_assert(their_digests);
    my_id = (uint8_t*)my_digests->d[DIGEST_SHA256];
    their_id = (uint8_t*)their_digests->d[DIGEST_SHA256];

    client_id = server ? their_id : my_id;
    server_id = server ? my_id : their_id;

    /* Client ID digest: 32 octets. */
    memcpy(ptr, client_id, 32);
    ptr += 32;

    /* Server ID digest: 32 octets. */
    memcpy(ptr, server_id, 32);
    ptr += 32;
  }

  {
    crypto_digest_env_t *server_d, *client_d;
    if (server) {
      server_d = conn->handshake_state->digest_sent;
      client_d = conn->handshake_state->digest_received;
    } else {
      client_d = conn->handshake_state->digest_sent;
      server_d = conn->handshake_state->digest_received;
    }

    /* Server log digest : 32 octets */
    crypto_digest_get_digest(server_d, (char*)ptr, 32);
    ptr += 32;

    /* Client log digest : 32 octets */
    crypto_digest_get_digest(client_d, (char*)ptr, 32);
    ptr += 32;
  }

  {
    /* Digest of cert used on TLS link : 32 octets. */
    const tor_cert_t *cert = NULL;
    tor_cert_t *freecert = NULL;
    if (server) {
      tor_tls_get_my_certs(1, &cert, NULL);
    } else {
      freecert = tor_tls_get_peer_cert(conn->tls);
      cert = freecert;
    }
    if (!cert)
      return -1;
    memcpy(ptr, tor_cert_get_cert_digests(cert)->d[DIGEST_SHA256], 32);

    if (freecert)
      tor_cert_free(freecert);
    ptr += 32;
  }

  /* HMAC of clientrandom and serverrandom using master key : 32 octets */
  tor_tls_get_tlssecrets(conn->tls, ptr);
  ptr += 32;

  tor_assert(ptr - out == V3_AUTH_FIXED_PART_LEN);

  if (server)
    return V3_AUTH_FIXED_PART_LEN; // ptr-out

  /* Time: 8 octets. */
  {
    uint64_t now = time(NULL);
    if ((time_t)now < 0)
      return -1;
    set_uint32(ptr, htonl((uint32_t)(now>>32)));
    set_uint32(ptr+4, htonl((uint32_t)now));
    ptr += 8;
  }

  /* Nonce: 16 octets. */
  crypto_rand((char*)ptr, 16);
  ptr += 16;

  tor_assert(ptr - out == V3_AUTH_BODY_LEN);

  if (!signing_key)
    return V3_AUTH_BODY_LEN; // ptr - out

  {
    int siglen;
    char d[32];
    crypto_digest256(d, (char*)out, ptr-out, DIGEST_SHA256);
    siglen = crypto_pk_private_sign(signing_key,
                                    (char*)ptr, outlen - (ptr-out),
                                    d, 32);
    if (siglen < 0)
      return -1;

    ptr += siglen;
    tor_assert(ptr <= out+outlen);
    return (int)(ptr - out);
  }
}

/** Send an AUTHENTICATE cell on the connection <b>conn</b>.  Return 0 on
 * success, -1 on failure */
int
connection_or_send_authenticate_cell(or_connection_t *conn, int authtype)
{
  var_cell_t *cell;
  crypto_pk_env_t *pk = tor_tls_get_my_client_auth_key();
  int authlen;
  size_t cell_maxlen;
  /* XXXX make sure we're actually supposed to send this! */

  if (!pk) {
    log_warn(LD_BUG, "Can't compute authenticate cell: no client auth key");
    return -1;
  }
  if (authtype != AUTHTYPE_RSA_SHA256_TLSSECRET) {
    log_warn(LD_BUG, "Tried to send authenticate cell with unknown "
             "authentication type %d", authtype);
    return -1;
  }

  cell_maxlen = 4 + /* overhead */
    V3_AUTH_BODY_LEN + /* Authentication body */
    crypto_pk_keysize(pk) + /* Max signature length */
    16 /* add a few extra bytes just in case. */;

  cell = var_cell_new(cell_maxlen);
  cell->command = CELL_AUTHENTICATE;
  set_uint16(cell->payload, htons(AUTHTYPE_RSA_SHA256_TLSSECRET));
  /* skip over length ; we don't know that yet. */

  authlen = connection_or_compute_authenticate_cell_body(conn,
                                                         cell->payload+4,
                                                         cell_maxlen-4,
                                                         pk,
                                                         0 /* not server */);
  if (authlen < 0) {
    log_warn(LD_BUG, "Unable to compute authenticate cell!");
    var_cell_free(cell);
    return -1;
  }
  tor_assert(authlen + 4 <= cell->payload_len);
  set_uint16(cell->payload+2, htons(authlen));
  cell->payload_len = authlen + 4;

  connection_or_write_var_cell_to_buf(cell, conn);
  var_cell_free(cell);

  return 0;
}

