/*
 *  BIRD -- Shared Memory Export
 *
 *  Shared memory layout and export helpers.
 */

#ifndef _BIRD_SHM_EXPORT_H_
#define _BIRD_SHM_EXPORT_H_

#include "sysdep/config.h"

#define BIRD_SHM_NAME "/bird_shm_export"
#define BIRD_SHM_MAGIC 0x42524453u /* 'BRDS' */
#define BIRD_SHM_VERSION 1u
#define BIRD_SHM_SNAPSHOT_VERSION 1u

#define BIRD_SHM_CMD_SNAPSHOT 1u

#define BIRD_SHM_MAX_INTERFACES 512u
#define BIRD_SHM_MAX_IFACE_ADDRS 2048u
#define BIRD_SHM_MAX_PROTOCOLS 512u
#define BIRD_SHM_MAX_BGP 512u
#define BIRD_SHM_MAX_OSPF 64u
#define BIRD_SHM_MAX_OSPF_LSAS 2048u
#define BIRD_SHM_MAX_OSPF_NEIGHBORS 1024u
#define BIRD_SHM_MAX_BFD_SESSIONS 512u
#define BIRD_SHM_MAX_BABEL 64u
#define BIRD_SHM_MAX_BABEL_IFACES 256u
#define BIRD_SHM_MAX_BABEL_NEIGHBORS 1024u

#define BIRD_SHM_TRUNC_IFACES        (1u << 0)
#define BIRD_SHM_TRUNC_IFACE_ADDRS   (1u << 1)
#define BIRD_SHM_TRUNC_PROTOCOLS     (1u << 2)
#define BIRD_SHM_TRUNC_BGP           (1u << 3)
#define BIRD_SHM_TRUNC_OSPF          (1u << 4)
#define BIRD_SHM_TRUNC_OSPF_LSAS     (1u << 5)
#define BIRD_SHM_TRUNC_OSPF_NEIGHS   (1u << 6)
#define BIRD_SHM_TRUNC_BFD           (1u << 7)
#define BIRD_SHM_TRUNC_BABEL         (1u << 8)
#define BIRD_SHM_TRUNC_BABEL_IFACES  (1u << 9)
#define BIRD_SHM_TRUNC_BABEL_NEIGHS  (1u << 10)

enum bird_shm_proto_class {
  BIRD_SHM_PROTO_DEVICE = 1,
  BIRD_SHM_PROTO_RADV,
  BIRD_SHM_PROTO_RIP,
  BIRD_SHM_PROTO_STATIC,
  BIRD_SHM_PROTO_MRT,
  BIRD_SHM_PROTO_OSPF,
  BIRD_SHM_PROTO_L3VPN,
  BIRD_SHM_PROTO_AGGREGATOR,
  BIRD_SHM_PROTO_PIPE,
  BIRD_SHM_PROTO_BGP,
  BIRD_SHM_PROTO_BMP,
  BIRD_SHM_PROTO_BFD,
  BIRD_SHM_PROTO_BABEL,
  BIRD_SHM_PROTO_RPKI,
  BIRD_SHM_PROTO_UNKNOWN = 255
};

struct bird_shm_ip_addr {
  u8 af;        /* 0 = none, 4 = IPv4, 6 = IPv6 */
  u8 pad[3];
  u8 bytes[16];
};

struct bird_shm_status {
  u64 boot_time;
  u64 current_time;
};

struct bird_shm_iface {
  char name[16];
  u32 flags;
  u32 mtu;
  u32 index;
  u32 addr_start;
  u32 addr_count;
};

struct bird_shm_iface_addr {
  u32 iface_index;
  u16 prefix_len;
  u16 scope;
  u32 flags;
  struct bird_shm_ip_addr ip;
  struct bird_shm_ip_addr brd;
  struct bird_shm_ip_addr opposite;
};

struct bird_shm_proto {
  char name[32];
  u32 class;
  u32 state;
};

struct bird_shm_bgp_info {
  char name[32];
  u32 local_as;
  u32 remote_as;
  u8 conn_state;
  u8 pad[3];
  struct bird_shm_ip_addr remote_ip;
};

struct bird_shm_ospf_info {
  char name[32];
  u32 router_id;
  u8 version;
  u8 pad[3];
  u32 lsa_start;
  u32 lsa_count;
  u32 neigh_start;
  u32 neigh_count;
};

struct bird_shm_ospf_lsa {
  u32 proto_index;
  u32 lsa_type;
  u32 domain;
  u32 id;
  u32 rt;
  s32 sn;
  u16 age;
  u16 length;
  u16 type_raw;
  u16 pad;
};

struct bird_shm_ospf_neighbor {
  u32 proto_index;
  char ifname[16];
  u32 rid;
  u8 state;
  u8 pad[3];
  struct bird_shm_ip_addr ip;
};

struct bird_shm_bfd_session {
  struct bird_shm_ip_addr addr;
  char ifname[16];
  u8 state;
  u8 rem_state;
  u8 pad[2];
  u32 local_disc;
  u32 remote_disc;
};

struct bird_shm_babel_info {
  char name[32];
  u64 router_id;
  u32 update_seqno;
  u8 triggered;
  u8 pad[3];
  u32 iface_start;
  u32 iface_count;
  u32 neigh_count;
};

struct bird_shm_babel_iface {
  u32 proto_index;
  char ifname[16];
  u8 up;
  u8 pad[3];
  u32 tx_length;
  u16 hello_seqno;
  u16 pad2;
  u32 neigh_start;
  u32 neigh_count;
  struct bird_shm_ip_addr addr;
  struct bird_shm_ip_addr next_hop_ip4;
  struct bird_shm_ip_addr next_hop_ip6;
};

struct bird_shm_babel_neighbor {
  u32 iface_index;
  u16 rxcost;
  u16 txcost;
  u16 cost;
  u8 hello_cnt;
  u8 pad;
  u32 last_hello_int;
  u32 last_tstamp;
  u64 srtt;
  u64 hello_expiry;
  u64 ihu_expiry;
  struct bird_shm_ip_addr addr;
};

struct bird_shm_mailbox {
  u32 cmd;
  u32 reserved;
  u64 arg0;
  u64 arg1;
};

struct bird_shm_snapshot {
  u32 version;
  u32 trunc_flags;
  u64 last_cmd;
  struct bird_shm_status status;

  u32 iface_count;
  u32 iface_addr_count;
  u32 proto_count;
  u32 bgp_count;
  u32 ospf_count;
  u32 ospf_lsa_count;
  u32 ospf_neigh_count;
  u32 bfd_count;
  u32 babel_count;
  u32 babel_iface_count;
  u32 babel_neigh_count;

  struct bird_shm_iface ifaces[BIRD_SHM_MAX_INTERFACES];
  struct bird_shm_iface_addr iface_addrs[BIRD_SHM_MAX_IFACE_ADDRS];
  struct bird_shm_proto protos[BIRD_SHM_MAX_PROTOCOLS];
  struct bird_shm_bgp_info bgp[BIRD_SHM_MAX_BGP];
  struct bird_shm_ospf_info ospf[BIRD_SHM_MAX_OSPF];
  struct bird_shm_ospf_lsa ospf_lsas[BIRD_SHM_MAX_OSPF_LSAS];
  struct bird_shm_ospf_neighbor ospf_neighs[BIRD_SHM_MAX_OSPF_NEIGHBORS];
  struct bird_shm_bfd_session bfd[BIRD_SHM_MAX_BFD_SESSIONS];
  struct bird_shm_babel_info babel[BIRD_SHM_MAX_BABEL];
  struct bird_shm_babel_iface babel_ifaces[BIRD_SHM_MAX_BABEL_IFACES];
  struct bird_shm_babel_neighbor babel_neighs[BIRD_SHM_MAX_BABEL_NEIGHBORS];
};

struct bird_shm_region {
  u32 magic;
  u32 version;
  volatile u32 cmd_flag;
  u32 reserved;
  volatile u64 version_seq;
  struct bird_shm_mailbox mailbox;
  struct bird_shm_snapshot snapshot;
};

void bird_shm_init(void);
void bird_shm_poll(void);

#endif /* _BIRD_SHM_EXPORT_H_ */
