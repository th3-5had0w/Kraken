# Kraken

Quick demo server using io_uring feature

Credits:

https://github.com/axboe/liburing

https://unixism.net/loti/tutorial/webserver_liburing.html

This line was added to let you know that [d4rkn19ht](https://github.com/sinkthemall) did contribute to this repo :D

# Prerequisites

```bash
sudo apt install liburing-dev
```

# Build

```bash
make
```

# Benchmark

```bash
make benchmark
cd playground
python3 benchmarker.py
```
