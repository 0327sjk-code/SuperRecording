[CmdletBinding()]
param(
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')]
    [string]$Version,

    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string]$Repository = '0327sjk-code/SuperRecording',

    [string]$CommitMessage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [switch]$AllowFailure,
        [switch]$EchoOutput
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Command @Arguments 2>&1 | ForEach-Object {
            $line = [string]$_
            [void]$lines.Add($line)
            if ($EchoOutput) {
                Write-Host $line
            }
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $result = [pscustomobject]@{
        ExitCode = $exitCode
        Output = $lines.ToArray()
    }
    if (-not $AllowFailure -and $exitCode -ne 0) {
        $details = ($result.Output -join [Environment]::NewLine).Trim()
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = 'No diagnostic output was returned.'
        }
        throw "Command failed with exit code ${exitCode}: $Command $($Arguments -join ' ')`n$details"
    }
    return $result
}

function Get-RequiredSingleLine {
    param(
        [Parameter(Mandatory = $true)]
        [psobject]$Result,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $value = ($Result.Output -join [Environment]::NewLine).Trim()
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "$Description is missing."
    }
    return $value
}

function Get-NormalizedGitHubRepository {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RemoteUrl
    )

    $url = $RemoteUrl.Trim()
    $path = $null
    if ($url -match '^https://github\.com/(?<path>[^?#]+)$') {
        $path = $Matches.path
    }
    elseif ($url -match '^git@github\.com:(?<path>.+)$') {
        $path = $Matches.path
    }
    elseif ($url -match '^ssh://git@github\.com/(?<path>.+)$') {
        $path = $Matches.path
    }
    else {
        return $null
    }

    $path = $path.Trim('/')
    if ($path.EndsWith('.git', [StringComparison]::OrdinalIgnoreCase)) {
        $path = $path.Substring(0, $path.Length - 4)
    }
    if ($path -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
        return $null
    }
    return $path
}

function Assert-ReleaseCompilerPolicy {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectPath
    )

    [xml]$projectXml = [System.IO.File]::ReadAllText($ProjectPath)
    $releaseCompile = $null
    foreach ($definitionGroup in $projectXml.Project.ItemDefinitionGroup) {
        if ([string]$definitionGroup.Condition -match 'Release\|x64') {
            $releaseCompile = $definitionGroup.ClCompile
            break
        }
    }
    if ($null -eq $releaseCompile -or
        [string]$releaseCompile.WarningLevel -ne 'Level4' -or
        [string]$releaseCompile.TreatWarningAsError -ne 'true') {
        throw 'Release|x64 must enable WarningLevel=Level4 and TreatWarningAsError=true.'
    }
}

function Write-Utf8NoBomAtomically {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    $directory = Split-Path -Parent $Path
    if (-not [System.IO.Directory]::Exists($directory)) {
        throw "Output directory does not exist: $directory"
    }
    $transactionId = [Guid]::NewGuid().ToString('N')
    $temporaryPath = "$Path.$transactionId.tmp"
    $backupPath = "$Path.$transactionId.bak"
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    try {
        [System.IO.File]::WriteAllText($temporaryPath, $Text, $utf8)
        if ([System.IO.File]::Exists($Path)) {
            [System.IO.File]::Replace($temporaryPath, $Path, $backupPath, $true)
            if ([System.IO.File]::Exists($backupPath)) {
                [System.IO.File]::Delete($backupPath)
            }
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    finally {
        if ([System.IO.File]::Exists($temporaryPath)) {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
}

function Get-HttpStatusCode {
    param(
        [Parameter(Mandatory = $true)]
        [System.Exception]$Exception
    )

    $responseProperty = $Exception.PSObject.Properties['Response']
    if ($null -eq $responseProperty -or $null -eq $responseProperty.Value) {
        return 0
    }
    $statusProperty = $responseProperty.Value.PSObject.Properties['StatusCode']
    if ($null -eq $statusProperty -or $null -eq $statusProperty.Value) {
        return 0
    }
    return [int]$statusProperty.Value
}

function Invoke-GitHubRest {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Get', 'Post')]
        [string]$Method,

        [Parameter(Mandatory = $true)]
        [string]$Uri,

        [Parameter(Mandatory = $true)]
        [hashtable]$Headers,

        [string]$Body,
        [string]$InFile,
        [string]$ContentType = 'application/json',
        [switch]$AllowNotFound
    )

    try {
        $parameters = @{
            Method = $Method
            Uri = $Uri
            Headers = $Headers
            ErrorAction = 'Stop'
        }
        if (-not [string]::IsNullOrWhiteSpace($Body)) {
            $parameters.Body = $Body
            $parameters.ContentType = $ContentType
        }
        if (-not [string]::IsNullOrWhiteSpace($InFile)) {
            $parameters.InFile = $InFile
            $parameters.ContentType = $ContentType
        }
        return Invoke-RestMethod @parameters
    }
    catch {
        $statusCode = Get-HttpStatusCode -Exception $_.Exception
        if ($AllowNotFound -and $statusCode -eq 404) {
            return $null
        }
        throw "GitHub API request failed with HTTP status ${statusCode}: $Method $Uri"
    }
}

function Resolve-GitHubCli {
    $command = Get-Command gh.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $packageRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (-not [System.IO.Directory]::Exists($packageRoot)) {
        return $null
    }
    $candidate = Get-ChildItem -LiteralPath $packageRoot -Directory |
        Where-Object { $_.Name -like 'GitHub.cli_*' } |
        Sort-Object LastWriteTimeUtc -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\gh.exe' } |
        Where-Object { [System.IO.File]::Exists($_) } |
        Select-Object -First 1
    return $candidate
}

function Assert-OnlyExpectedGitChanges {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$StatusLines,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedPaths
    )

    foreach ($line in $StatusLines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line.Length -lt 4) {
            throw "Unexpected git status output: $line"
        }
        $path = $line.Substring(3).Trim('"')
        if ($path.Contains(' -> ')) {
            $path = ($path -split ' -> ')[-1].Trim('"')
        }
        $path = $path.Replace('\', '/')
        if ($ExpectedPaths -notcontains $path) {
            throw "Unexpected file changed during release preparation: $path"
        }
    }
}

$projectRoot = [System.IO.Path]::GetFullPath(
    (Split-Path -Parent $PSScriptRoot)
)
$productInfoPath = Join-Path $projectRoot 'src\common\ProductInfo.h'
if ([string]::IsNullOrWhiteSpace($Version)) {
    if (-not [System.IO.File]::Exists($productInfoPath)) {
        throw "Product version file was not found: $productInfoPath"
    }
    $productInfoText = [System.IO.File]::ReadAllText($productInfoPath)
    $versionMatch = [regex]::Match(
        $productInfoText,
        'inline constexpr wchar_t Version\[\][ \t]*=[ \t]*L"(?<major>[0-9]+)\.(?<minor>[0-9]+)\.(?<patch>[0-9]+)";'
    )
    if (-not $versionMatch.Success) {
        throw 'Current product version could not be read from ProductInfo.h.'
    }
    $major = [int]$versionMatch.Groups['major'].Value
    $minor = [int]$versionMatch.Groups['minor'].Value
    $patch = [int]$versionMatch.Groups['patch'].Value
    if ($major -gt 65535 -or $minor -gt 65535 -or $patch -ge 65535) {
        throw 'Current product version cannot be automatically patch-incremented.'
    }
    $Version = '{0}.{1}.{2}' -f $major, $minor, ($patch + 1)
    Write-Host "No Version was supplied; publishing the next patch version $Version."
}
$setVersionScript = Join-Path $PSScriptRoot 'set-version.ps1'
$buildScript = Join-Path $projectRoot 'build.ps1'
$projectFile = Join-Path $projectRoot 'SuperRecording.vcxproj'
$releaseExecutable = Join-Path $projectRoot 'build\Release\SuperRecording.exe'
$versionFile = Join-Path $projectRoot 'version.txt'
$tagName = "v$Version"
if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
    $CommitMessage = "Release SuperRecording $tagName"
}
else {
    $CommitMessage = $CommitMessage.Trim()
    if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
        throw 'CommitMessage cannot contain only whitespace.'
    }
}

foreach ($requiredFile in @($setVersionScript, $buildScript, $projectFile)) {
    if (-not [System.IO.File]::Exists($requiredFile)) {
        throw "Required release file was not found: $requiredFile"
    }
}

$gitCommandInfo = Get-Command git.exe -ErrorAction SilentlyContinue
if ($null -eq $gitCommandInfo) {
    throw 'git.exe was not found in PATH.'
}
$git = $gitCommandInfo.Source

$gitRootResult = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'rev-parse', '--show-toplevel')
$gitRoot = [System.IO.Path]::GetFullPath(
    (Get-RequiredSingleLine -Result $gitRootResult -Description 'Git repository root')
).TrimEnd('\')
if (-not $gitRoot.Equals(
        $projectRoot.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Project must be the Git repository root. Expected: $projectRoot Actual: $gitRoot"
}

$identityName = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'config', '--get', 'user.name') `
    -AllowFailure
$identityEmail = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'config', '--get', 'user.email') `
    -AllowFailure
if ($identityName.ExitCode -ne 0 -or
    [string]::IsNullOrWhiteSpace(($identityName.Output -join '').Trim()) -or
    $identityEmail.ExitCode -ne 0 -or
    [string]::IsNullOrWhiteSpace(($identityEmail.Output -join '').Trim())) {
    throw 'Git identity is incomplete. Configure user.name and user.email before publishing.'
}

$branchResult = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'symbolic-ref', '--quiet', '--short', 'HEAD')
$branchName = Get-RequiredSingleLine -Result $branchResult -Description 'Current Git branch'

$statusBefore = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'status', '--porcelain=v1', '--untracked-files=all')
if ($statusBefore.Output.Count -ne 0) {
    throw "Git working tree must be clean before publishing:`n$($statusBefore.Output -join [Environment]::NewLine)"
}

$originResult = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'remote', 'get-url', 'origin')
$originUrl = Get-RequiredSingleLine -Result $originResult -Description 'Git origin remote'
$originRepository = Get-NormalizedGitHubRepository -RemoteUrl $originUrl
if ($null -eq $originRepository -or
    -not $originRepository.Equals($Repository, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The origin remote does not match the requested GitHub Repository parameter.'
}

$previousGitTerminalPrompt = $env:GIT_TERMINAL_PROMPT
$env:GIT_TERMINAL_PROMPT = '0'
try {
    Write-Host 'Checking Git remote access and synchronization...'
    Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'fetch', '--prune', '--tags', 'origin') `
        -EchoOutput | Out-Null

    $headCommit = Get-RequiredSingleLine `
        -Result (Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'rev-parse', 'HEAD')) `
        -Description 'Current Git commit'
    $remoteBranch = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('ls-remote', '--heads', 'origin', "refs/heads/$branchName")
    if ($remoteBranch.Output.Count -gt 0) {
        $remoteCommit = (($remoteBranch.Output[0] -split '[ \t]+')[0]).Trim()
        if (-not $remoteCommit.Equals($headCommit, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Local branch $branchName is not exactly synchronized with origin/$branchName."
        }
    }

    $localTag = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'show-ref', '--verify', '--quiet', "refs/tags/$tagName") `
        -AllowFailure
    if ($localTag.ExitCode -eq 0) {
        throw "Local tag already exists: $tagName"
    }
    if ($localTag.ExitCode -ne 1) {
        throw "Unable to check local tag: $tagName"
    }

    $remoteTag = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('ls-remote', '--exit-code', '--tags', 'origin', "refs/tags/$tagName") `
        -AllowFailure
    if ($remoteTag.ExitCode -eq 0) {
        throw "Remote tag already exists: $tagName"
    }
    if ($remoteTag.ExitCode -ne 2) {
        throw "Unable to check remote tag: $tagName"
    }

    Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'push', '--dry-run', 'origin', "HEAD:refs/heads/$branchName") `
        -EchoOutput | Out-Null

    $gh = Resolve-GitHubCli
    $useGitHubCli = -not [string]::IsNullOrWhiteSpace($gh)
    $githubHeaders = $null
    if ($useGitHubCli) {
        Write-Host 'Using GitHub CLI for release publication.'
        Invoke-NativeCommand `
            -Command $gh `
            -Arguments @('auth', 'status', '--hostname', 'github.com') `
            -EchoOutput | Out-Null
        $repositoryResult = Invoke-NativeCommand `
            -Command $gh `
            -Arguments @('repo', 'view', $Repository, '--json', 'nameWithOwner')
        $repositoryView = ($repositoryResult.Output -join '') | ConvertFrom-Json
        if (-not ([string]$repositoryView.nameWithOwner).Equals(
                $Repository,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'GitHub CLI authenticated repository does not match Repository.'
        }
        $existingRelease = Invoke-NativeCommand `
            -Command $gh `
            -Arguments @('release', 'view', $tagName, '--repo', $Repository, '--json', 'tagName') `
            -AllowFailure
        if ($existingRelease.ExitCode -eq 0) {
            throw "GitHub Release already exists: $tagName"
        }
        $releaseLookupError = ($existingRelease.Output -join ' ')
        if ($releaseLookupError -notmatch '(?i)(release not found|HTTP 404|not found)') {
            throw "Unable to confirm that GitHub Release $tagName is absent: $releaseLookupError"
        }
    }
    else {
        if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
            throw 'gh.exe is unavailable and GITHUB_TOKEN is not set.'
        }
        Write-Host 'GitHub CLI was not found; using GitHub REST API.'
        $githubHeaders = @{
            Authorization = "Bearer $($env:GITHUB_TOKEN)"
            Accept = 'application/vnd.github+json'
            'X-GitHub-Api-Version' = '2022-11-28'
            'User-Agent' = 'SuperRecording-Publisher'
        }
        $repositoryView = Invoke-GitHubRest `
            -Method Get `
            -Uri "https://api.github.com/repos/$Repository" `
            -Headers $githubHeaders
        if (-not ([string]$repositoryView.full_name).Equals(
                $Repository,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'GITHUB_TOKEN cannot access the requested Repository.'
        }
        $existingRelease = Invoke-GitHubRest `
            -Method Get `
            -Uri "https://api.github.com/repos/$Repository/releases/tags/$tagName" `
            -Headers $githubHeaders `
            -AllowNotFound
        if ($null -ne $existingRelease) {
            throw "GitHub Release already exists: $tagName"
        }
    }

    Assert-ReleaseCompilerPolicy -ProjectPath $projectFile

    Write-Host "Updating version to $Version..."
    Invoke-NativeCommand `
        -Command (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
        -Arguments @(
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy', 'Bypass',
            '-File', $setVersionScript,
            '-Version', $Version
        ) `
        -EchoOutput | Out-Null

    Write-Host 'Rebuilding Release|x64 with /W4 /WX...'
    Invoke-NativeCommand `
        -Command (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
        -Arguments @(
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy', 'Bypass',
            '-File', $buildScript,
            '-Configuration', 'Release',
            '-Rebuild'
        ) `
        -EchoOutput | Out-Null

    if (-not [System.IO.File]::Exists($releaseExecutable)) {
        throw "Release executable was not produced: $releaseExecutable"
    }
    $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
        $releaseExecutable
    )
    $actualFileVersion = '{0}.{1}.{2}.{3}' -f `
        $versionInfo.FileMajorPart,
        $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart,
        $versionInfo.FilePrivatePart
    $expectedFileVersion = "$Version.0"
    if ($actualFileVersion -ne $expectedFileVersion) {
        throw "Built FileVersion mismatch. Expected $expectedFileVersion, got $actualFileVersion."
    }

    Write-Utf8NoBomAtomically `
        -Path $versionFile `
        -Text ($Version + "`r`n")
    if (([System.IO.File]::ReadAllText($versionFile)).Trim() -ne $Version) {
        throw 'version.txt verification failed.'
    }

    $expectedReleasePaths = @(
        'src/common/ProductInfo.h',
        'src/app/SuperRecording.rc',
        'src/app/SuperRecording.manifest',
        'version.txt'
    )
    $statusAfter = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'status', '--porcelain=v1', '--untracked-files=all')
    Assert-OnlyExpectedGitChanges `
        -StatusLines $statusAfter.Output `
        -ExpectedPaths $expectedReleasePaths
    if ($statusAfter.Output.Count -eq 0) {
        throw 'Release preparation produced no Git changes; use a new version number.'
    }

    Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'diff', '--check') | Out-Null
    Invoke-NativeCommand `
        -Command $git `
        -Arguments @(
            '-C', $projectRoot, 'add', '--',
            'src/common/ProductInfo.h',
            'src/app/SuperRecording.rc',
            'src/app/SuperRecording.manifest',
            'version.txt'
        ) | Out-Null
    $stagedFiles = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'diff', '--cached', '--name-only')
    if ($stagedFiles.Output.Count -eq 0) {
        throw 'No release files were staged for commit.'
    }
    Assert-OnlyExpectedGitChanges `
        -StatusLines @($stagedFiles.Output | ForEach-Object { "M  $_" }) `
        -ExpectedPaths $expectedReleasePaths

    Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'commit', '-m', $CommitMessage) `
        -EchoOutput | Out-Null
    Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'tag', '-a', $tagName, '-m', $CommitMessage) | Out-Null

    Write-Host "Pushing $branchName and $tagName atomically..."
    Invoke-NativeCommand `
        -Command $git `
        -Arguments @(
            '-C', $projectRoot,
            'push', '--atomic', 'origin',
            "HEAD:refs/heads/$branchName",
            "refs/tags/$tagName:refs/tags/$tagName"
        ) `
        -EchoOutput | Out-Null

    Write-Host 'Creating GitHub Release and uploading fixed assets...'
    if ($useGitHubCli) {
        Invoke-NativeCommand `
            -Command $gh `
            -Arguments @(
                'release', 'create', $tagName,
                $releaseExecutable,
                $versionFile,
                '--repo', $Repository,
                '--title', "SuperRecording $Version",
                '--notes', $CommitMessage,
                '--verify-tag'
            ) `
            -EchoOutput | Out-Null
        $publishedReleaseResult = Invoke-NativeCommand `
            -Command $gh `
            -Arguments @(
                'release', 'view', $tagName,
                '--repo', $Repository,
                '--json', 'url,tagName,assets'
            )
        $publishedRelease = ($publishedReleaseResult.Output -join '') | ConvertFrom-Json
    }
    else {
        $releaseBody = @{
            tag_name = $tagName
            target_commitish = $branchName
            name = "SuperRecording $Version"
            body = $CommitMessage
            draft = $false
            prerelease = $false
        } | ConvertTo-Json -Depth 4 -Compress
        $publishedRelease = Invoke-GitHubRest `
            -Method Post `
            -Uri "https://api.github.com/repos/$Repository/releases" `
            -Headers $githubHeaders `
            -Body $releaseBody
        $uploadBase = ([string]$publishedRelease.upload_url) -replace '\{.*$', ''
        foreach ($asset in @(
            [pscustomobject]@{
                Path = $releaseExecutable
                Name = 'SuperRecording.exe'
                ContentType = 'application/vnd.microsoft.portable-executable'
            },
            [pscustomobject]@{
                Path = $versionFile
                Name = 'version.txt'
                ContentType = 'text/plain'
            }
        )) {
            $escapedAssetName = [Uri]::EscapeDataString($asset.Name)
            Invoke-GitHubRest `
                -Method Post `
                -Uri "${uploadBase}?name=$escapedAssetName" `
                -Headers $githubHeaders `
                -InFile $asset.Path `
                -ContentType $asset.ContentType | Out-Null
        }
        $publishedRelease = Invoke-GitHubRest `
            -Method Get `
            -Uri "https://api.github.com/repos/$Repository/releases/tags/$tagName" `
            -Headers $githubHeaders
    }

    $publishedAssetNames = @($publishedRelease.assets | ForEach-Object { [string]$_.name })
    if ($publishedAssetNames -notcontains 'SuperRecording.exe' -or
        $publishedAssetNames -notcontains 'version.txt') {
        throw 'GitHub Release was created, but one or more required assets are missing.'
    }

    $releaseUrl = if ($useGitHubCli) {
        [string]$publishedRelease.url
    }
    else {
        [string]$publishedRelease.html_url
    }
    Write-Host "Published SuperRecording $Version successfully: $releaseUrl"
}
finally {
    if ($null -eq $previousGitTerminalPrompt) {
        Remove-Item Env:GIT_TERMINAL_PROMPT -ErrorAction SilentlyContinue
    }
    else {
        $env:GIT_TERMINAL_PROMPT = $previousGitTerminalPrompt
    }
}
