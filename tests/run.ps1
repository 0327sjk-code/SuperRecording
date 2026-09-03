param(
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $testRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}

$msbuild = & $vswhere -latest -products '*' `
    -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild was not found. Install Visual Studio 2022 C++ Build Tools.'
}

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$testProject = Join-Path $testRoot 'MediaTimelineTests.vcxproj'
& $msbuild $testProject /nologo /m "/t:$target" `
    /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw "Test build failed. MSBuild exit code: $LASTEXITCODE"
}

$testExecutable = Join-Path $projectRoot 'build\tests\MediaTimelineTests.exe'
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw 'Test build finished, but the executable was not found.'
}
& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Media timeline tests failed. Exit code: $LASTEXITCODE"
}
