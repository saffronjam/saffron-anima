//! The synchronous, single-threaded, non-blocking `AF_UNIX` socket server.
//!
//! A listening socket polled once per frame from the host's main loop. There is
//! no async runtime, no worker pool, no background thread — `drain` accepts
//! pending clients, reads readable bytes, splits on `\n`, runs each request on
//! the calling thread, and writes one compact JSON line back, never blocking.

use std::os::fd::{AsFd, OwnedFd};

use rustix::event::{PollFlags, poll};
use rustix::fs::{Mode, chmod, unlink};
use rustix::io::Errno;
use rustix::net::{
    AddressFamily, RecvFlags, SendFlags, SocketAddrUnix, SocketFlags, SocketType, accept_with,
    bind_unix, listen, recv, send, socket_with,
};

use crate::error::{Error, Result};

/// The `recv` scratch buffer size.
const RECV_CHUNK: usize = 4096;

/// One connected client: its socket, the bytes accumulated awaiting a `\n`, and
/// whether the peer has gone (closing is by dropping the `OwnedFd`, so death is
/// flagged explicitly).
struct Client {
    fd: OwnedFd,
    inbuf: Vec<u8>,
    dead: bool,
}

/// The listening server: the accept socket, the bound path (unlinked on stop),
/// and the live clients.
pub struct ControlServer {
    listen_fd: OwnedFd,
    path: String,
    clients: Vec<Client>,
}

impl ControlServer {
    /// The bound socket path.
    #[must_use]
    pub fn path(&self) -> &str {
        &self.path
    }
}

impl Drop for ControlServer {
    /// Closes the listening socket (clients drop with the vec) and unlinks the
    /// bound path. The `OwnedFd`s close themselves; only the path needs explicit
    /// removal.
    fn drop(&mut self) {
        if !self.path.is_empty() {
            let _ = unlink(self.path.as_str());
        }
    }
}

/// Resolves the control socket path: `SAFFRON_CONTROL_SOCK` if set, else
/// `$XDG_RUNTIME_DIR/saffron-control.sock`, else `/tmp/saffron-control-<uid>.sock`.
#[must_use]
pub fn control_socket_path() -> String {
    resolve_socket_path(
        std::env::var("SAFFRON_CONTROL_SOCK").ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
        rustix::process::getuid().as_raw(),
    )
}

/// The pure resolution rule behind [`control_socket_path`], split out so the
/// precedence is testable without mutating process-global env.
fn resolve_socket_path(override_path: Option<&str>, runtime_dir: Option<&str>, uid: u32) -> String {
    if let Some(path) = override_path {
        return path.to_owned();
    }
    if let Some(runtime) = runtime_dir {
        return format!("{runtime}/saffron-control.sock");
    }
    format!("/tmp/saffron-control-{uid}.sock")
}

/// Binds and listens on `path`: a non-blocking, close-on-exec `AF_UNIX` stream
/// socket, the stale path unlinked first, mode `0600`, backlog 8.
///
/// # Errors
///
/// [`Error::Socket`] if any of socket/bind/listen fails; [`Error::PathTooLong`]
/// if the path does not fit the socket address.
pub fn start_control_server(path: String) -> Result<ControlServer> {
    let listen_fd = socket_with(
        AddressFamily::UNIX,
        SocketType::STREAM,
        SocketFlags::NONBLOCK | SocketFlags::CLOEXEC,
        None,
    )
    .map_err(|e| Error::Socket(format!("socket: {e}")))?;

    // Remove any stale socket before binding.
    let _ = unlink(path.as_str());

    let addr = SocketAddrUnix::new(path.as_str()).map_err(|_| Error::PathTooLong(path.clone()))?;
    bind_unix(&listen_fd, &addr).map_err(|e| Error::Socket(format!("bind '{path}': {e}")))?;

    // Owner-only permission is the access control (with the 0700 runtime dir).
    let _ = chmod(path.as_str(), Mode::from(0o600));

    listen(&listen_fd, 8).map_err(|e| Error::Socket(format!("listen: {e}")))?;

    Ok(ControlServer {
        listen_fd,
        path,
        clients: Vec::new(),
    })
}

impl ControlServer {
    /// Accepts pending clients, drains their readable bytes, splits the buffered
    /// input on `\n`, and for each complete line calls `handle(line) -> reply`,
    /// flushing the reply (plus a trailing `\n`) back with the short-write loop.
    /// Closed clients are dropped at the end.
    ///
    /// `handle` is the request→reply seam: the per-frame caller wires it to parse
    /// the line and dispatch through the registry against the live
    /// `EngineContext`. Threading the dispatch through a closure keeps the socket
    /// machinery decoupled from the un-headless-constructible subsystems, so the
    /// framing and flush are unit-testable on their own.
    pub fn drain(&mut self, mut handle: impl FnMut(&str) -> String) {
        self.accept_pending();

        for client in &mut self.clients {
            read_into(client);

            while !client.dead {
                let Some(newline) = client.inbuf.iter().position(|&b| b == b'\n') else {
                    break;
                };
                let line: Vec<u8> = client.inbuf.drain(..=newline).collect();
                // The slice up to (not incl.) the trailing '\n' is the request.
                let request = String::from_utf8_lossy(&line[..line.len() - 1]);

                let mut reply = handle(&request);
                reply.push('\n');
                flush_reply(client, reply.as_bytes());
            }
        }

        self.clients.retain(|client| !client.dead);
    }

    /// Accepts every pending connection until `accept` would block.
    fn accept_pending(&mut self) {
        while let Ok(fd) = accept_with(
            self.listen_fd.as_fd(),
            SocketFlags::NONBLOCK | SocketFlags::CLOEXEC,
        ) {
            self.clients.push(Client {
                fd,
                inbuf: Vec::new(),
                dead: false,
            });
        }
    }
}

/// Reads all currently-available bytes from the client into its buffer.
///
/// A `recv` of 0 is EOF (the peer closed) and marks the client dead;
/// `EAGAIN`/`EWOULDBLOCK` means the socket is drained for this frame.
fn read_into(client: &mut Client) {
    let mut chunk = [0u8; RECV_CHUNK];
    loop {
        match recv(client.fd.as_fd(), &mut chunk, RecvFlags::DONTWAIT) {
            Ok(0) => {
                client.dead = true;
                return;
            }
            Ok(received) => client.inbuf.extend_from_slice(&chunk[..received]),
            // `EAGAIN` and `EWOULDBLOCK` are the same errno on Linux: socket drained.
            Err(Errno::AGAIN) => return,
            Err(Errno::INTR) => {}
            Err(_) => {
                client.dead = true;
                return;
            }
        }
    }
}

/// Sends `out` in full, looping over short writes.
///
/// The client socket is non-blocking, so a single `send` short-writes any reply
/// larger than the socket buffer and silently drops the tail — the client then
/// never sees the `\n` and hangs. This loops until the whole reply is flushed,
/// `poll`-waiting (1000 ms) on `POLLOUT` when the buffer fills, ignoring `EINTR`,
/// and marking the client dead on any other fatal error (`MSG_NOSIGNAL` keeps a
/// vanished peer from raising `SIGPIPE`).
fn flush_reply(client: &mut Client, out: &[u8]) {
    let mut sent = 0;
    while sent < out.len() && !client.dead {
        match send(client.fd.as_fd(), &out[sent..], SendFlags::NOSIGNAL) {
            Ok(n) => sent += n,
            // `EAGAIN`/`EWOULDBLOCK` (one errno on Linux): the send buffer is full;
            // wait for writability, then retry.
            Err(Errno::AGAIN) => {
                let mut fds = [rustix::event::PollFd::new(&client.fd, PollFlags::OUT)];
                let _ = poll(&mut fds, 1000);
            }
            Err(Errno::INTR) => {}
            Err(_) => client.dead = true,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::resolve_socket_path;

    #[test]
    fn path_resolution_honors_override_then_runtime_then_tmp() {
        // The override wins outright.
        assert_eq!(
            resolve_socket_path(Some("/run/custom.sock"), Some("/run/user/1000"), 1000),
            "/run/custom.sock"
        );
        // Else the runtime dir.
        assert_eq!(
            resolve_socket_path(None, Some("/run/user/1000"), 1000),
            "/run/user/1000/saffron-control.sock"
        );
        // Else the per-uid tmp fallback.
        assert_eq!(
            resolve_socket_path(None, None, 1000),
            "/tmp/saffron-control-1000.sock"
        );
    }
}
