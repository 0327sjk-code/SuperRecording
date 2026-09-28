param([Parameter(Mandatory=$true)][string]$ArtifactsDirectory, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $testRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild was not found.' }
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
& $msbuild (Join-Path $testRoot 'AnnotationTests.vcxproj') /nologo /m "/t:$target" /p:Configuration=Release /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Annotation test build failed.' }
& (Join-Path $projectRoot 'build\tests\AnnotationTests.exe') ([IO.Path]::GetFullPath($ArtifactsDirectory)) --media-only
if ($LASTEXITCODE -ne 0) { throw "Annotation media tests failed: $LASTEXITCODE" }
& (Join-Path $projectRoot 'build\tests\AnnotationTests.exe') ([IO.Path]::GetFullPath($ArtifactsDirectory)) --ui-only
if ($LASTEXITCODE -ne 0) { throw "Annotation tests failed: $LASTEXITCODE" }
