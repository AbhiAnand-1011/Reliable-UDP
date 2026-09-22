# Reliable-UDP

A custom **Reliable UDP (RUDP) file-transfer protocol** implemented in C++ using POSIX UDP sockets.

Reliable-UDP takes the connectionless and unreliable nature of UDP and builds a small application-level reliability layer on top of it. The protocol provides session initialization, sequence-numbered file chunks, acknowledgments, timeout-based retransmissions, packet integrity validation, ordered file reconstruction, and end-to-end CRC32 verification.

The project is implemented from the ground up rather than relying on TCP, providing hands-on experience with networking, transport-layer concepts, packet formats, binary serialization, reliability mechanisms, checksums, and POSIX socket programming.

---

## Features

- Custom binary packet protocol on top of UDP
- `HELLO` / `HELLO_ACK` session initialization
- Sequence-numbered data packets
- 1200-byte file chunking
- Bounded sender transmission window of 32 packets
- Per-packet acknowledgments
- Timeout-based retransmission
- Maximum retransmission limit
- CRC32 packet integrity validation
- End-to-end CRC32 verification of the reconstructed file
- Out-of-order packet reception
- Ordered chunk reassembly
- Temporary `.part` files for received chunks
- Filename sanitization to prevent path traversal
- `FIN` / `FIN_ACK` transfer teardown
- Configurable receiver port and output directory
- Binary file transfer support

---

## Architecture

The implementation is divided into three logical layers.

### 1. Protocol Layer

**Files:**
- `protocol.h`
- `protocol.cpp`

This layer defines the wire protocol and is responsible for:

- Packet types
- Packet headers
- Serialization
- Deserialization
- Network byte order conversion
- CRC32 calculation
- HELLO metadata encoding/decoding
- Packet construction
- Bitmap encoding/decoding helpers
- NACK packet construction

The protocol currently defines the following packet types:

```text
HELLO
HELLO_ACK
DATA
ACK
NACK
FIN
FIN_ACK
```

`NACK` packets and bitmap helpers are implemented in the protocol layer but are currently not used by the active sender/receiver transfer path.

---

### 2. Utility Layer

**Files:**
- `utils.h`
- `utils.cpp`

This layer handles local file operations and supporting functionality:

- Reading files into fixed-size chunks
- Writing received chunks to temporary files
- Reassembling chunks in sequence-number order
- Creating output directories
- CRC-related file processing
- Monotonic time measurement
- Filename sanitization

---

### 3. Endpoint Layer

**Files:**
- `sender.cpp`
- `receiver.cpp`

#### Sender

The sender:

1. Opens the input file.
2. Splits it into 1200-byte chunks.
3. Calculates the complete file CRC32.
4. Sends a `HELLO` containing file metadata.
5. Waits for `HELLO_ACK`.
6. Sends sequence-numbered `DATA` packets.
7. Tracks acknowledgments for individual chunks.
8. Retransmits unacknowledged packets after the timeout expires.
9. Sends `FIN` after all chunks have been acknowledged.
10. Waits for `FIN_ACK`.

#### Receiver

The receiver:

1. Binds a UDP socket to the requested port.
2. Waits for a valid `HELLO`.
3. Validates the file metadata.
4. Sanitizes the transmitted filename.
5. Sends `HELLO_ACK`.
6. Accepts `DATA` packets in any order.
7. Writes each received chunk to a temporary `.part` file.
8. Sends an `ACK` for each valid chunk.
9. Waits until every expected sequence number has been received.
10. Reassembles the file in sequence order.
11. Verifies the final file CRC32.
12. Waits for a valid `FIN`.
13. Sends `FIN_ACK`.
14. Exits after a short grace period for duplicate/retransmitted `FIN` packets.

---

## Packet Format

Every packet contains a fixed binary header followed by an optional payload.

```text
+---------+------+----------+--------------+-------------+----------+
| Version | Type | Seq      | Total Chunks | Payload Len | Checksum |
+---------+------+----------+--------------+-------------+----------+
| 1 byte  |1 byte| 4 bytes  |   4 bytes    |   2 bytes   | 4 bytes  |
+---------+------+----------+--------------+-------------+----------+
|                         Payload                                  |
+-------------------------------------------------------------------+
```

### Header fields

| Field | Size | Description |
|---|---:|---|
| Version | 1 byte | Protocol version |
| Type | 1 byte | Packet type |
| Sequence Number | 4 bytes | Chunk/packet sequence number |
| Total Chunks | 4 bytes | Number of chunks in the file |
| Payload Length | 2 bytes | Size of the payload |
| Checksum | 4 bytes | CRC32 integrity checksum |

All multi-byte integer fields are serialized using network byte order.

The protocol limits UDP datagrams to:

```text
MAX_PACKET_SIZE = 1400 bytes
```

File data is divided into:

```text
CHUNK_SIZE = 1200 bytes
```

File chunks are limited to 1200 bytes, leaving room for the custom 16-byte header while keeping the resulting UDP datagrams below 1400 bytes.

---

## Checksum and Integrity

The protocol uses **CRC32** for integrity checking.

For serialized packets, the checksum is calculated over the canonical representation of:

```text
version
type
sequence number
total chunks
payload length
payload
```

The checksum field itself is excluded from the calculation.

In addition to packet-level integrity validation, the sender calculates a CRC32 over the complete original file and includes it in the `HELLO` metadata.

After receiving all chunks, the receiver:

1. Reassembles the file.
2. Calculates the CRC32 of the reconstructed file.
3. Compares it with the expected file CRC.
4. Accepts the transfer only when the CRC values match.

This provides both packet-level corruption detection and end-to-end file integrity verification.

CRC32 is an integrity mechanism, **not cryptographic authentication or encryption**.

---

## Reliability Mechanism

UDP itself does not guarantee:

- Delivery
- Ordering
- Duplicate suppression
- Retransmission
- Data integrity

Reliable-UDP implements these mechanisms at the application layer.

### Sequence Numbers

Every `DATA` packet receives a sequence number:

```text
0, 1, 2, 3, ...
```

The receiver stores chunks using these sequence numbers and reconstructs the final file in numerical order.

This allows packets to arrive out of order without corrupting the resulting file.

### Acknowledgments

For every valid `DATA` packet received, the receiver sends an `ACK` containing the corresponding sequence number.

The sender maintains state for every chunk:

```text
sent
acked
last_sent_time
retry_count
```

### Retransmission

If a packet remains unacknowledged beyond:

```text
TIMEOUT_MS_DEFAULT = 500 ms
```

the sender retransmits it.

The maximum retry count is:

```text
MAX_RETRIES = 10
```

If the retransmission limit is exceeded, the transfer fails.

### Transmission Window

The sender limits new outstanding transmissions to:

```text
DEFAULT_WINDOW = 32 packets
```

This provides a bounded transmission window instead of attempting to send the entire file at once.

The current implementation does **not** implement TCP-style congestion control, adaptive RTT estimation, selective ACK negotiation, or fast retransmit.

---

## Protocol Flow

A successful transfer follows this general sequence:

```text
Sender                                  Receiver
  |                                        |
  | ----------- HELLO -------------------> |
  |                                        |
  | <--------- HELLO_ACK ----------------- |
  |                                        |
  | ----------- DATA #0 -----------------> |
  | <----------- ACK #0 ------------------ |
  |                                        |
  | ----------- DATA #1 -----------------> |
  | <----------- ACK #1 ------------------ |
  |                                        |
  |              ...                       |
  |                                        |
  | ----------- DATA #N -----------------> |
  | <----------- ACK #N ------------------ |
  |                                        |
  | ----------- FIN ---------------------> |
  | <--------- FIN_ACK ------------------- |
  |                                        |
```

If an individual data packet is lost, the sender retransmits it after the timeout expires.

---

## Project Structure

```text
Reliable-UDP/
│
├── sender.cpp
├── receiver.cpp
│
├── protocol.cpp
├── protocol.h
│
├── utils.cpp
├── utils.h
│
├── README.md
└── .vscode/
```

---

## Requirements

### Operating System

The project uses POSIX networking APIs and filesystem functionality.

Recommended environment:

- Linux
- WSL
- Other POSIX-compatible environments with the required C++17 support

### Compiler

A C++ compiler with C++17 support is required.

For example:

```bash
g++
```

---

## Building

Clone the repository:

```bash
git clone https://github.com/AbhiAnand-1011/Reliable-UDP.git
cd Reliable-UDP
```

Compile the receiver:

```bash
g++ -std=c++17 -O2 -Wall -Wextra receiver.cpp protocol.cpp utils.cpp -o receiver
```

Compile the sender:

```bash
g++ -std=c++17 -O2 -Wall -Wextra sender.cpp protocol.cpp utils.cpp -o sender
```

After compilation:

```bash
ls -lh sender receiver
```

---

## Basic Local Test

The simplest test runs both endpoints on the same machine using the loopback interface.

### 1. Create a test file

```bash
echo "Hello from Reliable-UDP" > test.txt
```

### 2. Start the receiver

Open Terminal 1:

```bash
mkdir -p received
./receiver 9000 received
```

Expected output:

```text
receiver listening on port 9000
```

### 3. Start the sender

Open Terminal 2:

```bash
./sender 127.0.0.1 9000 test.txt
```

The sender should report the handshake, transmission, acknowledgments, and final teardown.

### 4. Verify the received file

```bash
cat received/test.txt
```

Then compare the files directly:

```bash
cmp test.txt received/test.txt && echo "TRANSFER OK"
```

Expected:

```text
TRANSFER OK
```

---

## Testing a Larger File

The protocol is designed around 1200-byte chunks, so a larger file is useful for exercising multiple packets and acknowledgments.

For example:

```bash
dd if=/dev/urandom of=large_test.bin bs=1M count=10
```

Start the receiver:

```bash
mkdir -p received
./receiver 9000 received
```

Then transfer the file:

```bash
./sender 127.0.0.1 9000 large_test.bin
```

Verify the result:

```bash
cmp large_test.bin received/large_test.bin && echo "TRANSFER OK"
```

You can also compare cryptographic hashes independently:

```bash
sha256sum large_test.bin received/large_test.bin
```

The two SHA-256 values should match.

---

## Testing Between Two Machines

The receiver binds to:

```text
0.0.0.0:<port>
```

so it can accept UDP packets arriving through the machine's network interfaces.

On the receiving machine:

```bash
./receiver 9000 received
```

Find the receiver's IP address:

```bash
ip addr
```

Then run the sender from another machine:

```bash
./sender <receiver-ip> 9000 test.txt
```

For example:

```bash
./sender 192.168.1.20 9000 test.txt
```

The UDP port must be reachable between the two machines, and any host firewall must allow the selected UDP port.

---

## Current Implementation Characteristics

| Parameter | Value |
|---|---:|
| Protocol version | 1 |
| Data chunk size | 1200 bytes |
| Maximum UDP datagram size | 1400 bytes |
| Sender window | 32 packets |
| Retransmission timeout | 500 ms |
| Maximum retries | 10 |
| Integrity mechanism | CRC32 |
| Transport | UDP |
| Socket API | POSIX sockets |

---

## Current Limitations

This implementation is intentionally focused on the core mechanisms of a reliable UDP file-transfer protocol.

### No congestion control

There is currently no TCP-like congestion-control algorithm such as:

- Slow start
- Congestion avoidance
- Fast retransmit
- Fast recovery
- Dynamic congestion window

### Fixed retransmission timeout

The retransmission timeout is currently a fixed:

```text
500 ms
```

It is not calculated dynamically from measured RTT.

### No selective ACK bitmap in active transfer

Bitmap encode/decode functionality exists in the protocol layer, but the current sender and receiver use individual ACK packets for received sequence numbers.

### NACK is not currently used

`NACK` packet construction exists but the active transfer path does not currently send or process NACK packets.

### Sender memory usage

The sender reads the complete file into memory as a collection of chunks before starting transmission.

### Temporary chunk files

The receiver stores incoming chunks as:

```text
0.part
1.part
2.part
...
```

These temporary files are used during reconstruction and are currently not automatically deleted after the transfer.

### One transfer per receiver process

The receiver handles a transfer and then exits after its FIN grace period rather than operating as a continuously available multi-client server.

### Empty-file edge case

Empty files are recognized as zero-chunk transfers and can complete the protocol exchange, but the current receiver path does not create a zero-byte output file.

### No encryption or authentication

The protocol provides integrity checking with CRC32 but does not provide:

- Encryption
- Authentication
- Confidentiality
- Protection against an active attacker modifying packets

---

## What This Project Demonstrates

This project provides practical implementation experience with:

- UDP socket programming
- POSIX networking APIs
- Binary protocol design
- Packet serialization/deserialization
- Network byte order
- Sequence numbers
- Bounded transmission windows
- ACK-based reliability
- Timeout-based retransmission
- Duplicate handling
- Out-of-order delivery
- File chunking
- File reassembly
- CRC32 integrity checks
- Connection/session state machines
- Basic protocol validation
- C++17 filesystem and networking functionality

---

## Future Improvements

Possible extensions include:

- Selective acknowledgment bitmaps
- Active NACK handling
- Adaptive RTT estimation
- Dynamic retransmission timers
- Congestion control
- Fast retransmission
- Zero-copy or streaming file transmission
- Better duplicate/session handling
- Transfer statistics and throughput measurement
- Packet-loss simulation for automated reliability testing
- Packet corruption simulation
- Automated protocol tests
- Persistent multi-client receiver
- Optional encryption/authentication layer

---

## License

This project is provided for educational and experimental purposes.
