#!/usr/bin/env python3
"""rr_proxy.py LISTEN_PORT PIDFILE LOGFILE BACKEND_PORT... - a loopback TCP proxy that sends each new connection to the
next backend in turn, the way kube-proxy spreads connections to a Kubernetes Service over its endpoints. It stands in
for a ClickHouse "cluster Service" (one address in front of both replicas) in isolated upgrade-path tests. Listens on
127.0.0.1 only and connects only to 127.0.0.1 backends; logs one line per connection (backend chosen)."""
import asyncio
import itertools
import os
import sys
import time


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main():
    port, pidfile, logfile = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    backends = [int(p) for p in sys.argv[4:]]
    rr = itertools.cycle(backends)
    log = open(logfile, "a", buffering=1)

    async def handle(creader, cwriter):
        backend = next(rr)
        log.write(f"{time.time():.3f}\t{port}\t{backend}\n")
        try:
            breader, bwriter = await asyncio.open_connection("127.0.0.1", backend)
        except OSError:
            cwriter.close()
            return
        await asyncio.gather(pipe(creader, bwriter), pipe(breader, cwriter))

    server = await asyncio.start_server(handle, "127.0.0.1", port)
    with open(pidfile, "w") as f:
        f.write(str(os.getpid()))
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
