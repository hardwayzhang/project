# AGENTS.md

## Cursor Cloud specific instructions

### Repository Structure

The `main` branch is an intentionally empty base (README.md only). All project work lives in feature branches:

| Branch Pattern | Technology | Description |
|---|---|---|
| `cursor/go-resource-management-excel-*` | Go 1.24, Protobuf, Excelize | Resource management with Excel parsing |
| `cursor/proto-message-conversion-logic-*` | C++, CMake, Protobuf | Protobuf message conversion library |
| `cursor/steam-*` | Python 3, requests | Steam review scraping |
| `cursor/test-pulsar-*` | Go, Apache Pulsar | Messaging fault-tolerance testing |
| `cursor/ue4-*` | Unreal Engine 4 | Game engine projects |

### Development Environment

The VM has the following toolchains pre-installed:

- **Go** 1.22.2 (feature branches may require newer; install via `go install golang.org/dl/go1.24.0@latest` if needed)
- **Python** 3.12.3
- **Node.js** 22.22.2
- **Git** 2.43.0
- **GCC/G++** and CMake available for C++ projects

### Working on Feature Branches

Since `main` is empty, there is nothing to lint/test/build/run on it. When working on a feature branch:

1. Check the branch's dependency files (`go.mod`, `requirements.txt`, `CMakeLists.txt`, etc.)
2. Install dependencies appropriate to the branch's technology stack
3. Run tests/builds per that branch's conventions

### No Global Dependencies

There is no global dependency manifest or lockfile. Each feature branch is self-contained with its own dependency management.
