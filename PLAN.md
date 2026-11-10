# CXL Memory-Centric Algorithms

Original idea: implement a CXL system using SimCXL, gem5, Ramulator, or a similar
memory simulator to study memory-centric algorithm design.

The project now uses **CXLMemSim** and implements hand-written BFS, SpMV, and
(stretch) k-NN, with a 2D stencil as a calibration workload.

- **Full plan:** [docs/project-plan.md](docs/project-plan.md)
- **Platform decision:** [docs/adr/0001-cxl-platform-and-backend.md](docs/adr/0001-cxl-platform-and-backend.md)
- **Glossary:** [GLOSSARY.md](GLOSSARY.md)

## Quick start

```bash
# Build the CXLMemSim server (pinned commit)
./scripts/build_cxlmemsim.sh

# Start the server
./scripts/run_server.sh

# Build this project and run the client smoke test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/cxl_client_smoke
```

See [README.md](README.md) for details.
