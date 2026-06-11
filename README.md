# TCP Throughput Benchmark

Measures unidirectional TCP throughput between two machines for message sizes ranging from 1 byte to 1 MB (powers of two, 21 sizes). Built for a university networking course.

## Build

```bash
make all      # builds server and client
make clean    # removes binaries
```

Requires `g++` with C++17 and POSIX sockets (Linux).

## Run

On the server machine:
```bash
./server
```

On the client machine:
```bash
./client <server-ip>
```

The port is hardcoded to `12345`. The client prints 21 lines of tab-delimited output:

```
<message-size-bytes>	<throughput-value>	<unit>
```

Units auto-scale: `bps`, `Kbps`, `Mbps`, or `Gbps`.

## Files

| File | Purpose |
|------|---------|
| `server.cpp` | Listens, accepts one connection, drains bytes in batched 1 MB chunks, sends ACKs |
| `client.cpp` | Connects, sends warmup+timed batches per size, measures elapsed time, prints results |
| `Makefile` | Build system |
| `convergence_detector/` | Tool to find optimal `MSG_COUNTS` values per message size |
| `warmup_probe/` | Tool to find optimal `WARMUP_COUNTS` values per message size |
| `Exercise_1_Performance_Measurements_TCP.md` | Lab report with methodology, design decisions, and results |
| `FirstDraft/` | Reference implementation used as basis for current code |
