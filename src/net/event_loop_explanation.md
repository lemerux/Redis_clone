# Event loop explanation

This document explains [`event_loop.cpp`](./event_loop.cpp) and the related
types it uses. The implementation is Linux-specific because it uses `epoll`.

## What the event loop does

The event loop lets one thread serve many TCP clients. It waits for readiness
notifications from `epoll`, accepts new connections, reads available bytes,
parses RESP commands, dispatches handlers, and writes queued replies. Client
sockets are non-blocking, so a slow client does not make the server wait for
that client before serving others.

The main flow for one client request is:

1. `epoll_wait` reports that the listening socket or a client socket is ready.
2. The loop accepts clients or reads bytes from a ready client.
3. `RespParser` consumes each complete RESP value from the input buffer.
4. The command table finds and invokes the matching handler.
5. The handler appends a reply to the output buffer.
6. The loop writes pending reply bytes and watches for `EPOLLOUT` only while
   bytes remain to be sent.
7. Connections scheduled for closure are destroyed after the current event
   batch has been processed.

## Structs and types used

### `Connection`

Declared in [`event_loop.h`](./event_loop.h). One instance represents one
accepted client connection.

| Member | Meaning |
|---|---|
| `socket` | Owns the client socket. Its RAII destructor closes the fd. |
| `input` | Accumulates bytes read from the client until complete RESP values can be parsed. |
| `output` | Holds reply bytes that have not yet been fully sent. |
| `clientId` | Monotonically assigned identifier used in command log entries. |
| `closeAfterReply` | Stops further command reads and closes after queued output is sent. Also set when the peer reaches EOF. |
| `closing` | Marks a connection scheduled for deferred removal, preventing duplicate close requests or handling more events for it. |

The event loop stores owning `std::unique_ptr<Connection>` objects in a vector
indexed by socket fd. A null slot means no live connection is registered at
that fd.

### `CommandContext`

Declared in [`command.h`](../command/command.h). This is the small set of
connection state exposed to a command handler:

- `output` is a reference to the connection's reply buffer.
- `closeAfterReply` is a reference to the flag a handler can set to request
  connection closure after its reply is delivered (used by `QUIT`).

It lets free-function handlers act on the relevant connection state without
needing to know about the whole `EventLoop` or `Connection`.

### `CommandSpec`

Declared in [`command.h`](../command/command.h). Describes one supported
command:

- `name` is its registered name.
- `minimumArguments` and `maximumArguments` define valid command arities,
  counting the command name itself.
- `handler` points to the free function that implements the command.

`CommandTable::find` returns a pointer to a matching spec, or `nullptr` when
the name is unknown.

### `CommandHandler`

Declared in [`command.h`](../command/command.h) as a function-pointer alias.
A handler receives its `CommandContext` and the command's RESP array. The
current handlers are `cmd_ping`, `cmd_echo`, and `cmd_quit`.

### `RespValue` and `RespValue::Array`

Declared in [`resp.h`](../resp/resp.h). A `RespValue` stores one RESP value in
a `std::variant`. For commands, the event loop expects the value to contain an
`Array`; the array's elements must be `BulkString` values. Each bulk string is
one command argument, with element zero holding the command name.

The variant alternatives are:

- `SimpleString`: a RESP simple string.
- `ErrorString`: a RESP error value.
- `std::int64_t`: a RESP integer.
- `BulkString`: a RESP bulk string; the event loop uses this for command names
  and arguments.
- `RespValue::Array`: a sequence of nested RESP values; command requests use
  this representation.
- `NullValue`: a RESP null.

The parser can represent all of these types, but this command dispatcher
rejects non-array commands and non-bulk-string arguments with an error reply.

### `ParseResult` and `ParseStatus`

Declared in [`resp.h`](../resp/resp.h). `RespParser::parse` returns a
`ParseResult` containing:

- `status`: `NeedMoreData`, `Command`, or `ProtocolError`.
- `value`: the parsed value when the status is `Command`.
- `error`: parser diagnostic text for a protocol error.

The event loop waits for more socket bytes on `NeedMoreData`, dispatches a
complete value on `Command`, and queues a protocol-error reply before closing
on `ProtocolError`.

### `Buffer`

Declared in [`resp.h`](../resp/resp.h). A growable byte buffer with read and
write positions. The event loop uses `append` to add received bytes or
generated replies, `readable` to access pending output, `consume` after
successful writes or parsing, and `empty` to check whether bytes remain.

### `Socket`

Declared in [`socket.h`](./socket.h). A move-only RAII wrapper around a file
descriptor. The event loop uses it to accept clients, set non-blocking mode and
TCP_NODELAY, and perform partial reads and writes. `nativeHandle()` supplies
the fd to `epoll_ctl` and for identifying the listener in returned events;
socket I/O itself goes through the wrapper.

### `epoll_event`

A Linux system struct from `<sys/epoll.h>`, not a project-defined struct. It
contains the readiness flags and the fd-associated value registered by the
program. Here, `data.fd` stores the socket fd and `events` carries flags such
as `EPOLLIN` (readable), `EPOLLOUT` (writable), and `EPOLLRDHUP` (peer closed
its write side).

## Constants and helper functions

### `kReadBufferSize`

Maximum number of bytes read in one `recv` call: 4096. Larger requests are
handled by subsequent reads.

### `kMaxEvents`

Maximum number of readiness events returned by one `epoll_wait`: 1024.

### `kTimerTickMilliseconds`

The maximum time one `epoll_wait` blocks: 100 ms. This provides a periodic
return to the main loop, but this implementation does not yet run timer
callbacks.

### `throwSystemError`

Captures the current `errno` and throws a `std::system_error` tagged with the
operation name. Used for fatal system-call failures that should propagate to
the caller.

### `epollControl`

Builds an `epoll_event` with the requested flags and fd, then calls
`epoll_ctl`. It throws if adding, modifying, or registering a descriptor
fails. `closeConnection` performs deletion separately because it handles
expected deletion errors specially.

### `appendCommandError`

Writes a RESP error reply into an output `Buffer` using `RespWriter`. This
keeps command/protocol errors queued for normal non-blocking output handling.

### `logCommand`

Writes one line to `std::clog` for each parsed command, including the client
ID and command name. It intentionally omits arguments, which may contain
credentials or user data. Non-printable bytes, backslashes, and quotes in the
name are escaped so the name cannot inject additional log lines or corrupt the
quoted format. A value with no usable bulk-string command name is logged as
`<malformed>`.

## `Connection` constructor

`Connection::Connection(Socket, std::uint64_t)` moves the accepted socket into
the connection and stores the assigned client ID. The input/output buffers
start empty and both lifecycle flags start false.

## `EventLoop` functions

### `EventLoop::EventLoop`

Creates the listening TCP socket, makes it non-blocking, binds it to the
requested port on all IPv4 interfaces, and starts listening with backlog 128.
It then creates the epoll instance and registers the listener for `EPOLLIN`.

If creating or registering the epoll instance fails, it closes the epoll fd
before rethrowing. The `Socket` member still owns and closes the listening
socket automatically.

### `EventLoop::~EventLoop`

Closes the epoll fd if it was created. After the destructor body, normal
member destruction releases the listener and all live client `Socket`s.

### `EventLoop::runOnce`

Waits up to 100 ms for up to 1024 events:

- `EINTR` means a signal interrupted the wait, so this iteration returns
  without treating it as a fatal error.
- Other `epoll_wait` errors are thrown.
- Listener events call `acceptClients`.
- Client events call `handleClientEvent`.
- After all returned events have been examined,
  `finishDeferredCloses` destroys connections marked for closure.

Deferring destruction to the end of the batch matters because an fd must not
be closed and reused while older events in that same returned batch may still
refer to it.

### `EventLoop::acceptClients`

Accepts repeatedly until the non-blocking listener reports `EAGAIN` or
`EWOULDBLOCK`, meaning there are no more queued connections right now. For
each accepted socket it:

1. Enables non-blocking mode and TCP_NODELAY.
2. Assigns a new client ID and creates a `Connection`.
3. Grows the fd-indexed connection vector if necessary.
4. Registers the socket for readable data and peer half-close notifications.
5. Stores the owning connection in the fd's vector slot.

Other accept errors propagate. Socket setup and registration errors also
propagate; RAII closes the local socket/connection if it was not successfully
stored.

### `EventLoop::handleClientEvent`

Looks up the connection by fd and ignores invalid fds, empty slots, or
connections already scheduled to close. It then:

- Closes on `EPOLLERR`.
- Reads on `EPOLLIN`, `EPOLLRDHUP`, or `EPOLLHUP`, so available final bytes
  are drained before handling EOF.
- Writes on `EPOLLOUT` if the read path did not schedule closure.

### `EventLoop::readFromClient`

Reads repeatedly into a 4096-byte temporary array:

- `EAGAIN`/`EWOULDBLOCK` means all currently available data has been read.
- Other read errors schedule the connection for closure.
- A zero-byte read means peer EOF. The connection is marked to close after
  queued output is sent.
- Positive reads are appended to the input buffer, then passed to
  `dispatchCommands`.

After reading, it closes immediately if EOF was reached and there is no reply
to flush; otherwise it updates epoll interest. Incomplete buffered input is
discarded when the peer disconnects and the connection is eventually
destroyed.

### `EventLoop::dispatchCommands`

Parses as many complete RESP values as possible from the connection's input
buffer. Parsing consumes bytes for complete values, allowing pipelined
commands to be dispatched in order. If input is incomplete, it returns and
waits for another read. On a protocol error, it queues `-ERR protocol error`,
marks the connection to close after the reply, and stops parsing. It also
stops after a command such as QUIT requests closure.

### `EventLoop::dispatchCommand`

Handles one parsed RESP value:

1. Logs the command name and client ID.
2. Requires a non-empty RESP array whose elements are all bulk strings.
3. Looks up the first bulk string in `CommandTable`.
4. Queues `ERR unknown command` if no command matches.
5. Checks the array length against the command's arity and queues a
   wrong-number-of-arguments error if invalid.
6. Builds a `CommandContext` and calls the command handler.

The dispatcher validates the RESP shape and arity; each handler implements
its command-specific reply behavior.

### `EventLoop::writeToClient`

Attempts to send all currently pending output using `Socket::writeSome`.
Successful partial writes advance a local count; `EAGAIN`/`EWOULDBLOCK`
leaves the remaining bytes queued for a later `EPOLLOUT` event. Other send
errors and a zero-byte send schedule closure.

After the loop, it consumes only the bytes successfully written. If output is
empty and closure was requested, it schedules closure; otherwise it updates
epoll interest.

### `EventLoop::updateInterest`

Modifies the registered event flags for a client:

- Always watches `EPOLLRDHUP`.
- Watches `EPOLLIN` while the connection is still accepting commands.
- Watches `EPOLLOUT` only when the output buffer is non-empty.

Not watching `EPOLLOUT` for an empty buffer avoids a continuous stream of
write-ready notifications.

### `EventLoop::closeConnection`

Schedules a connection for closure exactly once. It marks the connection as
closing, removes its fd from epoll, and adds the fd to `deferredCloses_`.
`ENOENT` and `EBADF` from epoll deletion are tolerated because the descriptor
may already have been removed or closed; other deletion errors are written to
`std::cerr`. The socket itself remains owned by the connection until deferred
cleanup.

### `EventLoop::finishDeferredCloses`

Resets the connection slot for every deferred fd, destroying the connection
and its RAII socket, then clears the deferred-fd list. It runs only after the
current `epoll_wait` batch has been processed.

## Errors and connection lifetime

Socket readiness errors and malformed RESP input do not terminate the server:
the affected client is replied to when possible and then closed. Unexpected
epoll or accept failures propagate as exceptions to the server's top-level
error handler.

The single close path is `closeConnection`. It removes the fd from epoll and
defers destruction until the current event batch ends. Keeping the old socket
open during that batch prevents the operating system from assigning its fd to
a newly accepted connection before stale events in the batch have been
examined.

## Scope and current limitations

- The listener binds IPv4 `INADDR_ANY`; clients are not restricted to loopback.
- Only RESP arrays of bulk strings are accepted as commands.
- The implemented command table currently contains PING, ECHO, and QUIT.
- Command logs include names and client IDs, not arguments.
- The 100 ms epoll timeout is only a periodic wake-up; no timer callback
  system is implemented here.
- This event loop uses a vector indexed by fd, so the vector can grow to the
  largest fd value observed.
