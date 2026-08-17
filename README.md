# RUDP: Reliable User Datagram Protocol

## Overview
SIUU-RUDP is a custom Reliable User Datagram Protocol implementation written in C++. It facilitates reliable file transfers over standard UDP sockets by introducing TCP-like mechanisms such as connection handshakes, sequence numbering, CRC32 checksum validation, acknowledgment (ACK) packets, and sliding-window retransmissions.

## Architecture
The project is divided into three distinct logical layers:

1. **Protocol Layer (`protocol.h` / `protocol.cpp`)**
   Defines the network rulebook. It specifies the packet structures, packet types (HELLO, DATA, ACK, FIN, etc.), and the serialization/deserialization logic required to translate C++ structures into raw byte streams for network transmission. It also handles data integrity via CRC32 hashing.

2. **Utility Layer (`utils.h` / `utils.cpp`)**
   Manages local file system interactions. It is responsible for chunking outgoing files into 1200-byte segments, writing incoming segments to temporary `.part` files, and reassembling the complete file once all chunks are successfully received.

3. **Endpoint Layer (`sender.cpp` / `receiver.cpp`)**
   The network actors implementing the state machines. 
   - The **Sender** acts as the client, handling file reading, the sliding transmission window, and timeout-based retransmissions.
   - The **Receiver** acts as the server, binding to a port, verifying data integrity, acknowledging received packets, and managing file assembly.

### Packet Structure
Custom packets are restricted to a safe 1400-byte maximum size to avoid IP fragmentation. Each packet includes a custom header:
- **Version:** Protocol version identifier.
- **Type:** Packet classification (e.g., HELLO, DATA, ACK).
- **Sequence Number:** Unique ID for chunk ordering.
- **Total Chunks:** Expected file size in chunks.
- **Payload Length:** Size of the appended data.
- **Checksum:** CRC32 hash of the payload for corruption detection.

## Process and Data Flow
The file transfer follows a strict lifecycle to ensure absolute reliability over a connectionless protocol.

1. **The Handshake:** The Sender transmits a `HELLO` packet containing the file metadata (name, total chunks, full file CRC). The Receiver validates this and responds with a `HELLO_ACK`.
2. **Data Transmission:** The Sender transmits `DATA` packets up to a defined sliding window limit (default: 32 in-flight packets). 
3. **Validation and Acknowledgment:** The Receiver catches `DATA` packets, recalculates the CRC32 checksum, and compares it to the header. If valid, the chunk is written to disk, and an `ACK` packet is immediately fired back to the Sender.
4. **Retransmission:** The Sender monitors incoming ACKs. If a packet remains unacknowledged past the 500ms timeout threshold, it is assumed dropped and is automatically retransmitted.
5. **Assembly and Teardown:** Once the Receiver possesses all expected chunks, it assembles the `.part` files into the final output file. The Sender issues a `FIN` packet, the Receiver replies with a `FIN_ACK`, and both sockets close cleanly.

## Usage Instructions

### Prerequisites
- A POSIX-compliant environment (Linux, macOS, or WSL on Windows).
- GCC/G++ compiler installed with C++17 support or higher.

### Step 1: Compilation
Navigate to the root directory of the project in your terminal and compile the endpoint binaries using `g++`.

**Compile the Receiver:**
    g++ receiver.cpp protocol.cpp utils.cpp -o receiver

**Compile the Sender:**
    g++ sender.cpp protocol.cpp utils.cpp -o sender

### Step 2: Set Up the Environment
To test the file transfer, you will need two separate terminal windows. Create a simple text file to serve as the test payload:
    echo "This is a test file for the SIUU-RUDP protocol." > test_file.txt

### Step 3: Run the Receiver
In the first terminal, start the receiver. Provide the port number you want it to listen on, followed by the directory where you want downloaded files to be saved.
    ./receiver 8080 ./downloads

### Step 4: Run the Sender
In the second terminal, start the sender. Provide the target IP address (use `127.0.0.1` for local testing), the target port, and the path to the file you wish to send.
    ./sender 127.0.0.1 8080 test_file.txt

You will see console output in both terminals detailing the handshake, chunk acknowledgments, and connection teardown. The fully assembled file will be available in the receiver's specified output directory.
