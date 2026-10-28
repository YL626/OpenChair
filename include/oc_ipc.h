#ifndef OC_IPC_H
#define OC_IPC_H

/* oc_ipc: AF_UNIX SOCK_SEQPACKET transport for the OpenChair processes.
 *
 * STATUS: [REVIEW] follows docs/IMPLEMENTATION_NOTES.md section 3. Linux only.
 * This layer moves whole, bounded, validated datagrams. It does not decide
 * whether a sender is acceptable (oc_ipc_hello_check does, statelessly), it
 * keeps no per-source state, and it contains no supervisor or hub logic: the
 * hub process (S3B) and supervisor (S4) are built on top of it.
 *
 * Errors: 0 or a negative errno. EINTR is retried internally and never
 * surfaces. Unless stated, a failed call leaves its output argument unchanged.
 *
 * ---- Initialization and descriptor ownership ----
 * struct oc_ipc_conn and struct oc_ipc_server own their descriptor. Every
 * object must be initialized with OC_IPC_CONN_INIT / OC_IPC_SERVER_INIT (or
 * the *_init functions) before first use. Initialized means open == false and
 * fd == -1. Close functions act only when open == true, so closing an
 * initialized, zeroed ({0}) or already-closed object is a no-op and never
 * closes descriptor 0 by accident. (A zeroed object has fd == 0, but
 * open == false, which is what is trusted.) Reading an uninitialized object
 * is undefined behaviour, as for any C object. Functions that fill an object
 * (accept, connect, server_open) return -EISCONN / -EBUSY if it is already
 * open instead of overwriting, and therefore leaking, its descriptor. On any
 * failure every descriptor acquired inside the call is closed and the object
 * stays closed. All descriptors are CLOEXEC and nonblocking. A live object is
 * not copied; move it with a struct assignment and then re-initialize the
 * source.
 *
 * ---- Sending ----
 * Every send is nonblocking and uses MSG_NOSIGNAL (no SIGPIPE); the library
 * never changes signal dispositions. There is NO userspace queue. The only
 * buffering is the kernel's per-socket send buffer, which the library sets
 * with SO_SNDBUF (OC_IPC_SNDBUF_REQUEST; the effective, kernel-doubled value
 * is read back into conn.sndbuf). Unread datagrams are charged to the
 * SENDER's send buffer, and the kernel refuses a send once that is full, so
 * the queue is bounded above by sndbuf / (smallest datagram sent) + 1 (for
 * SAMPLEs, sndbuf / 28 + 1); per-datagram kernel overhead makes the real count
 * much lower (43 on the development kernel). This library does not rely on
 * net.unix.max_dgram_qlen for the bound; the sysctl may only lower the
 * capacity further, never raise it. A full buffer returns -EAGAIN at once.
 * SEQPACKET is all-or-nothing: a send that reports a length other than the
 * one requested is an error (-EIO), not a short success.
 * Control messages (HELLO, BYE) are never dropped silently: -EAGAIN is
 * returned to the caller, who owns the retry. SAMPLE sends are dropped by
 * design; see oc_ipc_send_sample.
 *
 * ---- Receiving ----
 * Boundaries are preserved: one oc_ipc_recv consumes exactly one datagram.
 * The datagram is read with recvmsg(MSG_TRUNC) into a fixed 64-byte buffer so
 * its true length is known; anything longer than OC_IPC_MAX_MSG is discarded
 * whole. Because each datagram is consumed in full, a malformed, truncated or
 * oversized message cannot desynchronize the stream: -EMSGSIZE and -EBADMSG
 * mean "that one datagram was consumed and counted; the connection is still
 * usable". Whether to close after repeated errors is hub policy (S3B).
 * -EPROTONOSUPPORT is different and CONNECTION-FATAL: the peer speaks an
 * unknown version, so nothing it sends can be trusted or answered. oc_ipc_recv
 * closes the connection itself (c->open == false afterwards) and sends no
 * reply. The caller must drop the connection from its own bookkeeping.
 * EOF is reported as -EPIPE. A zero-length datagram also reads as 0 bytes on
 * SEQPACKET; it is told apart from EOF by poll (POLLHUP) and, if the peer is
 * still connected, consumed and counted as malformed (-EBADMSG). This library
 * never sends one.
 *
 * ---- Process rules ----
 * A server's socket path should be absolute: close unlinks by the stored path,
 * so a chdir() in between would leave the file behind. Only the process that
 * opened a server should close it; a fork() child that inherits it must not
 * call oc_ipc_server_close (it would unlink the parent's live socket).
 *
 * Timing uses CLOCK_MONOTONIC. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "oc_ipc_msg.h"

#define OC_IPC_SNDBUF_REQUEST 16384u   /* [MEASURE] requested SO_SNDBUF; kernel doubles it */
#define OC_IPC_MIN_DGRAM OC_MSG_HEADER_SIZE   /* smallest legal datagram, for the bound above */
#define OC_IPC_BACKLOG 4
#define OC_IPC_RECV_CAP 64u            /* receive buffer; bigger datagrams are oversized */

/* Counters, owned by the connection; never reset by the library.
 *  rx_msgs       datagrams that decoded successfully
 *  rx_malformed  consumed datagrams rejected as truncated/trailing/-EBADMSG
 *  rx_oversized  consumed datagrams longer than OC_IPC_MAX_MSG
 *  tx_msgs       datagrams of any type accepted by the kernel
 *  tx_dropped    valid SAMPLEs NOT handed to the kernel (buffer full, peer
 *                gone, connection not open): exactly once per discarded sample */
struct oc_ipc_stats {
    uint64_t rx_msgs;
    uint64_t rx_malformed;
    uint64_t rx_oversized;
    uint64_t tx_msgs;
    uint64_t tx_dropped;
};

struct oc_ipc_conn {
    int fd;                      /* valid only while open */
    bool open;
    bool connecting;             /* client connect still in progress */
    struct oc_ipc_peer peer;     /* kernel-reported peer; valid once connected */
    uint32_t sndbuf;             /* effective SO_SNDBUF */
    struct oc_ipc_stats st;
};
#define OC_IPC_CONN_INIT {.fd = -1}      /* initializer only; use oc_ipc_conn_init() to assign */

struct oc_ipc_server {
    int fd;
    bool open;
    char path[108];              /* sizeof(sun_path); NUL-terminated */
    dev_t dev;                   /* identity of the socket file WE created */
    ino_t ino;
};
#define OC_IPC_SERVER_INIT {.fd = -1}    /* initializer only; use oc_ipc_server_init() to assign */

void oc_ipc_conn_init(struct oc_ipc_conn *c);
void oc_ipc_server_init(struct oc_ipc_server *s);

/* Closes the descriptor once and re-initializes. No-op unless open. Counters
 * are kept so a caller can read them after close. A close() that fails with
 * EINTR is not retried (the descriptor is gone on Linux). */
void oc_ipc_conn_close(struct oc_ipc_conn *c);

/* ---- server ---- */

/* Creates, binds and listens on `path`. The caller creates the parent
 * directory; it must be a directory owned by the effective uid with no group
 * or other write bits (-EACCES otherwise), which is what makes unlinking a
 * stale path safe. Path rules:
 *   absent                         -> create
 *   socket, owned by us, no server -> stale: unlinked (identity re-checked)
 *   socket with a live server      -> -EADDRINUSE, the live socket is untouched
 *   socket owned by someone else   -> -EACCES
 *   anything else (file, symlink,
 *     directory)                   -> -ENOTSOCK, never unlinked
 * -ENAMETOOLONG if the path does not fit sockaddr_un; -EINVAL if empty or it
 * ends in '/'. The probe of a live server is itself a connection that the
 * live server will see accepted and then closed with no data. */
int oc_ipc_server_open(struct oc_ipc_server *s, const char *path);

/* Accepts one pending connection. -EAGAIN if none; -ECONNABORTED if a client
 * vanished before accept (just call again). Fills conn->peer from SO_PEERCRED.
 * The server never replies by itself: the caller (hub) validates HELLO and
 * sends ACK or REJECT. */
int oc_ipc_server_accept(struct oc_ipc_server *s, struct oc_ipc_conn *out);

/* Closes the listening descriptor and unlinks the path only if it still is the
 * very socket file this server created (same device and inode, still a
 * socket). A replaced or foreign file is left alone. No-op unless open. */
void oc_ipc_server_close(struct oc_ipc_server *s);

/* ---- client ---- */

/* One nonblocking connect attempt. Results:
 *   0              connected; peer credentials are checked (-EPERM, with the
 *                  connection closed, if the server is not our uid)
 *   -EINPROGRESS   the kernel reports the connect as in flight (also EINTR);
 *                  c is open with connecting == true: finish it with
 *                  oc_ipc_client_connect_finish or oc_ipc_client_handshake
 *   -EAGAIN        the server's backlog is full; nothing was acquired, retry
 *   -ENOENT / -ECONNREFUSED / ...  no server; nothing acquired
 * AF_UNIX normally completes (or fails) immediately; -EINPROGRESS is handled
 * because the contract allows it. */
int oc_ipc_client_connect(struct oc_ipc_conn *c, const char *path);

/* Nonblocking check of an in-progress connect using poll and
 * getsockopt(SO_ERROR), then verifies the peer exists and its uid.
 * 0 = connected (also if it already was), -EINPROGRESS = not yet,
 * other = failed and the connection has been closed. */
int oc_ipc_client_connect_finish(struct oc_ipc_conn *c);

/* Sends HELLO and waits for the reply under ONE CLOCK_MONOTONIC deadline of
 * timeout_ms covering: completing a pending connect, sending HELLO, and
 * waiting for the reply. Signals neither restart nor extend that deadline.
 *   0              a valid HELLO_ACK whose session_id, instance and source
 *                  echo the HELLO. Nothing else counts as success.
 *   -EACCES        the hub sent a valid HELLO_REJECT; *reject (may be NULL)
 *                  holds its reason and detail
 *   -ETIMEDOUT     deadline reached
 *   -EPROTO        valid message but not an ACK/REJECT, or an ACK whose echo
 *                  differs from the HELLO
 *   -ECONNABORTED  the hub answered with BYE
 *   -EPIPE / -ECONNRESET  hub closed; -EPROTONOSUPPORT / -EBADMSG / -EMSGSIZE
 *                  as for oc_ipc_recv
 * Any failure after I/O began CLOSES the connection (a connection whose
 * HELLO was answered wrongly or not at all is not reusable), including a
 * single malformed reply (-EBADMSG/-EMSGSIZE): during the handshake the
 * "malformed datagram leaves the connection usable" rule does not apply.
 * -EACCES means ONLY a HELLO_REJECT (a server of another uid is -EPERM). -EINVAL (bad
 * hello/arguments) and -ENOTCONN (c not open) do no I/O and leave c alone.
 * `reject` is written only on -EACCES. timeout_ms must be > 0. */
int oc_ipc_client_handshake(struct oc_ipc_conn *c, const struct oc_hello *hello,
                            int timeout_ms, struct oc_hello_reject *reject);

/* ---- both ends ---- */

/* Sends one complete datagram (len in [OC_MSG_HEADER_SIZE, OC_IPC_MAX_MSG]),
 * MSG_DONTWAIT|MSG_NOSIGNAL. 0, or -EAGAIN (buffer full: NOT sent), -EPIPE /
 * -ECONNRESET (peer gone), -ENOTCONN (not open or still connecting), -EIO if
 * the kernel reports a length other than len. Not counted as a drop. */
int oc_ipc_send_msg(struct oc_ipc_conn *c, const uint8_t *buf, size_t len);

/* Encodes and sends one SAMPLE. 0 = handed to the kernel. -EINVAL = invalid
 * sample (nothing sent, not counted). For ANY other failure the sample is
 * discarded, st.tx_dropped is incremented exactly once, and the errno is
 * returned (-EAGAIN when full). The caller keeps its schedule and does not
 * retry or queue. */
int oc_ipc_send_sample(struct oc_ipc_conn *c, const struct oc_sample *s);

/* Receives and decodes one datagram; see the file comment. Success fills *out;
 * all errors leave it unchanged. -EAGAIN = nothing pending. */
int oc_ipc_recv(struct oc_ipc_conn *c, struct oc_ipc_msg *out);

/* Waits until a datagram, EOF or an error is pending, under a single
 * CLOCK_MONOTONIC deadline (signals do not restart it). 0 = readable,
 * -ETIMEDOUT, -ENOTCONN. timeout_ms == 0 polls once. */
int oc_ipc_wait_readable(const struct oc_ipc_conn *c, int timeout_ms);

#endif
