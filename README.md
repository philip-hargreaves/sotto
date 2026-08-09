# sotto

On-device ambient scribe for clinical consultations. A C++20 engine with a WinUI 3 shell that
transcribes the consultation, works out who said what, and drafts a structured note. Audio and
text stay on the machine.

Early stage. Only the engine skeleton and test harness exist so far.

## Build

Requires Visual Studio 2022 or 2026 with the Desktop development with C++ workload, and CMake
3.25 or later.

```powershell
cmake --workflow --preset dev
```

That configures, builds and runs the tests. Use `--preset release` for an optimised build.
Visual Studio can also open the folder directly.

## Notes

No model weights are needed to build or run the tests. The suite uses stub fixtures.

C++ follows the Google C++ Style Guide, with a 4-space indent and a 100 column limit set in
`.clang-format`. CI rejects unformatted code.

## License

Distributed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2026 Philip Hargreaves.

GoogleTest is fetched at build time and is not redistributed with this project. Model weights
are covered by their own upstream licences and are not held in this repository.
