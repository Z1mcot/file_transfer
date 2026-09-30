# File Transfer Protocol, Version 1

All integers are unsigned and encoded in network byte order (big-endian). The protocol is a framed byte stream. Receivers must handle partial reads and writes; frame boundaries do not correspond to TCP packet boundaries.

## Frame header

Every frame begins with this 12-byte header:

| Offset | Size | Field | Value / meaning |
|---:|---:|---|---|
| 0 | 4 | Magic | ASCII `FTRN` (`0x4654524e`) |
| 4 | 2 | Version | `1` |
| 6 | 2 | Type | `1` HELLO, `2` DATA, `3` FINISH, `4` RESULT |
| 8 | 4 | Payload length | Number of payload bytes immediately following the header |

The receiver rejects a bad magic, unsupported version, unknown type, truncated frame, or payload larger than 65,552 bytes. The maximum DATA payload is 16 bytes of metadata plus 65,536 data bytes.

## Message payloads

### HELLO (type 1), 16 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | File size in bytes (`uint64`) |
| 8 | 4 | Whole-file CRC32 (IEEE) |
| 12 | 4 | Requested DATA chunk size, 1 through 65,536 |

The client sends exactly one HELLO before any data. V1 uses the requested chunk size as the maximum size of each DATA body.

### DATA (type 2), 16-byte prefix plus data

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Zero-based sequence number |
| 8 | 4 | Data length |
| 12 | 4 | CRC32 of this DATA body |
| 16 | variable | File bytes |

DATA length must be nonzero, no greater than the HELLO chunk size, and equal to the expected chunk length for the remaining declared file size. Sequence numbers start at zero and increase by one. Empty files contain no DATA frames.

### FINISH (type 3), 20 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Total file bytes sent |
| 8 | 8 | Total DATA chunks sent |
| 16 | 4 | Whole-file CRC32 |

The receiver requires the totals to match the bytes and chunks actually received. The FINISH CRC must match both HELLO and the receiver's streaming CRC.

### RESULT (type 4), 4-byte prefix plus text

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Result code; `0` means success, nonzero means failure |
| 2 | 2 | Message byte length, at most 512 |
| 4 | variable | Human-readable message bytes |

The server sends a success RESULT only after the complete file has been validated and atomically published. It attempts to send a nonzero RESULT after a transfer error when the connection remains writable. A client considers the operation successful only after receiving code zero.

## CRC32

CRC is CRC-32/ISO-HDLC (IEEE reflected polynomial `0xedb88320`, initial value `0xffffffff`, final XOR `0xffffffff`). The standard ASCII test vector `123456789` yields `0xcbf43926`; the empty payload yields zero. CRC detects accidental corruption; it is not authentication.

## Transfer sequence

```text
client -> server: HELLO
client -> server: DATA sequence 0..N-1
client -> server: FINISH
server -> client: RESULT
```

Any unexpected message, disconnect, malformed length, sequence gap, size mismatch, per-block checksum mismatch, whole-file checksum mismatch, or storage failure aborts the transfer. The server deletes the staging `.part` file and continues serving other connections.