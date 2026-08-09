# sotto

On-device ambient AI for clinical consultations. A C++20 engine with a WinUI 3 shell that
listens to the consultation, produces a labelled transcipt, and drafts a structured clinical note.

Early stage. Only the engine skeleton and test harness exist so far.

## Build

Requires Visual Studio 2022 or 2026 with the Desktop development with C++ workload, and CMake
3.25 or later.

```powershell
cmake --workflow --preset dev
```

Configures, builds and runs the tests. Use `--preset release` for an optimised build.

## Notes

No model weights are needed to build or run the tests. The suite uses stub fixtures.

C++ follows the Google C++ Style Guide, with a 4-space indent and a 100 column limit set in
`.clang-format`.

## License

Distributed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2026 Philip Hargreaves.
