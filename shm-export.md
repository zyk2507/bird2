# Shared Memory Export (BIRD -> External Agent)

This document describes the shared-memory (SHM) export mechanism implemented in
`sysdep/unix/shm_export.{c,h}` and the companion gRPC agent in
`bird-grpc-agent/` (kept as a separate process outside the BIRD daemon).

## Overview

BIRD acts as the producer and writes a snapshot of internal state into a shared
memory segment. An external consumer (e.g., the Rust gRPC agent) issues a
request via a mailbox flag and reads the resulting snapshot using a seqlock
pattern.

Key properties:
- No text parsing: data is read directly from internal C structures.
- Snapshot-based: the data buffer is rewritten on each request.
- Lock-free for readers: seqlock versioning protects consistency.
- Export scope: status, interfaces, protocols, BGP, OSPF, BFD, and Babel (no routes).

## SHM Segment

- Name: `"/bird_shm_export"` (see `BIRD_SHM_NAME`)
- Permissions: `0600` (created with `shm_open`)
- Size: `sizeof(struct bird_shm_region)`
- Endianness: integer fields are native-endian; IP bytes are stored in
  network order (big-endian) using `put_ip4`/`put_ip6`.
- Time units: `boot_time` and `current_time` are `btime` (microseconds, stored as `u64`).

### Lifecycle

BIRD creates and maps the SHM segment at startup (`bird_shm_init()` in
`sysdep/unix/main.c`) and checks for requests once per I/O loop cycle
(`bird_shm_poll()` in `sysdep/unix/io.c`). The segment is not unlinked on exit.
Export is enabled only when `ENABLE_SHM_EXPORT` is set to a non-zero integer.

## Writer/Reader Protocol

### Writer (BIRD)

1. If `cmd_flag == 0`, do nothing.
2. If `cmd_flag == 1`, increment `version_seq` (odd => writing).
3. Serialize data into `snapshot`.
4. Increment `version_seq` (even => consistent).
5. Reset `cmd_flag = 0`.

### Reader (External)

1. Fill `mailbox` (command + args).
2. Set `cmd_flag = 1`.
3. Spin until `cmd_flag == 0` and `version_seq` changes to a new even value.
4. Read snapshot with seqlock validation:
   - Read `version_seq` (must be even).
   - Copy `snapshot`.
   - Read `version_seq` again; if changed or odd, retry.

## Memory Layout

All layout definitions live in `sysdep/unix/shm_export.h`. The Rust agent uses
`#[repr(C)]` equivalents in `src/shm.rs`.

### Top-Level Region

```c
struct bird_shm_region {
  u32 magic;              // BIRD_SHM_MAGIC (0x42524453)
  u32 version;            // BIRD_SHM_VERSION
  volatile u32 cmd_flag;  // 0 = idle, 1 = request pending
  u32 reserved;
  volatile u64 version_seq; // seqlock counter
  struct bird_shm_mailbox mailbox;
  struct bird_shm_snapshot snapshot;
};
```

### Mailbox

```c
struct bird_shm_mailbox {
  u32 cmd;     // currently only 1 = CMD_SNAPSHOT
  u32 reserved;
  u64 arg0;    // reserved for future use
  u64 arg1;    // reserved for future use
};
```

### Snapshot Header

```c
struct bird_shm_snapshot {
  u32 version;        // BIRD_SHM_SNAPSHOT_VERSION
  u32 trunc_flags;    // truncation bitmap (see below)
  u64 last_cmd;       // last command processed
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

  // fixed-capacity arrays follow...
};
```

### Truncation Flags

When a list exceeds the fixed capacity, it is truncated and the corresponding
flag is set:

- `BIRD_SHM_TRUNC_IFACES`
- `BIRD_SHM_TRUNC_IFACE_ADDRS`
- `BIRD_SHM_TRUNC_PROTOCOLS`
- `BIRD_SHM_TRUNC_BGP`
- `BIRD_SHM_TRUNC_OSPF`
- `BIRD_SHM_TRUNC_OSPF_LSAS`
- `BIRD_SHM_TRUNC_OSPF_NEIGHS`
- `BIRD_SHM_TRUNC_BFD`
- `BIRD_SHM_TRUNC_BABEL`
- `BIRD_SHM_TRUNC_BABEL_IFACES`
- `BIRD_SHM_TRUNC_BABEL_NEIGHS`

### Fixed-Capacity Arrays

The snapshot stores fixed arrays with a count field for each. The counts are
bounded by the constants in `shm_export.h`:

- Interfaces: `BIRD_SHM_MAX_INTERFACES`
- Interface addresses: `BIRD_SHM_MAX_IFACE_ADDRS`
- Protocols: `BIRD_SHM_MAX_PROTOCOLS`
- BGP sessions: `BIRD_SHM_MAX_BGP`
- OSPF instances: `BIRD_SHM_MAX_OSPF`
- OSPF LSAs: `BIRD_SHM_MAX_OSPF_LSAS`
- OSPF neighbors: `BIRD_SHM_MAX_OSPF_NEIGHBORS`
- BFD sessions: `BIRD_SHM_MAX_BFD_SESSIONS`
- Babel instances: `BIRD_SHM_MAX_BABEL`
- Babel interfaces: `BIRD_SHM_MAX_BABEL_IFACES`
- Babel neighbors: `BIRD_SHM_MAX_BABEL_NEIGHBORS`

### IP Address Encoding

```c
struct bird_shm_ip_addr {
  u8 af;        // 0 = none, 4 = IPv4, 6 = IPv6
  u8 pad[3];
  u8 bytes[16]; // IPv4 uses bytes[0..3], IPv6 uses bytes[0..15]
};
```

IPv4 and IPv6 bytes are stored in network byte order.

### Interfaces

```c
struct bird_shm_iface {
  char name[16];
  u32 flags;
  u32 mtu;
  u32 index;
  u32 addr_start; // index into iface_addrs
  u32 addr_count; // number of addresses for this iface
};

struct bird_shm_iface_addr {
  u32 iface_index;   // index in ifaces[]
  u16 prefix_len;
  u16 scope;
  u32 flags;
  struct bird_shm_ip_addr ip;
  struct bird_shm_ip_addr brd;
  struct bird_shm_ip_addr opposite;
};
```

### Protocols

```c
struct bird_shm_proto {
  char name[32];
  u32 class;  // enum bird_shm_proto_class (see below)
  u32 state;  // proto_state (PS_*)
};
```

The `class` field is derived from the protocol type (not config class) and
uses `enum bird_shm_proto_class` from `shm_export.h`:

- `BIRD_SHM_PROTO_DEVICE`, `BIRD_SHM_PROTO_RADV`, `BIRD_SHM_PROTO_RIP`,
  `BIRD_SHM_PROTO_STATIC`, `BIRD_SHM_PROTO_MRT`, `BIRD_SHM_PROTO_OSPF`,
  `BIRD_SHM_PROTO_L3VPN`, `BIRD_SHM_PROTO_AGGREGATOR`, `BIRD_SHM_PROTO_PIPE`,
  `BIRD_SHM_PROTO_BGP`, `BIRD_SHM_PROTO_BMP`, `BIRD_SHM_PROTO_BFD`,
  `BIRD_SHM_PROTO_BABEL`, `BIRD_SHM_PROTO_RPKI`, or `BIRD_SHM_PROTO_UNKNOWN`.

### BGP

```c
struct bird_shm_bgp_info {
  char name[32];
  u32 local_as;
  u32 remote_as;
  u8 conn_state;              // bgp_conn.state
  u8 pad[3];
  struct bird_shm_ip_addr remote_ip;
};
```

### OSPF

```c
struct bird_shm_ospf_info {
  char name[32];
  u32 router_id;
  u8 version;      // 2 or 3
  u8 pad[3];
  u32 lsa_start;   // index into ospf_lsas
  u32 lsa_count;
  u32 neigh_start; // index into ospf_neighs
  u32 neigh_count;
};

struct bird_shm_ospf_lsa {
  u32 proto_index; // index in ospf[]
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
  u32 proto_index; // index in ospf[]
  char ifname[16];
  u32 rid;
  u8 state;
  u8 pad[3];
  struct bird_shm_ip_addr ip;
};
```

Only LSAs with a valid `lsa_body` are included.

### BFD

```c
struct bird_shm_bfd_session {
  struct bird_shm_ip_addr addr;
  char ifname[16];
  u8 state;
  u8 rem_state;
  u8 pad[2];
  u32 local_disc;
  u32 remote_disc;
};
```

### Babel

```c
struct bird_shm_babel_info {
  char name[32];
  u64 router_id;
  u32 update_seqno;
  u8 triggered;
  u8 pad[3];
  u32 iface_start; // index into babel_ifaces
  u32 iface_count;
  u32 neigh_count;
};

struct bird_shm_babel_iface {
  u32 proto_index; // index in babel[]
  char ifname[16];
  u8 up;
  u8 pad[3];
  u32 tx_length;
  u16 hello_seqno;
  u16 pad2;
  u32 neigh_start; // index into babel_neighs
  u32 neigh_count;
  struct bird_shm_ip_addr addr;
  struct bird_shm_ip_addr next_hop_ip4;
  struct bird_shm_ip_addr next_hop_ip6;
};

struct bird_shm_babel_neighbor {
  u32 iface_index; // index in babel_ifaces[]
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
```

## Using the gRPC Agent

The Rust agent is built in `bird-grpc-agent/` and runs as a separate process.
It uses `tonic` + `prost` with a generated `exporter.proto`.

Build (from the `bird-grpc-agent` repo root):

```
cargo check
cargo build
```

Run (default address `127.0.0.1:50051`):

```
BIRD_GRPC_ADDR=127.0.0.1:50051 ./target/debug/bird-grpc-agent
```

RPCs (from `proto/exporter.proto`):
- `GetStatus`
- `ListInterfaces`
- `ListProtocols`
- `ListBgp`
- `ListOspf`
- `ListBfd`
- `ListBabel`
- `GetSnapshot`

Each RPC triggers a SHM snapshot read.

## Implementation Notes

- BIRD increments `version_seq` before and after writing the snapshot; odd values
  indicate an in-progress write. Readers must retry if the sequence changes or
  is odd.
- `cmd_flag` and `version_seq` are updated using atomic operations to avoid torn
  reads/writes on weak memory models.
- The Rust agent validates `magic`, `version`, and `snapshot.version` before
  decoding and returns an error on mismatch.
- The mailbox currently supports only `CMD_SNAPSHOT`; unknown commands are
  ignored and reset to idle.
- The SHM segment is created on startup and left in place on exit.

## Compatibility Notes

- The SHM layout is versioned (`magic`, `version`, `snapshot.version`).
- Readers must validate the header before interpreting the data.
- This layout is host-ABI dependent and intended for same-host consumption.
- Changing struct definitions requires bumping the version constants.
