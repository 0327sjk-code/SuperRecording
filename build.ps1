param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}

$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild was not found. Install Visual Studio 2022 C++ Build Tools.'
}

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$projectFile = Join-Path $projectRoot 'SuperRecording.vcxproj'
& $msbuild $projectFile /nologo /m "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw "Build failed. MSBuild exit code: $LASTEXITCODE"
}

$output = Join-Path $projectRoot "build\$Configuration\SuperRecording.exe"
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
    throw 'Build finished, but the executable was not found.'
}
Write-Host "Build succeeded: $output"
