# sotto

On-device ambient AI for clinical consultations. A C++20 engine with a WinUI 3 shell that
listens to the consultation, produces a labelled transcript, and drafts a structured clinical note.


## Build

Requires Visual Studio 2022 or 2026 with the Desktop development with C++ workload, CMake
3.25 or later, and the .NET 10 SDK.

```powershell
cmake --preset dev
cmake --build --preset dev
dotnet build sotto.slnx
```

Configures and builds the engine, then the shell. Use the `release` presets for an optimised
engine build.

## Tests

```powershell
cmake --workflow --preset dev
dotnet test sotto.slnx
```

The workflow runs configure, build and the engine tests in one step. `dotnet test` builds and
runs the C# suites; the integration tests launch `sotto_engine.exe`, so build the engine first.

Unit tests cover the view models and the supervision policy with fakes at the ports.
Integration tests exercise the real Win32 adapters (job objects, process launch, kill and
restart) by spawning short-lived stand-in processes, and carry the `Integration` trait:

```powershell
dotnet test sotto.slnx --filter "Category!=Integration"
```

runs the unit tests alone. Both kinds run on every build. There is no UI automation; the
views hold no logic to test.

## Notes

C++ follows the Google C++ Style Guide, with a 4-space indent and a 100 column limit set in
`.clang-format`.

C# follows the standard .NET conventions, with naming and style enforced at build time
through `.editorconfig`.

## License

Distributed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2026 Philip Hargreaves.
