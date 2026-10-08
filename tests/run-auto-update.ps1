param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $testRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found' }
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
& $msbuild (Join-Path $testRoot 'AutoUpdateTests.vcxproj') /nologo /m "/t:$target" /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Automatic-update tests failed to build' }
& (Join-Path $projectRoot 'build\tests\AutoUpdateTests.exe') $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Automatic-update tests failed' }
