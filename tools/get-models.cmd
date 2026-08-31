@echo off
rem One-time model download for a ambient release package. Safe to re-run;
rem an interrupted download resumes.
"%~dp0fetch\Ambient.Fetch.exe" fetch "%~dp0weights" "%~dp0"
if errorlevel 1 (
  echo.
  echo Something failed above - run this again to resume.
)
pause
