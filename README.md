# TCP Throughput Benchmark

Measures unidirectional TCP throughput between two machines for message sizes ranging from 1 byte to 1 MB (powers of two, 21 sizes). Built for a university networking course.

## Build

```bash
make all      # builds server and client
make test     # builds and runs unit tests
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
| `common.h` / `common.cpp` | Shared socket utilities, throughput computation, message-size generation |
| `server.cpp` | Listens, receives batches, sends acknowledgments |
| `client.cpp` | Connects, sends timed batches, prints results |
| `test_common.cpp` | Unit tests for pure functions |
| `Makefile` | Build system |
| `GUIDE.md` | Detailed walkthrough with rationale for every code section |
