# AGENTS.md

## Project Overview
**Goal:** Create a high-performance, structured data export mechanism for the BIRD Internet Routing Daemon by reading internal C structures and serving them via a separate gRPC agent.

**Architecture:**
1.  **BIRD (C):** Acts as the **Producer**. It reads its own internal memory structures (without regex parsing) and serializes them into a Shared Memory (SHM) segment.
2.  **IPC (Shared Memory):**
    * **Command Channel (Rust → BIRD):** Single-slot mailbox.
    * **Data Channel (BIRD → Rust):** Versioned Snapshot (Seqlock pattern).
3.  **Exporter (Rust):** Acts as the **Consumer**. A standalone gRPC server running in a separate process that attaches to the SHM, reads binary data, and serves it via Google Protobufs.

---

## Part 1: Shared Memory Protocol (IPC)
**Constraint:** Use the simplest correct shared-memory protocol (Command Mailbox + Seqlock Snapshot).

**Protocol Logic:**
* **BIRD (Writer):**
    1.  Check `cmd_flag` in SHM. If 0, do nothing.
    2.  If 1: Increment `version_seq` (make odd = writing).
    3.  Read `mailbox` arguments, serialize internal structs to the `data_buffer`.
    4.  Increment `version_seq` (make even = consistent).
    5.  Reset `cmd_flag = 0` (idle).
* **Rust (Reader):**
    1.  Write request arguments to `mailbox`.
    2.  Set `cmd_flag = 1`.
    3.  Loop: Read `version_seq`.
    4.  If odd, spin/wait.
    5.  Copy `data_buffer` to local memory.
    6.  Re-read `version_seq`. If it differs from step 3, retry loop.

---

## Part 2: BIRD Modification (C Language)
**Location:** Main BIRD source repository.
**Constraints:**
* **NO** Regex parsing of text output. Access raw pointers only.
* **NO** changes to configuration logic (`conf.y`, `cf-lex.l`).

**Tasks:**
1.  **SHM Setup:**
    * In `sysdep/unix/main.c`: Initialize `shm_open` and `mmap` on startup.
2.  **Event Loop Hook:**
    * Add a non-blocking check in the main IO loop to monitor `cmd_flag`.
3.  **Serialization (New File `sysdep/unix/shm_export.c`):**
    * **Status:** Read globals (`bird_launch_time`, `current_time`).
    * **Interfaces:** Iterate `iface_list` (Name, Type, MTU, State, Addrs).
    * **Protocols:** Iterate `proto_list`. Check `p->proto_state`.
    * **BGP:** Cast to `struct bgp_proto`. Read `conn` state, neighbor AS.
    * **OSPF:** Cast to `struct ospf_proto`. Read `lsadb` (LSA headers) and neighbor list.
    * **BFD:** Iterate BFD sessions list.
   ** DO NOT ** export any route detail information since it will saturate the memory.
4. **Enabling the feature**
   An environment varibale is needed (ENABLE_SHM_EXPORT) to enable this feature. If the environmen variable value is not set or 0. It should not export anything.

---

## Part 3: gRPC Server (Rust Language)
**Location:** A **NEW** directory (e.g., `bird-grpc-agent/`) *outside* the main source tree.
**Constraints:**
* Use `tonic` (gRPC) and `prost` (Google Protobuf).
* Run as a completely separate process.

**Tasks:**
1.  **Dependencies:** `tonic`, `prost`, `nix` (or `libc` for SHM).
2.  **Protobuf Definition (`exporter.proto`):**
    * Define messages mirroring the internal C structs: `RouterStatus`, `Interface`, `ProtocolInfo`, `OspfLsa`, `BgpInfo`.
    * Define Service: `service BirdExporter { rpc GetStatus(...) returns (...); ... }`
3.  **SHM Attachment:**
    * Open the shared memory file created by BIRD.
    * Map it into the Rust process address space.
4.  **Server Logic:**
    * Receive gRPC request.
    * Translate to IPC Command ID.
    * Execute Reader Protocol (Write Mailbox → Wait for Seqlock → Read Data).
    * Deserialize binary data → Populate Protobuf → Send Response.

---

## Part 4: Validation & Self-Correction
**Mandatory Requirement:** The Agent must compile and verify the code after generation.

**Steps:**
1.  **Compile BIRD:**
    * Run `autoreconf`, `./configure`, and `make`.
    * **Action:** If compilation fails (linker errors, syntax errors, missing struct members), analyze the error log, apply fixes to the C source, and retry until clean.
2.  **Compile Rust Agent:**
    * Run `cargo check` and `cargo build`.
    * **Action:** If compilation fails (borrow checker, type mismatch, dependency issues), analyze the compiler output, apply fixes, and retry until the build succeeds.
