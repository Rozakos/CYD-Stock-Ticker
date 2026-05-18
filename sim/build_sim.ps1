param(
  [string]$BuildDir = "sim/build",
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$msys = "C:\msys64\msys2_shell.cmd"
if (-not (Test-Path $msys)) {
  throw "MSYS2 not found at $msys. Install MSYS2 and the MinGW64 packages listed in sim/README.md."
}

function Convert-ToMsysPath([string]$Path) {
  $p = $Path -replace "\\", "/"
  if ($p -match "^([A-Za-z]):/(.*)$") {
    return "/" + $Matches[1].ToLowerInvariant() + "/" + $Matches[2]
  }
  return $p
}

$repoMsys = Convert-ToMsysPath $repo.Path
$buildMsys = ($BuildDir -replace "\\", "/")

if ($Clean -and (Test-Path (Join-Path $repo.Path $BuildDir))) {
  Remove-Item -Recurse -Force (Join-Path $repo.Path $BuildDir)
}

& $msys -mingw64 -defterm -no-start -c "cd '$repoMsys' && cmake -S sim -B '$buildMsys' -G Ninja && cmake --build '$buildMsys' --target cyd_sim"
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}
