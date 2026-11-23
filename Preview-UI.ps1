[CmdletBinding()]
param([switch]$Rebuild)

$ErrorActionPreference = 'Stop'
$previewExe = Join-Path $PSScriptRoot 'build\ui-preview\uplift-ui-preview.exe'

if ($Rebuild -or -not (Test-Path -LiteralPath $previewExe -PathType Leaf)) {
    $running = Get-Process -Name 'uplift-ui-preview' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $previewExe }
    if ($running) {
        throw 'Close the GITC Uplift UI Preview window before rebuilding.'
    }

    # The preview build launches the window on success.
    & (Join-Path $PSScriptRoot 'build.cmd') preview
    if ($LASTEXITCODE -ne 0) {
        throw "Preview build failed with exit code $LASTEXITCODE."
    }
    return
}

Start-Process -FilePath $previewExe -WorkingDirectory (Split-Path -Parent $previewExe)
