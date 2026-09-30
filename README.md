# file_transfer

`file_transfer` is a Linux command-line program that sends one file per client process to a continuously running server. Both modes are in the same C++20 executable. Transfers are streaming, checksummed, and published only after complete validation.

## Requirements

- Linux with `/proc/self/exe` and POSIX sockets
- CMake 3.20 or newer
- A C++20 compiler
- No third-party libraries

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

Start the long-running server:

```bash
./build/file_transfer -s
```

Send a file from another terminal:

```bash
./build/file_transfer -c ./example.bin
```

The defaults are server bind `0.0.0.0:5000` and client destination `127.0.0.1:5000`. Override them with `--port PORT` and client `--host HOST`:

```bash
./build/file_transfer -s --port 5001
./build/file_transfer -c ./example.bin --host 192.0.2.10 --port 5001
```

The server accepts one connection at a time and gives each accepted connection its own worker thread, up to 32 simultaneous clients; additional connections are closed and logged. TCP reads use a 30-second inactivity timeout so stalled peers release worker capacity. While idle the accept loop blocks in `poll()` with no timeout, so it uses no busy loop or periodic polling. A wake pipe notifies it both when a worker finishes and when shutdown is requested. `--port 0` asks the OS for an ephemeral port and is useful for tests.

## Output files

Received files are saved beside the server executable, independent of its working directory. Names use UTC with microseconds: `YYYYMMDD_HHMMSS_ffffff.hex`. `.hex` is only the required filename extension; payload bytes are not transformed.

Each transfer first writes to `payload.part` inside a private mode-0700 staging directory beside the executable. The server verifies the staging directory owner and permissions, keeps its directory descriptors open, and publishes relative to those descriptors, so the final file is the same inode the server opened and validated. After declared size, block order, per-block CRC32, whole-file CRC32, and FINISH metadata all match, it fsyncs the file, atomically publishes without replacing an existing name, then fsyncs both affected directories before reporting success. Failed or disconnected transfers explicitly remove their staging file and directory; cleanup failures are reported rather than silently treated as successful cleanup.

If directory fsync fails after publication, the server attempts to remove the final name and reports failure. If rollback itself fails, or its directory fsync fails and the name could return after a crash, the remaining file is already fully size/CRC validated; the error names its path so it is not mistaken for a partial transfer.

## Architecture

`CLI -> application -> transfer -> protocol -> ITransport -> TcpTransport`; `ITransportListener` provides the server-side accept boundary, and `IFileStore`/`IStagedFile` isolate file publication. The protocol and transfer layer use no TCP API. A future transport can implement the byte-stream interface without changing file transfer logic.

CRC32 is implemented locally using the standard reflected IEEE polynomial. The client makes two streaming passes over a regular input file: the first computes size and whole-file CRC32, and the second sends fixed 64 KiB chunks with their individual CRC32 values. Memory use remains bounded regardless of file size.

## Protocol

The wire format is versioned and uses explicit network-byte-order fields; no C++ structure is sent directly. Frames contain magic, version, message type, payload length, and a type-specific payload. DATA messages carry a zero-based sequence number, payload size, block CRC32, and bytes. FINISH repeats total bytes, chunks, and whole-file CRC32. The server returns RESULT success or an error. Full field sizes and validation rules are in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Failure and shutdown behavior

The server detects EOF, malformed frames, sequence/size mismatches, CRC errors, and storage failures. A failed connection is isolated to its worker; the server keeps accepting clients. SIGINT and SIGTERM stop accepting, cancel active socket I/O, join workers, and preserve files already published. SIGPIPE is ignored and TCP writes also use `MSG_NOSIGNAL`.

The ASCII `-c` flag is required. A visually similar flag containing Cyrillic `с` is rejected with an explicit diagnostic. Invalid arguments and transfer errors return a nonzero exit status.

## Tests

The dependency-free unit runner checks CRC32 vectors and incremental updates, endian encoding and protocol round trips, partial reads/writes, EOF, CLI validation, generated names, and injected storage errors. The process integration suite starts a real server and clients in a temporary directory and checks empty, one-byte, binary, 32 MiB, and eight concurrent transfers byte-for-byte. It also kills a client during a 512 MiB sparse-file transfer, verifies `.part` cleanup and absence of `.hex`, verifies recovery with the next client, and checks that server output follows the executable directory rather than the working directory.

Run just one lane with `ctest --test-dir build --output-on-failure -R unit` or `-R integration`.

## Limitations

- TCP is unauthenticated and unencrypted; use only on a trusted network or add a protected transport before exposing it to untrusted clients.
- Interrupted transfers are discarded; resume is not supported.
- Each simultaneous connection consumes one thread.
- No sender-provided filename is stored; server-generated names avoid path traversal and collisions.