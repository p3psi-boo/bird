/*
 *	BIRD -- Simple Network Management Protocol (SNMP)
 *
 *      (c) 2022 Vojtech Vilimek <vojtech.vilimek@nic.cz>
 *      (c) 2022 CZ.NIC z.s.p.o.
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * Simple Network Management Protocol State Machine
 *
 *  States with main transitions
 *
 *
 *    +-----------------+
 *    | SNMP_INIT	|     entry state after call snmp_start()
 *    +-----------------+
 *	  |
 *	  |   acquiring object lock for communication socket
 *	  V
 *    +-----------------+
 *    | SNMP_LOCKED	|     object lock aquired
 *    +-----------------+
 *	  |
 *	  |   opening communaiton socket
 *	  V
 *    +-----------------+
 *    | SNMP_OPEN	|     socket created, starting subagent
 *    +-----------------+
 *	  |
 *	  |   BIRD recieve response for Open-PDU
 *	  V
 *    +-----------------+
 *    | SNMP_REGISTER   |     session was established, subagent registers MIBs
 *    +-----------------+
 *	  |
 *	  |   subagent recieved responses for all registration requests
 *	  V
 *    +-----------------+
 *    | SNMP_CONN	|     everything is set
 *    +-----------------+
 *	  |
 * 	  |   function snmp_shutdown() is called, BIRD sends Close-PDU
 *	  V
 *    +-----------------+
 *    | SNMP_STOP	|     waiting for response
 *    +-----------------+
 *	  |
 *	  |   cleaning old state information
 *	  V
 *    +-----------------+
 *    | SNMP_DOWN	|     session is closed
 *    +-----------------+
 *
 *
 *  Erroneous transitions:
 *    SNMP is UP in states SNMP_CONN and also in SNMP_REGISTER because the
 *    session is establised and the GetNext request should be responsed
 *    without regard to MIB registration.
 *
 *    When the session has been closed for some reason (socket error, reciept of
 *    Close-PDU) SNMP cleans the session information and message queue and goes
 *    back to the SNMP_LOCKED state.
 *
 *    Reconfiguration is done in similar fashion to BGP, the reconfiguration
 *    request is declined, the protocols is stoped and started with new
 *    configuration.
 *
 */

#include "nest/bird.h"
#include "nest/cli.h"
#include "nest/locks.h"
#include "lib/socket.h"
#include "lib/lists.h"

#include "snmp.h"
#include "subagent.h"
#include "snmp_utils.h"

static const char * const snmp_state[] = {
  [SNMP_ERR]	    = "SNMP ERROR",
  [SNMP_INIT]	    = "SNMP INIT",
  [SNMP_LOCKED]	    = "SNMP LOCKED",
  [SNMP_OPEN]	    = "SNMP CONNECTION OPENED",
  [SNMP_REGISTER]   = "SNMP REGISTERING MIBS",
  [SNMP_CONN]	    = "SNMP CONNECTED",
  [SNMP_STOP]	    = "SNMP STOPPING",
  [SNMP_DOWN]	    = "SNMP DOWN",
};

static struct proto *
snmp_init(struct proto_config *CF)
{
  struct proto *P = proto_new(CF);
  struct snmp_proto *p = SKIP_BACK(struct snmp_proto, p, P);
  const struct snmp_config *cf = SKIP_BACK(struct snmp_config, cf, CF);

  p->rl_gen = (struct tbf) TBF_DEFAULT_LOG_LIMITS;

  p->local_ip = cf->local_ip;
  p->remote_ip = cf->remote_ip;
  p->local_port = cf->local_port;
  p->remote_port = cf->remote_port;
  p->local_as = cf->local_as;
  snmp_log("changing proto_snmp state to INIT");
  p->state = SNMP_INIT;

  // p->timeout = cf->timeout;
  p->timeout = 15;

  snmp_log("snmp_reconfigure() lip: %I:%u rip: %I:%u",
    cf->local_ip, cf->local_port, cf->remote_ip, cf->remote_port);

  return P;
}

static inline void
snmp_cleanup(struct snmp_proto *p)
{
  rfree(p->startup_timer);
  p->startup_timer = NULL;

  rfree(p->ping_timer);
  p->ping_timer = NULL;

  rfree(p->sock);
  p->sock = NULL;

  rfree(p->lock);
  p->lock = NULL;

  p->state = SNMP_DOWN;
}

void
snmp_down(struct snmp_proto *p)
{
  snmp_cleanup(p);

  proto_notify_state(&p->p, PS_DOWN);
}

static void
snmp_sock_err(sock *sk, int err)
{
  snmp_log("snmp_sock_err() %s - err no: %d",  strerror(err), err);
  struct snmp_proto *p = sk->data;
  p->errs++;

  tm_stop(p->ping_timer);

  rfree(p->sock);
  p->sock = NULL;

  rfree(p->lock);
  p->lock = NULL;

  snmp_log("changing proto_snmp state to ERR[OR]");
  p->state = SNMP_ERR;
  //  snmp_shutdown((struct proto *) p);

  // TODO ping interval
  tm_start(p->startup_timer, 4 S);
}

static void
snmp_connected(sock *sk)
{
  struct snmp_proto *p = sk->data;
  snmp_log("snmp_connected() connection created");

  p->state = SNMP_OPEN;

  byte *buf UNUSED = sk->rpos;

  sk->rx_hook = snmp_rx;
  sk->tx_hook = NULL;
  //sk->tx_hook = snmp_tx;

  snmp_start_subagent(p);

  // TODO ping interval
  tm_set(p->ping_timer, 15 S);
}

static void
snmp_start_locked(struct object_lock *lock)
{
  snmp_log("snmp_start_locked() - lock acquired; preparing socket");
  struct snmp_proto *p = lock->data;

  p->state = SNMP_LOCKED;

  sock *s = sk_new(p->p.pool);
  s->type = SK_TCP_ACTIVE;
  s->saddr = p->local_ip;
  s->daddr = p->remote_ip;
  s->dport = p->remote_port;
  s->rbsize = SNMP_RX_BUFFER_SIZE;
  s->tbsize = SNMP_TX_BUFFER_SIZE;

  //s->tos = IP_PREC_INTERNET_CONTROL
  //s->rx_hook = snmp_connected;
  s->tx_hook = snmp_connected;
  s->err_hook = snmp_sock_err;

  //mb_free(p->sock);
  p->sock = s;
  s->data = p;

  p->to_send = 0;
  p->errs = 0;

  if (sk_open(s) < 0)
  {
    log(L_ERR "Cannot open listening socket");
    snmp_down(p);
  }

  snmp_log("socket ready!, trying to connect");
}

static void
snmp_startup(struct snmp_proto *p)
{
  //snmp_log("changing proto_snmp state to INIT");

  if (p->state == SNMP_LOCKED ||
      p->state == SNMP_OPEN ||
      p->state == SNMP_REGISTER ||
      p->state == SNMP_CONN)
  {
    snmp_log("startup() already in connected state %u", p->state);
    return;
  }

  snmp_log("snmp_startup()");
  p->state = SNMP_INIT;

  /* starting agentX communicaiton channel */

  snmp_log("preparing lock");
  struct object_lock *lock;

  /* we could have the lock already acquired but be in ERROR state */
  lock = p->lock = olock_new(p->p.pool);

  // lock->addr
  // lock->port
  // lock->iface
  // lock->vrf
  lock->type = OBJLOCK_TCP;
  lock->hook = snmp_start_locked;
  lock->data = p;

  snmp_log("lock acquiring");
  olock_acquire(lock);

  /*
  snmp_log("local ip: %I:%u, remote ip: %I:%u",
    p->local_ip, p->local_port, p->remote_ip, p->remote_port);
  */
}

static void
snmp_startup_timeout(timer *t)
{
  snmp_log("startup timer triggered");
  snmp_startup(t->data);
}

static void
snmp_stop_timeout(timer *t)
{
  snmp_log("stop timer triggered");

  struct snmp_proto *p = t->data;

  snmp_down(p);
}

static void
snmp_ping_timer(struct timer *tm)
{
  // snmp_log("snmp_ping_timer() ");
  struct snmp_proto *p = tm->data;

  if (p->state == SNMP_CONN)
  {
    snmp_ping(p);
  }

  //tm_set(tm, current_time() + (15 S));
  tm_set(tm, current_time() + 15 S);
}

static int
snmp_start(struct proto *P)
{
  snmp_log("snmp_start() - starting timer (almost)");
  struct snmp_proto *p = (void *) P;
  struct snmp_config *cf = (struct snmp_config *) P->cf;

  p->startup_timer = tm_new_init(p->p.pool, snmp_startup_timeout, p, 0, 0);

  p->to_send = 0;
  p->errs = 0;

  p->pool = lp_new(p->p.pool);
  p->bgp_trie = f_new_trie(p->pool, cf->bonds);

  init_list(&p->register_queue);
  init_list(&p->bgp_registered);
  p->partial_response = NULL;

  p->ping_timer = tm_new_init(p->p.pool, snmp_ping_timer, p, 0, 0);
  // tm_set(p->ping_timer, current_time() + 2 S);

  /* create copy of bonds to bgp */
  HASH_INIT(p->bgp_hash, p->p.pool, 10);

  struct snmp_bond *b;
  WALK_LIST(b, cf->bgp_entries)
  {
    struct bgp_config *bc = (struct bgp_config *) b->proto;
    if (bc && !ipa_zero(bc->remote_ip))
    {
      struct snmp_bgp_peer *peer =
	mb_allocz(p->p.pool, sizeof(struct snmp_bgp_peer));
      peer->config = (struct bgp_config *) b->proto;
      peer->peer_ip = bc->remote_ip;

      struct net_addr net;
      net_fill_ip4(&net, ipa_to_ip4(peer->peer_ip), IP4_MAX_PREFIX_LENGTH);

      trie_add_prefix(p->bgp_trie, &net, IP4_MAX_PREFIX_LENGTH, IP4_MAX_PREFIX_LENGTH);

      HASH_INSERT(p->bgp_hash, SNMP_HASH, peer);
    }
  }

  snmp_startup(p);
  return PS_START;
}

static int
snmp_reconfigure(struct proto *P, struct proto_config *CF)
{
  struct snmp_proto *p = SKIP_BACK(struct snmp_proto, p, P);
  const struct snmp_config *cf = SKIP_BACK(struct snmp_config, cf, CF);

  p->local_ip = cf->local_ip;
  p->remote_ip = cf->remote_ip;
  p->local_port = cf->local_port;
  p->remote_port = cf->remote_port;
  p->local_as = cf->local_as;
  p->timeout = 15;

  /* workaround to make the registration happen */
  p->register_to_ack = 1;

  /* TODO walk all bind protocols and find their (new) IP
    to update HASH table */
  snmp_log("snmp_reconfigure() lip: %I:%u rip: %I:%u",
    p->local_ip, p->local_port, p->remote_ip, p->remote_port);

  return 1;
}

static void
snmp_show_proto_info(struct proto *P)
{
  struct snmp_proto *sp = (void *) P;
  struct snmp_config *c = (void *) P->cf;

  cli_msg(-1006, "");
  cli_msg(-1006, " snmp status %s", snmp_state[sp->state]);
  cli_msg(-1006, "");
  cli_msg(-1006, "  BGP peers");
  struct snmp_bond *bond;
  WALK_LIST(bond, c->bgp_entries)
  {
    struct proto_config *cf = P->cf;
    struct bgp_config *bcf = (struct bgp_config *) cf;
    struct proto *p = cf->proto;
    struct bgp_proto *bp = (struct bgp_proto *) cf->proto;
    struct bgp_conn *conn = bp->conn;

    cli_msg(-1006, "    name: %s", cf->name);
    cli_msg(-1006, "");
    cli_msg(-1006, "	loc. identifier: %I4", bp->local_id);
    cli_msg(-1006, "    rem. identifier: %I4", bp->remote_id);
    cli_msg(-1006, "    admin status: %s", (p->disabled) ? "stop" :
	      "start");
    cli_msg(-1006, "    version: 4");
    cli_msg(-1006, "    local ip: %I4", bcf->local_ip);
    cli_msg(-1006, "    remote ip: %I4", bcf->remote_ip);
    cli_msg(-1006, "    local port: %I4", bcf->local_port);
    cli_msg(-1006, "    remote port: %I4", bcf->remote_port);
    /*
    if (conn) {
      cli_msg(-1006, "    state: %u", conn->state);
      cli_msg(-1006, "    remote as: %u", conn->remote_caps->as4_number);
    }
    */
    cli_msg(-1006, "    in updates: %u", bp->stats.rx_updates);
    cli_msg(-1006, "    out updates: %u", bp->stats.tx_updates);
    cli_msg(-1006, "    in total: %u", bp->stats.rx_messages);
    cli_msg(-1006, "    out total: %u", bp->stats.tx_messages);
    cli_msg(-1006, "    fsm transitions: %u",
bp->stats.fsm_established_transitions);

    cli_msg(-1006, "    fsm total time: -- (0)");
    cli_msg(-1006, "    retry interval: %u", bcf->connect_retry_time);

    /*
    if (conn) {
      cli_msg(-1006, "    hold time: %u", conn->hold_time);
      cli_msg(-1006, "    keep alive: %u", conn->keepalive_time );
    }
    */

    cli_msg(-1006, "    hold configurated: %u", bcf->hold_time );
    cli_msg(-1006, "    keep alive config: %u", bcf->keepalive_time );

    cli_msg(-1006, "    min AS origin. int.: -- (0)");
    cli_msg(-1006, "    min route advertisement: %u", 0 );
    cli_msg(-1006, "    in update elapsed time: %u", 0 );

    if (!conn)
      cli_msg(-1006, "  no default connection");

    cli_msg(-1006, "  outgoinin_conn state %u", bp->outgoing_conn.state + 1);
    cli_msg(-1006, "  incoming_conn state: %u", bp->incoming_conn.state + 1);
  }
}

static void
snmp_postconfig(struct proto_config *CF)
{
  // walk the bgp protocols and cache their references
  if (((struct snmp_config *) CF)->local_as == 0)
    cf_error("local as not specified");
}

static int
snmp_shutdown(struct proto *P)
{
  snmp_log("snmp_shutdown()");
  struct snmp_proto *p = SKIP_BACK(struct snmp_proto, p, P);

  tm_stop(p->ping_timer);

  /* connection established -> close the connection */
  if (p->state == SNMP_REGISTER ||
      p->state == SNMP_CONN)
  {
    p->state = SNMP_STOP;

    /* startup time is reused for connection closing */
    p->startup_timer->hook = snmp_stop_timeout;

    // TODO timeout option
    tm_set(p->startup_timer, 15 S);

    snmp_stop_subagent(p);

    return PS_STOP;
  }
  /* no connection to close */
  else
  {
    snmp_cleanup(p);
    return PS_DOWN;
  }
}

struct protocol proto_snmp = {
  .name =		"Snmp",
  .template =		"snmp%d",
  .channel_mask =	NB_ANY,
  .proto_size =		sizeof(struct snmp_proto),
  .config_size =	sizeof(struct snmp_config),
  .postconfig =		snmp_postconfig,
  .init =		snmp_init,
  .start =		snmp_start,
  .reconfigure =	snmp_reconfigure,
  .shutdown =		snmp_shutdown,
  .show_proto_info = 	snmp_show_proto_info,
};

void
snmp_build(void)
{
  proto_build(&proto_snmp);
}
