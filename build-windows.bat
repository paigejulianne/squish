@echo off
rem Build squish.dll + squish.exe with MSVC (native Windows build).
rem Equivalent to 'make windows-dll'; see also 'make dll' for mingw-w64.
rem
rem Works from a plain command prompt: if cl.exe is not already on PATH
rem (e.g. a VS Developer Command Prompt), locates Visual Studio via
rem vswhere and initializes the amd64 build environment.

setlocal
cd /d "%~dp0"

where cl.exe >nul 2>nul
if %errorlevel%==0 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: cl.exe not on PATH and vswhere.exe not found.
    echo Install Visual Studio with the C++ workload, or run this from a
    echo "Developer Command Prompt for VS".
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo error: no Visual Studio installation with C++ tools found.
    exit /b 1
)

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo
if errorlevel 1 (
    echo error: failed to initialize the Visual Studio environment.
    exit /b 1
)

:build
rem squish.exe links the library statically (squish.c compiled straight in) so
rem it stands alone and runs without squish.dll present.
cl /nologo /O2 /W3 /LD /DSQUISH_BUILD_DLL /Fe:squish.dll squish.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /Fe:squish.exe squish_cli.c squish.c
if errorlevel 1 exit /b 1

echo.
echo Built squish.dll and squish.exe

rem --- Azure Trusted Signing --------------------------------------------------
rem Signs the built binaries with Azure Trusted Signing (a cloud certificate
rem profile; no local .pfx). The signer details are read from signer.json next
rem to this script -- account identifiers only, no secrets -- so signing turns
rem on automatically whenever that file is present. Delete or rename signer.json
rem to build without signing (contributors without an account still get a build).
rem
rem signer.json fields (each overridable by the matching environment variable):
rem   endpoint            TRUSTED_SIGNING_ENDPOINT   region endpoint URI
rem   account             TRUSTED_SIGNING_ACCOUNT    Trusted Signing account name
rem   certificateProfile  TRUSTED_SIGNING_PROFILE    certificate profile name
rem   timestampUrl        TRUSTED_SIGNING_TIMESTAMP  RFC-3161 timestamp URL (optional)
rem
rem Authentication uses the Azure credential chain (DefaultAzureCredential):
rem   - Interactive dev machine:  run `az login` first
rem   - OIDC federated:           set AZURE_CLIENT_ID, AZURE_TENANT_ID and
rem                               AZURE_FEDERATED_TOKEN_FILE
rem   - Service principal:        set AZURE_CLIENT_ID, AZURE_TENANT_ID,
rem                               AZURE_CLIENT_SECRET
rem The signer is the `sign` .NET global tool (https://github.com/dotnet/sign),
rem installed automatically below if missing (needs the .NET SDK on PATH).

rem Load signer.json (if present); values already in the environment win.
set "SIGNER_JSON=%~dp0signer.json"
if exist "%SIGNER_JSON%" (
    for /f "usebackq tokens=1* delims==" %%a in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$j='%SIGNER_JSON%'; $c = ConvertFrom-Json (Get-Content -Raw -LiteralPath $j); $m = [ordered]@{ TRUSTED_SIGNING_ENDPOINT = $c.endpoint; TRUSTED_SIGNING_ACCOUNT = $c.account; TRUSTED_SIGNING_PROFILE = $c.certificateProfile; TRUSTED_SIGNING_TIMESTAMP = $c.timestampUrl }; foreach ($k in $m.Keys) { $ev = [Environment]::GetEnvironmentVariable($k); $val = $(if ($ev) { $ev } else { $m[$k] }); if ($val) { $k + '=' + $val } }"`) do set "%%a=%%b"
)

rem Endpoint, account and profile are all required to sign.
if not defined TRUSTED_SIGNING_ENDPOINT goto :nosign
if not defined TRUSTED_SIGNING_ACCOUNT  goto :nosign
if not defined TRUSTED_SIGNING_PROFILE  goto :nosign

where sign >nul 2>nul
if %errorlevel%==0 goto :dosign

where dotnet >nul 2>nul
if not %errorlevel%==0 (
    echo error: Trusted Signing is configured but neither 'sign' nor 'dotnet'
    echo is on PATH. Install the .NET SDK, then re-run, or run:
    echo     dotnet tool install --global sign
    exit /b 1
)
echo Installing the 'sign' .NET global tool...
dotnet tool install --global sign
if errorlevel 1 exit /b 1
rem Make the freshly installed tool reachable in this session.
set "PATH=%PATH%;%USERPROFILE%\.dotnet\tools"

:dosign
echo.
echo Signing squish.dll and squish.exe with Azure Trusted Signing...
set "TIMESTAMP_ARG="
if defined TRUSTED_SIGNING_TIMESTAMP set "TIMESTAMP_ARG=--timestamp-url "%TRUSTED_SIGNING_TIMESTAMP%""
sign code artifact-signing squish.dll squish.exe ^
    --artifact-signing-endpoint "%TRUSTED_SIGNING_ENDPOINT%" ^
    --artifact-signing-account "%TRUSTED_SIGNING_ACCOUNT%" ^
    --artifact-signing-certificate-profile "%TRUSTED_SIGNING_PROFILE%" ^
    --file-digest sha256 %TIMESTAMP_ARG%
if errorlevel 1 (
    echo error: code signing failed.
    exit /b 1
)
echo Signed squish.dll and squish.exe
goto :eof

:nosign
echo.
echo Note: Azure Trusted Signing not configured ^(no signer.json and TRUSTED_SIGNING_* unset^); binaries are unsigned.
goto :eof
