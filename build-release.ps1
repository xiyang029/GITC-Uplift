# Builds both release halves (x64 and x86) by delegating to build.cmd, which picks the matching
# vcvars, fetches deps, then runs cmake --preset / --build for the preset.
$ErrorActionPreference = 'Stop'
foreach ($preset in 'release', 'release-x86') {
  Write-Host "==> $preset"
  & "$PSScriptRoot\build.cmd" $preset
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
