# Ambient Voice Scribe Technology - better name TBC

On-device ambient AI for clinical consultations. A C++20 engine with a WinUI 3 shell that
listens to the consultation, produces a labelled transcript, and drafts a structured clinical note.

Everything runs locally: audio, transcripts and notes never leave the machine.

## Run the release package

Requirements: Windows 11 x64, an Intel Core Ultra Series 2 processor with Intel Arc iGPU,
32 GB RAM and 20 GB free disk. The engine runs its models on the Intel GPU only and will
report, rather than hide, a machine without one. Nothing needs installing - no runtimes,
no admin rights.

1. Extract `ambient.zip` anywhere, for example a folder on the Desktop.
2. Open the `ambient` folder and double-click `Ambient.App.exe`.

If Windows SmartScreen objects to an unsigned download, choose "More info", then
"Run anyway".

The first launch is slow - each model is compiled for the machine's GPU and cached, which
can take a few minutes. While that runs, the first playback or recording may stutter, and
in the worst case the app can crash; close it and open it again - the prepared models are
kept, so it does not repeat. This is a quirk of the zip-style distribution: an installed
build would prepare the models during installation instead. Every later launch starts
quickly.

### Trying it out

The zip bundles recorded doctor-patient consultations, so you can see the full pipeline
without holding a consultation yourself:

1. Open Settings and turn on **Demo tray**. A replay bar appears along the bottom.
2. Pick **Elbow swelling** and press play. Click the speed button a few times to take it
   to 16x. Even at 16x the nine-minute consultation takes a couple of minutes to
   transcribe, so watch the transcript build and let it run to the end. A real
   consultation arrives at normal speed, which the models transcribe with a lot of
   headroom - live use keeps up with the conversation throughout.
3. When the replay finishes, the clinical note is written, then the patient sheet.
4. On the note, change the style (Prose or SOAP) and the detail level, and press
   Regenerate to rewrite it.
5. Open Patient information, pick a language, and press Translate.

Recording a real conversation with the microphone works the same way - press the record
button and speak.

The zip already contains the model weights.

## Build and run with Visual Studio

You need:

- Visual Studio 2022 or 2026 with the **Desktop development with C++** workload
  (this includes CMake and the MSVC toolchain)
- the .NET 10 SDK

Then, from a developer command prompt in the repo root:

1. Fetch the OpenVINO toolchain the engine builds against:

   ```powershell
   powershell tools\get-openvino.ps1
   ```

   This downloads the pinned OpenVINO GenAI archive (about 1 GB), verifies its hash, and
   installs it at `external\openvino` inside the repo. CMake is already pointed there;
   nothing needs configuring. Run it once per clone - it is a no-op when already installed.

2. Build the engine:

   ```powershell
   cmake --preset release
   cmake --build --preset release
   ```

   The app always launches the *release* engine, even from a Debug shell - a debug engine
   is 5-10x slower through the models, which makes every timing observation misleading.
   You only rebuild it when engine code changes.

3. Put the models in place. Either fetch them:

   ```powershell
   cmake --build --preset release --target fetch-models
   ```

   or copy the `models` folder out of a release zip into the repo root. Both give you
   `models/` with one folder per model.

4. Open `ambient.slnx` in Visual Studio. Set `Ambient.App` as the startup project and the
   platform to **x64**, then F5.

The first build creates `models`, `prompts` and `demo` junctions beside the exe, pointing
back into the repo - so a prompt edit applies to the next note without a rebuild, and the
model store is shared rather than copied. The build output lives at
`app\Ambient.App\bin\x64\Debug\net10.0-windows10.0.26100.0\win-x64\`.

If the app starts but reports that the engine or a model is missing, the engine build
(step 1) or the model store (step 2) is what is missing - the app tells you which.

To build everything from the command line instead: `dotnet build ambient.slnx -p:Platform=x64`.

## Models

Weights are not in git; they ship as GitHub Release assets described by the `weights/`
registry (per-file SHA-256, sharded at 1.9 GiB). One command downloads, verifies, and
installs them into `models/` and `demo/`:

```powershell
cmake --build --preset dev --target fetch-models
```

Interrupted downloads resume on re-run; a hash mismatch is fatal, never installed. Release
packages carry the same tool as `get-models.cmd` - double-click it once beside the app.

## Tests

```powershell
cmake --workflow --preset dev
dotnet test ambient.slnx
```

The workflow runs configure, build and the engine tests in one step. `dotnet test` builds and
runs the C# suites; the integration tests launch `ambient_engine.exe`, so build the engine first.

Unit tests cover the view models and the supervision policy with fakes at the ports.
Integration tests exercise the real Win32 adapters (job objects, process launch, kill and
restart) by spawning short-lived stand-in processes, and carry the `Integration` trait:

```powershell
dotnet test ambient.slnx --filter "Category!=Integration"
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
