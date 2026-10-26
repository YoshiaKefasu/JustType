# Rebuilds JustType64.msi for the JustType rename (BUILD 6177).
#
# Mirrors the environment bazel/bazel_wrapper/bazel.bat sets up, against the
# Bazel 9.0.2 binary .bazeliskrc pins (bazelisk is not on PATH here).
# Invoked under psmux because a full installer build outruns any single tool
# call.
$ErrorActionPreference = 'Continue'
$src = 'D:\OSS_ProgramFiles\JustType\src'
$bazel = 'C:\Users\yosia\AppData\Local\bazelisk\downloads\sha256\e45277210da3fc1999e474edce5d9e73b37a6fdc0b78418880d9adad12276c4e\bin\bazel.exe'
$log = 'D:\OSS_ProgramFiles\JustType\msi_build.log'

Set-Location $src

if (Test-Path "$src\third_party\llvm\bin\clang-cl.exe") {
  $env:BAZEL_LLVM = "$src\third_party\llvm"
}
if (Test-Path "$src\third_party\msys64\usr\bin\bash.exe") {
  $env:BAZEL_SH = "$src\third_party\msys64\usr\bin\bash.exe"
}
# The host name is non-ASCII, so java.logging writes a log file whose name it
# then reads back as Latin-1 and fails on.  --batch keeps it a warning.
$env:BAZEL_VC = (& python "$src\build_tools\vs_util.py" 2>&1 | Select-Object -Last 1).Trim()

"=== bazel: $bazel ===" | Tee-Object -FilePath $log
"=== BAZEL_VC: $env:BAZEL_VC ===" | Tee-Object -FilePath $log -Append
"=== started: $(Get-Date -Format o) ===" | Tee-Object -FilePath $log -Append

& $bazel --batch build //win32/installer 2>&1 |
    Tee-Object -FilePath $log -Append

"=== bazel exit code: $LASTEXITCODE ===" | Tee-Object -FilePath $log -Append

$msi = "$src\bazel-bin\win32\installer\JustType64.msi"
if (Test-Path $msi) {
    $dest = 'C:\Temp\JustType64.msi'
    if (Test-Path $dest) {
        Set-ItemProperty -Path $dest -Name IsReadOnly -Value $false
        Remove-Item $dest -Force
    }
    New-Item -ItemType Directory -Path 'C:\Temp' -Force | Out-Null
    Copy-Item $msi $dest -Force
    Set-ItemProperty -Path $dest -Name IsReadOnly -Value $false
    "=== copied to $dest ===" | Tee-Object -FilePath $log -Append
    (Get-Item $dest) | Select-Object FullName, Length, LastWriteTime | Format-List | Out-String |
        Tee-Object -FilePath $log -Append
} else {
    "=== MSI NOT FOUND at $msi ===" | Tee-Object -FilePath $log -Append
}

"=== finished: $(Get-Date -Format o) ===" | Tee-Object -FilePath $log -Append
