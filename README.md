# sotto

On-device ambient AI for clinical consultations. A C++20 engine with a WinUI 3 shell that
listens to the consultation, produces a labelled transcript, and drafts a structured clinical note.


## Build

Requires Visual Studio 2022 or 2026 with the Desktop development with C++ workload, CMake
3.25 or later, and the .NET 10 SDK.

```powershell
cmake --workflow --preset dev
```

Configures, builds and runs the engine tests. Use `--preset release` for an optimised build.

```powershell
dotnet test sotto.slnx
```

Builds the C# solution and runs its tests.

## Notes

C++ follows the Google C++ Style Guide, with a 4-space indent and a 100 column limit set in
`.clang-format`.

C# follows the standard .NET conventions, with naming and style enforced at build time
through `.editorconfig`.

## License

Distributed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2026 Philip Hargreaves.
