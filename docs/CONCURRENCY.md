# Concurrency Model

## Old model

The server previously created one blocking worker thread for every accepted socket and rejected connections after a fixed limit of 32. A transfer driver assumed that one read returned the next complete protocol frame.

## Event loop

The server owns one level-triggered Linux `epoll` loop. A listener callback drains non-blocking `accept()` until `EAGAIN`.

The event loop stores registrations in an owning map. The callback registration is retired until the current wait batch finishes, so removing a handler cannot free the object behind an already returned readiness event. A handler owns its transport, parser, transfer counters, staging file, and pending output.

## State machine

Each server connection moves through `AwaitHello`, `Receiving`, `AwaitFinish`, and `SendingResult`. DATA blocks are validated for sequence, size, block CRC, total size, and whole-file CRC before publication. `FrameParser` accepts split headers, split payloads, and multiple frames in one read.

Each client file is an independent `ClientTransfer`. A scheduler keeps at most `--max-active` sockets active and starts the next queued file when one finishes. A failed file increments the aggregate failure count but does not cancel other files.

## Readiness and fairness

`EPOLLIN`/`EPOLLOUT` are enabled only for work that can be performed. A pending RESULT is a byte queue: partial writes preserve the unsent suffix and disable write interest after the queue drains. One read callback consumes at most 256 KiB before returning to dispatch.

## Timeouts

`timerfd` wakes the loop for activity deadlines. The same activity timestamp covers receive inactivity and a stalled pending write. The default is approximately 30 seconds; `--idle-timeout-ms` controls the timeout. A timeout removes the registration, closes the socket, and discards staging state.

## Shutdown

`signalfd` delivers SIGINT and SIGTERM to the loop. Shutdown stops accepting work, closes active handlers, and leaves the output directory with no partial staging state. `MSG_NOSIGNAL` and ignored SIGPIPE prevent a broken peer from terminating the process.

## Scaling and limitations

There is no application-level 32-connection cap. The practical limit is the process file-descriptor limit, memory, kernel socket buffers, and synchronous storage commit latency. `fsync` and publication remain inline so the durable storage contract is unchanged.

The client currently computes a file's initial metadata before opening its connection. Network I/O is event-driven after connection setup, but very large preflight hashes can still consume CPU before dispatch begins. The protocol remains one connection per file; files are not multiplexed inside one TCP stream.