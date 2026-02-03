/*
 *  BIRD -- Shared Memory Export
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nest/bird.h"
#include "lib/ip.h"
#include "lib/lists.h"
#include "lib/string.h"
#include "lib/timer.h"
#include "nest/iface.h"
#include "nest/protocol.h"

#include "sysdep/unix/shm_export.h"

#ifdef CONFIG_OSPF
#include "lib/slists.h"
#include "proto/ospf/ospf.h"
#include "proto/ospf/topology.h"
#endif

#ifdef CONFIG_BGP
#include "proto/bgp/bgp.h"
#endif

#ifdef CONFIG_BFD
#include "proto/bfd/bfd.h"
#endif

#ifdef CONFIG_BABEL
#include "proto/babel/babel.h"
#endif

static struct bird_shm_region *shm_region;
static int shm_enabled;
static int shm_fd = -1;

static int
bird_shm_env_enabled(void)
{
  const char *val = getenv("ENABLE_SHM_EXPORT");
  if (!val || !*val)
    return 0;

  char *end = NULL;
  unsigned long parsed = strtoul(val, &end, 10);
  if ((end != NULL) && (*end != '\0'))
    return 0;

  return parsed != 0;
}

static inline void
bird_shm_set_ip(struct bird_shm_ip_addr *dst, ip_addr addr)
{
  memset(dst, 0, sizeof(*dst));

  if (ipa_zero(addr))
    return;

  if (ipa_is_ip4(addr))
  {
    dst->af = 4;
    put_ip4(dst->bytes, ipa_to_ip4(addr));
  }
  else if (ipa_is_ip6(addr))
  {
    dst->af = 6;
    put_ip6(dst->bytes, ipa_to_ip6(addr));
  }
}

static inline u32
bird_shm_proto_class(const struct proto *p)
{
  if (!p || !p->proto)
    return BIRD_SHM_PROTO_UNKNOWN;

  switch (p->proto->class)
  {
  case PROTOCOL_DEVICE: return BIRD_SHM_PROTO_DEVICE;
  case PROTOCOL_RADV: return BIRD_SHM_PROTO_RADV;
  case PROTOCOL_RIP: return BIRD_SHM_PROTO_RIP;
  case PROTOCOL_STATIC: return BIRD_SHM_PROTO_STATIC;
  case PROTOCOL_MRT: return BIRD_SHM_PROTO_MRT;
  case PROTOCOL_OSPF: return BIRD_SHM_PROTO_OSPF;
  case PROTOCOL_L3VPN: return BIRD_SHM_PROTO_L3VPN;
  case PROTOCOL_AGGREGATOR: return BIRD_SHM_PROTO_AGGREGATOR;
  case PROTOCOL_PIPE: return BIRD_SHM_PROTO_PIPE;
  case PROTOCOL_BGP: return BIRD_SHM_PROTO_BGP;
  case PROTOCOL_BMP: return BIRD_SHM_PROTO_BMP;
  case PROTOCOL_BFD: return BIRD_SHM_PROTO_BFD;
  case PROTOCOL_BABEL: return BIRD_SHM_PROTO_BABEL;
  case PROTOCOL_RPKI: return BIRD_SHM_PROTO_RPKI;
  default: return BIRD_SHM_PROTO_UNKNOWN;
  }
}

static void
bird_shm_write_snapshot(struct bird_shm_region *region)
{
  struct bird_shm_snapshot *snap = &region->snapshot;
  memset(snap, 0, sizeof(*snap));

  snap->version = BIRD_SHM_SNAPSHOT_VERSION;
  snap->last_cmd = region->mailbox.cmd;
  snap->status.boot_time = (u64) boot_time;
  snap->status.current_time = (u64) current_time();

  u32 iface_index = 0;
  u32 iface_addr_index = 0;

  struct iface *ifa;
  WALK_LIST(ifa, iface_list)
  {
    if (iface_index >= BIRD_SHM_MAX_INTERFACES)
    {
      snap->trunc_flags |= BIRD_SHM_TRUNC_IFACES;
      break;
    }

    struct bird_shm_iface *out = &snap->ifaces[iface_index];
    memset(out, 0, sizeof(*out));
    strncpy(out->name, ifa->name, sizeof(out->name) - 1);
    out->flags = ifa->flags;
    out->mtu = ifa->mtu;
    out->index = ifa->index;
    out->addr_start = iface_addr_index;
    out->addr_count = 0;

    struct ifa *addr;
    WALK_LIST(addr, ifa->addrs)
    {
      if (iface_addr_index >= BIRD_SHM_MAX_IFACE_ADDRS)
      {
        snap->trunc_flags |= BIRD_SHM_TRUNC_IFACE_ADDRS;
        break;
      }

      struct bird_shm_iface_addr *out_addr = &snap->iface_addrs[iface_addr_index];
      memset(out_addr, 0, sizeof(*out_addr));
      out_addr->iface_index = iface_index;
      out_addr->prefix_len = (u16) net_pxlen(&addr->prefix);
      out_addr->scope = (u16) addr->scope;
      out_addr->flags = addr->flags;
      bird_shm_set_ip(&out_addr->ip, addr->ip);
      bird_shm_set_ip(&out_addr->brd, addr->brd);
      bird_shm_set_ip(&out_addr->opposite, addr->opposite);

      iface_addr_index++;
      out->addr_count++;
    }

    iface_index++;
  }

  snap->iface_count = iface_index;
  snap->iface_addr_count = iface_addr_index;

  u32 proto_index = 0;
  u32 bgp_index = 0;
  u32 ospf_index = 0;
  u32 ospf_lsa_index = 0;
  u32 ospf_neigh_index = 0;
  u32 bfd_index = 0;
  u32 babel_index = 0;
  u32 babel_iface_index = 0;
  u32 babel_neigh_index = 0;

  struct proto *p;
  WALK_LIST(p, proto_list)
  {
    if (proto_index < BIRD_SHM_MAX_PROTOCOLS)
    {
      struct bird_shm_proto *out = &snap->protos[proto_index];
      memset(out, 0, sizeof(*out));
      strncpy(out->name, p->name, sizeof(out->name) - 1);
      out->class = bird_shm_proto_class(p);
      out->state = p->proto_state;
      proto_index++;
    }
    else
    {
      snap->trunc_flags |= BIRD_SHM_TRUNC_PROTOCOLS;
    }

#ifdef CONFIG_BGP
    if (p->proto && p->proto->class == PROTOCOL_BGP)
    {
      if (bgp_index >= BIRD_SHM_MAX_BGP)
      {
        snap->trunc_flags |= BIRD_SHM_TRUNC_BGP;
      }
      else
      {
        struct bgp_proto *bp = (struct bgp_proto *) p;
        struct bird_shm_bgp_info *out = &snap->bgp[bgp_index];
        memset(out, 0, sizeof(*out));
        strncpy(out->name, p->name, sizeof(out->name) - 1);
        out->local_as = bp->local_as;
        out->remote_as = bp->remote_as;
        out->conn_state = bp->conn ? bp->conn->state : 0;
        bird_shm_set_ip(&out->remote_ip, bp->remote_ip);
        bgp_index++;
      }
    }
#endif

#ifdef CONFIG_OSPF
    if (p->proto && p->proto->class == PROTOCOL_OSPF)
    {
      if (ospf_index >= BIRD_SHM_MAX_OSPF)
      {
        snap->trunc_flags |= BIRD_SHM_TRUNC_OSPF;
      }
      else
      {
        struct ospf_proto *op = (struct ospf_proto *) p;
        struct bird_shm_ospf_info *out = &snap->ospf[ospf_index];
        memset(out, 0, sizeof(*out));
        strncpy(out->name, p->name, sizeof(out->name) - 1);
        out->router_id = op->router_id;
        out->version = (u8) ospf_get_version(op);
        out->lsa_start = ospf_lsa_index;
        out->neigh_start = ospf_neigh_index;
        out->lsa_count = 0;
        out->neigh_count = 0;

        struct top_hash_entry *en;
        WALK_SLIST(en, op->lsal)
        {
          if (!en->lsa_body || (en->lsa_body == LSA_BODY_DUMMY))
            continue;

          if (ospf_lsa_index >= BIRD_SHM_MAX_OSPF_LSAS)
          {
            snap->trunc_flags |= BIRD_SHM_TRUNC_OSPF_LSAS;
            break;
          }

          struct bird_shm_ospf_lsa *lsa = &snap->ospf_lsas[ospf_lsa_index];
          memset(lsa, 0, sizeof(*lsa));
          lsa->proto_index = ospf_index;
          lsa->lsa_type = en->lsa_type;
          lsa->domain = en->domain;
          lsa->id = en->lsa.id;
          lsa->rt = en->lsa.rt;
          lsa->sn = en->lsa.sn;
          lsa->age = en->lsa.age;
          lsa->length = en->lsa.length;
          lsa->type_raw = en->lsa.type_raw;
          ospf_lsa_index++;
          out->lsa_count++;
        }

        struct ospf_iface *oi;
        int neigh_trunc = 0;
        WALK_LIST(oi, op->iface_list)
        {
          struct ospf_neighbor *on;
          WALK_LIST(on, oi->neigh_list)
          {
            if (ospf_neigh_index >= BIRD_SHM_MAX_OSPF_NEIGHBORS)
            {
              snap->trunc_flags |= BIRD_SHM_TRUNC_OSPF_NEIGHS;
              neigh_trunc = 1;
              break;
            }

            struct bird_shm_ospf_neighbor *n = &snap->ospf_neighs[ospf_neigh_index];
            memset(n, 0, sizeof(*n));
            n->proto_index = ospf_index;
            if (oi->ifname)
              strncpy(n->ifname, oi->ifname, sizeof(n->ifname) - 1);
            else if (oi->iface)
              strncpy(n->ifname, oi->iface->name, sizeof(n->ifname) - 1);
            n->rid = on->rid;
            n->state = on->state;
            bird_shm_set_ip(&n->ip, on->ip);

            ospf_neigh_index++;
            out->neigh_count++;
          }
          if (neigh_trunc)
            break;
        }

        ospf_index++;
      }
    }
#endif

#ifdef CONFIG_BFD
    if (p->proto && p->proto->class == PROTOCOL_BFD)
    {
      struct bfd_proto *bp = (struct bfd_proto *) p;
      bfd_lock_sessions(bp);
      HASH_WALK(bp->session_hash_id, next_id, s)
      {
        if (bfd_index >= BIRD_SHM_MAX_BFD_SESSIONS)
        {
          snap->trunc_flags |= BIRD_SHM_TRUNC_BFD;
          break;
        }

        struct bird_shm_bfd_session *out = &snap->bfd[bfd_index];
        memset(out, 0, sizeof(*out));
        bird_shm_set_ip(&out->addr, s->addr);
        if (s->ifa && s->ifa->iface)
          strncpy(out->ifname, s->ifa->iface->name, sizeof(out->ifname) - 1);
        out->state = s->loc_state;
        out->rem_state = s->rem_state;
        out->local_disc = s->loc_id;
        out->remote_disc = s->rem_id;
        bfd_index++;
      }
      HASH_WALK_END;
      bfd_unlock_sessions(bp);
    }
#endif

#ifdef CONFIG_BABEL
    if (p->proto && p->proto->class == PROTOCOL_BABEL)
    {
      if (babel_index >= BIRD_SHM_MAX_BABEL)
      {
        snap->trunc_flags |= BIRD_SHM_TRUNC_BABEL;
      }
      else
      {
        struct babel_proto *bp = (struct babel_proto *) p;
        struct bird_shm_babel_info *out = &snap->babel[babel_index];
        memset(out, 0, sizeof(*out));
        strncpy(out->name, p->name, sizeof(out->name) - 1);
        out->router_id = bp->router_id;
        out->update_seqno = bp->update_seqno;
        out->triggered = bp->triggered;
        out->iface_start = babel_iface_index;
        out->iface_count = 0;
        out->neigh_count = 0;

        struct babel_iface *bi;
        WALK_LIST(bi, bp->interfaces)
        {
          if (babel_iface_index >= BIRD_SHM_MAX_BABEL_IFACES)
          {
            snap->trunc_flags |= BIRD_SHM_TRUNC_BABEL_IFACES;
            break;
          }

          struct bird_shm_babel_iface *out_iface = &snap->babel_ifaces[babel_iface_index];
          memset(out_iface, 0, sizeof(*out_iface));
          out_iface->proto_index = babel_index;
          if (bi->ifname)
            strncpy(out_iface->ifname, bi->ifname, sizeof(out_iface->ifname) - 1);
          else if (bi->iface)
            strncpy(out_iface->ifname, bi->iface->name, sizeof(out_iface->ifname) - 1);
          out_iface->up = bi->up;
          out_iface->tx_length = bi->tx_length;
          out_iface->hello_seqno = bi->hello_seqno;
          out_iface->neigh_start = babel_neigh_index;
          out_iface->neigh_count = 0;
          bird_shm_set_ip(&out_iface->addr, bi->addr);
          bird_shm_set_ip(&out_iface->next_hop_ip4, bi->next_hop_ip4);
          bird_shm_set_ip(&out_iface->next_hop_ip6, bi->next_hop_ip6);

          struct babel_neighbor *bn;
          int neigh_trunc = 0;
          WALK_LIST(bn, bi->neigh_list)
          {
            if (babel_neigh_index >= BIRD_SHM_MAX_BABEL_NEIGHBORS)
            {
              snap->trunc_flags |= BIRD_SHM_TRUNC_BABEL_NEIGHS;
              neigh_trunc = 1;
              break;
            }

            struct bird_shm_babel_neighbor *out_neigh = &snap->babel_neighs[babel_neigh_index];
            memset(out_neigh, 0, sizeof(*out_neigh));
            out_neigh->iface_index = babel_iface_index;
            out_neigh->rxcost = bn->rxcost;
            out_neigh->txcost = bn->txcost;
            out_neigh->cost = bn->cost;
            out_neigh->hello_cnt = bn->hello_cnt;
            out_neigh->last_hello_int = bn->last_hello_int;
            out_neigh->last_tstamp = bn->last_tstamp;
            out_neigh->srtt = (u64) bn->srtt;
            out_neigh->hello_expiry = (u64) bn->hello_expiry;
            out_neigh->ihu_expiry = (u64) bn->ihu_expiry;
            bird_shm_set_ip(&out_neigh->addr, bn->addr);

            babel_neigh_index++;
            out_iface->neigh_count++;
          }

          out->iface_count++;
          out->neigh_count += out_iface->neigh_count;
          babel_iface_index++;

          if (neigh_trunc)
            break;
        }

        babel_index++;
      }
    }
#endif
  }

  snap->proto_count = proto_index;
  snap->bgp_count = bgp_index;
  snap->ospf_count = ospf_index;
  snap->ospf_lsa_count = ospf_lsa_index;
  snap->ospf_neigh_count = ospf_neigh_index;
  snap->bfd_count = bfd_index;
  snap->babel_count = babel_index;
  snap->babel_iface_count = babel_iface_index;
  snap->babel_neigh_count = babel_neigh_index;
}

void
bird_shm_init(void)
{
  if (!bird_shm_env_enabled())
    return;

  shm_fd = shm_open(BIRD_SHM_NAME, O_CREAT | O_RDWR, 0600);
  if (shm_fd < 0)
  {
    log(L_WARN "SHM export: shm_open failed: %m");
    return;
  }

  size_t size = sizeof(struct bird_shm_region);
  if (ftruncate(shm_fd, size) < 0)
  {
    log(L_WARN "SHM export: ftruncate failed: %m");
    close(shm_fd);
    shm_fd = -1;
    return;
  }

  void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (addr == MAP_FAILED)
  {
    log(L_WARN "SHM export: mmap failed: %m");
    close(shm_fd);
    shm_fd = -1;
    return;
  }

  shm_region = (struct bird_shm_region *) addr;
  shm_enabled = 1;
  memset(shm_region, 0, size);
  shm_region->magic = BIRD_SHM_MAGIC;
  shm_region->version = BIRD_SHM_VERSION;
  shm_region->snapshot.version = BIRD_SHM_SNAPSHOT_VERSION;
  __atomic_store_n(&shm_region->cmd_flag, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&shm_region->version_seq, 0, __ATOMIC_RELEASE);

  log(L_INFO "SHM export enabled");
}

void
bird_shm_poll(void)
{
  if (!shm_enabled || !shm_region)
    return;

  u32 cmd_flag = __atomic_load_n(&shm_region->cmd_flag, __ATOMIC_ACQUIRE);
  if (!cmd_flag)
    return;

  u32 cmd = shm_region->mailbox.cmd;
  if (cmd != BIRD_SHM_CMD_SNAPSHOT)
  {
    __atomic_store_n(&shm_region->cmd_flag, 0, __ATOMIC_RELEASE);
    return;
  }

  __atomic_fetch_add(&shm_region->version_seq, 1, __ATOMIC_ACQ_REL);
  bird_shm_write_snapshot(shm_region);
  __atomic_fetch_add(&shm_region->version_seq, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&shm_region->cmd_flag, 0, __ATOMIC_RELEASE);
}
