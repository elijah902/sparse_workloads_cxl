# Serial SpMV on DRAM and CXL

This is a small C++ benchmark for serial sparse matrix–vector multiplication (`y = A x`). It uses the repository's CSR kernel and `TieredArray` backend to place the matrix arrays in host DRAM, CXL direct, or CXL block memory. `x` and `y` stay in host memory. CXL runs need a reachable `cxlmemsim_server`; DRAM runs do not.

## Build and run

```sh
./scripts/build.sh
./build/serial_spmv --matrix /path/to/cant.mtx --backend dram
./build/serial_spmv --matrix /path/to/cant.mtx --backend cxl-direct --host SERVER --port 9999
./build/serial_spmv --matrix /path/to/cant.mtx --backend cxl-block --host SERVER --port 9999
```

The Matrix Market reader accepts coordinate real/integer/pattern input and expands symmetric matrices. Each output line reports elapsed time, CXL reads/bytes, cache hit rate, and a checksum. Use `--repeat N` for repeated runs. The example `cant` file is at `/home/elijah/School/4990_tests/cxl_mem_mb/matrices/cant/cant.mtx` on the current machine.

## Example observations from the repository's earlier calibration

- On a 4,096 x 4,096 Laplacian, CXL block was about 7% faster than CXL direct in the simulator (1.04 vs 1.14 ms modeled).
- On the earlier 4,096-row RMAT workload, CSR on CXL direct modeled at 3.10 ms versus 0.46 ms on DRAM; the software cache hit about 91% of lookups.
- In those synthetic sweeps, SELL and BCSR did not beat CSR. These are example results from different matrices, not measurements of `cant`.

For a simple new experiment, compare DRAM, CXL direct, and CXL block on the same `cant.mtx`, then repeat each several times and compare median times.

The printed CXL counters describe simulated/server memory traffic. Wall time also includes software and network overhead, so it is not a hardware-only CXL latency measurement.
