# CMake support files

The project pins the exact verifier, nlohmann/json, and SQLite sources as
submodules, with RandomX nested under the verifier. The release tooling
dereferences them into a reproducible recursive source archive. The
remaining runtime dependencies are OpenSSL, libcurl, POSIX threads, and the
platform dynamic-loader library.
