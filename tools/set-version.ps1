[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')]
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Utf8TextFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not [System.IO.File]::Exists($Path)) {
        throw "Required version file was not found: $Path"
    }

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and
        (($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or
         ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF))) {
        throw "UTF-16 is not supported for version files: $Path"
    }

    $hasUtf8Bom =
        $bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and
        $bytes[2] -eq 0xBF
    $offset = if ($hasUtf8Bom) { 3 } else { 0 }
    $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
    try {
        $text = $strictUtf8.GetString($bytes, $offset, $bytes.Length - $offset)
    }
    catch {
        throw "Version file is not valid UTF-8: $Path"
    }

    return [pscustomobject]@{
        Text = $text
        HasUtf8Bom = $hasUtf8Bom
    }
}

function Replace-ExactlyOnce {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Replacement,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $options = [System.Text.RegularExpressions.RegexOptions]::Multiline -bor
        [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
    $expression = New-Object System.Text.RegularExpressions.Regex(
        $Pattern,
        $options
    )
    $matches = $expression.Matches($Text)
    if ($matches.Count -ne 1) {
        throw "$Description must occur exactly once; found $($matches.Count)."
    }
    return $expression.Replace($Text, $Replacement, 1)
}

function Write-Utf8StagingFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [bool]$WithBom
    )

    $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
    [byte[]]$contentBytes = $utf8.GetBytes($Text)
    $preambleLength = if ($WithBom) { 3 } else { 0 }
    $bytes = New-Object byte[] ($preambleLength + $contentBytes.Length)
    if ($WithBom) {
        $bytes[0] = 0xEF
        $bytes[1] = 0xBB
        $bytes[2] = 0xBF
    }
    if ($contentBytes.Length -gt 0) {
        [System.Array]::Copy(
            $contentBytes,
            0,
            $bytes,
            $preambleLength,
            $contentBytes.Length
        )
    }
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function Assert-VersionComponents {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Components
    )

    foreach ($component in $Components) {
        $numericComponent = 0
        if (-not [int]::TryParse($component, [ref]$numericComponent) -or
            $numericComponent -lt 0 -or
            $numericComponent -gt 65535) {
            throw "Each Windows version component must be between 0 and 65535: $component"
        }
    }
}

$projectRoot = [System.IO.Path]::GetFullPath(
    (Split-Path -Parent $PSScriptRoot)
)
$productInfoPath = Join-Path $projectRoot 'src\common\ProductInfo.h'
$resourcePath = Join-Path $projectRoot 'src\app\SuperRecording.rc'
$manifestPath = Join-Path $projectRoot 'src\app\SuperRecording.manifest'

$versionComponents = $Version.Split('.')
Assert-VersionComponents -Components $versionComponents
$fourPartVersion = "$Version.0"
$commaVersion = "$($versionComponents[0]),$($versionComponents[1]),$($versionComponents[2]),0"

$productInfo = Read-Utf8TextFile -Path $productInfoPath
$resource = Read-Utf8TextFile -Path $resourcePath
$manifest = Read-Utf8TextFile -Path $manifestPath

$updatedProductInfo = Replace-ExactlyOnce `
    -Text $productInfo.Text `
    -Pattern '^(inline constexpr wchar_t Version\[\][ \t]*=[ \t]*L")([0-9]+\.[0-9]+\.[0-9]+)(";[ \t]*)$' `
    -Replacement ('${1}' + $Version + '${3}') `
    -Description 'ProductInfo.h product version'

$updatedResource = Replace-ExactlyOnce `
    -Text $resource.Text `
    -Pattern '^([ \t]*FILEVERSION[ \t]+)([0-9]+,[0-9]+,[0-9]+,[0-9]+)([ \t]*)$' `
    -Replacement ('${1}' + $commaVersion + '${3}') `
    -Description 'SuperRecording.rc FILEVERSION'
$updatedResource = Replace-ExactlyOnce `
    -Text $updatedResource `
    -Pattern '^([ \t]*PRODUCTVERSION[ \t]+)([0-9]+,[0-9]+,[0-9]+,[0-9]+)([ \t]*)$' `
    -Replacement ('${1}' + $commaVersion + '${3}') `
    -Description 'SuperRecording.rc PRODUCTVERSION'
$updatedResource = Replace-ExactlyOnce `
    -Text $updatedResource `
    -Pattern '^([ \t]*VALUE[ \t]+"FileVersion",[ \t]+")([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)(\\0"[ \t]*)$' `
    -Replacement ('${1}' + $fourPartVersion + '${3}') `
    -Description 'SuperRecording.rc FileVersion string'
$updatedResource = Replace-ExactlyOnce `
    -Text $updatedResource `
    -Pattern '^([ \t]*VALUE[ \t]+"ProductVersion",[ \t]+")([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)(\\0"[ \t]*)$' `
    -Replacement ('${1}' + $fourPartVersion + '${3}') `
    -Description 'SuperRecording.rc ProductVersion string'

$updatedManifest = Replace-ExactlyOnce `
    -Text $manifest.Text `
    -Pattern '^([ \t]*<assemblyIdentity[ \t]+version=")([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)("[ \t]+processorArchitecture="amd64"[ \t]+name="SunTools\.SuperRecording"[^\r\n]*?/>[ \t]*)$' `
    -Replacement ('${1}' + $fourPartVersion + '${3}') `
    -Description 'SuperRecording.manifest application identity version'

$updates = @(
    [pscustomobject]@{
        Path = $productInfoPath
        Original = $productInfo.Text
        Updated = $updatedProductInfo
        HasUtf8Bom = [bool]$productInfo.HasUtf8Bom
    },
    [pscustomobject]@{
        Path = $resourcePath
        Original = $resource.Text
        Updated = $updatedResource
        HasUtf8Bom = [bool]$resource.HasUtf8Bom
    },
    [pscustomobject]@{
        Path = $manifestPath
        Original = $manifest.Text
        Updated = $updatedManifest
        HasUtf8Bom = [bool]$manifest.HasUtf8Bom
    }
)

$changedUpdates = @($updates | Where-Object { $_.Original -cne $_.Updated })
if ($changedUpdates.Count -eq 0) {
    Write-Host "Version is already $Version; no files changed."
    exit 0
}

$transactionId = [Guid]::NewGuid().ToString('N')
$staged = New-Object System.Collections.Generic.List[object]
$replaced = New-Object System.Collections.Generic.List[object]
$transactionComplete = $false

try {
    foreach ($update in $changedUpdates) {
        $temporaryPath = "$($update.Path).set-version.$transactionId.tmp"
        $backupPath = "$($update.Path).set-version.$transactionId.bak"
        Write-Utf8StagingFile `
            -Path $temporaryPath `
            -Text $update.Updated `
            -WithBom $update.HasUtf8Bom
        [void]$staged.Add([pscustomobject]@{
            Path = $update.Path
            TemporaryPath = $temporaryPath
            BackupPath = $backupPath
        })
    }

    foreach ($item in $staged) {
        [System.IO.File]::Replace(
            $item.TemporaryPath,
            $item.Path,
            $item.BackupPath,
            $true
        )
        [void]$replaced.Add($item)
    }

    $verifiedProductInfo = (Read-Utf8TextFile -Path $productInfoPath).Text
    $verifiedResource = (Read-Utf8TextFile -Path $resourcePath).Text
    $verifiedManifest = (Read-Utf8TextFile -Path $manifestPath).Text
    if ($verifiedProductInfo -cne $updatedProductInfo -or
        $verifiedResource -cne $updatedResource -or
        $verifiedManifest -cne $updatedManifest) {
        throw 'Post-write version verification failed.'
    }

    $transactionComplete = $true
}
catch {
    $originalFailure = $_.Exception.Message
    $rollbackFailures = New-Object System.Collections.Generic.List[string]
    for ($index = $replaced.Count - 1; $index -ge 0; --$index) {
        $item = $replaced[$index]
        try {
            if ([System.IO.File]::Exists($item.BackupPath)) {
                [System.IO.File]::Replace(
                    $item.BackupPath,
                    $item.Path,
                    $null,
                    $true
                )
            }
        }
        catch {
            [void]$rollbackFailures.Add(
                "$($item.Path): $($_.Exception.Message)"
            )
        }
    }

    if ($rollbackFailures.Count -gt 0) {
        throw "Version update failed: $originalFailure Rollback also failed: $($rollbackFailures -join '; ')"
    }
    throw "Version update failed and was rolled back: $originalFailure"
}
finally {
    foreach ($item in $staged) {
        if ([System.IO.File]::Exists($item.TemporaryPath)) {
            [System.IO.File]::Delete($item.TemporaryPath)
        }
        if ($transactionComplete -and
            [System.IO.File]::Exists($item.BackupPath)) {
            [System.IO.File]::Delete($item.BackupPath)
        }
    }
}

Write-Host "Updated SuperRecording version atomically: $Version ($fourPartVersion)"
