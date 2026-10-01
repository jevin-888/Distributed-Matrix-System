[CmdletBinding()]
param(
    [switch]$CleanOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$releaseDirectory = Join-Path $projectDirectory 'release'
$version = (Select-String -Path (Join-Path $projectDirectory 'pubspec.yaml') -Pattern '^version:\s*([0-9]+\.[0-9]+\.[0-9]+)\+[0-9]+\s*$').Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($version)) {
    throw 'Could not read the application version from pubspec.yaml'
}
$androidArtifactName = "hsvj-engine-$version-H6_M.apk"
$stagingDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
    'distributed-playback-system-release-' + [System.Guid]::NewGuid().ToString('N')
)

function Invoke-Flutter {
    param([Parameter(Mandatory)][string[]]$Arguments)

    & flutter @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "flutter $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Clear-FlutterBuildArtifacts {
    $temporaryPaths = @(
        (Join-Path $projectDirectory 'build'),
        (Join-Path $projectDirectory '.dart_tool'),
        (Join-Path $projectDirectory 'android\build'),
        (Join-Path $projectDirectory 'windows\flutter\ephemeral')
    )

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        Invoke-Flutter -Arguments @('clean')
        foreach ($temporaryPath in $temporaryPaths) {
            if (Test-Path -LiteralPath $temporaryPath) {
                Remove-Item -LiteralPath $temporaryPath -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        $remainingPaths = @(
            $temporaryPaths | Where-Object { Test-Path -LiteralPath $_ }
        )
        if ($remainingPaths.Count -eq 0) {
            return
        }
        Start-Sleep -Seconds $attempt
    }

    throw "Failed to clean temporary build paths: $($remainingPaths -join ', ')"
}

Push-Location $projectDirectory
try {
    Clear-FlutterBuildArtifacts
    if (-not $CleanOnly) {
        Invoke-Flutter -Arguments @('pub', 'get')
        Invoke-Flutter -Arguments @('analyze')
        Invoke-Flutter -Arguments @('test')
        Invoke-Flutter -Arguments @('build', 'windows', '--release')
        Invoke-Flutter -Arguments @('build', 'apk', '--release')

        $windowsBuildDirectory = Join-Path $projectDirectory 'build\windows\x64\runner\Release'
        $androidBuildFile = Join-Path $projectDirectory 'build\app\outputs\flutter-apk\app-release.apk'
        if (-not (Test-Path -LiteralPath $windowsBuildDirectory -PathType Container)) {
            throw "Windows release output was not found: $windowsBuildDirectory"
        }
        if (-not (Test-Path -LiteralPath $androidBuildFile -PathType Leaf)) {
            throw "Android release output was not found: $androidBuildFile"
        }

        $stagedWindowsDirectory = New-Item -ItemType Directory -Path (
            Join-Path $stagingDirectory 'windows'
        ) -Force
        $stagedAndroidDirectory = New-Item -ItemType Directory -Path (
            Join-Path $stagingDirectory 'android'
        ) -Force
        Get-ChildItem -LiteralPath $windowsBuildDirectory -Force |
            Copy-Item -Destination $stagedWindowsDirectory.FullName -Recurse -Force
        Copy-Item -LiteralPath $androidBuildFile -Destination (
            Join-Path $stagedAndroidDirectory.FullName $androidArtifactName
        ) -Force

        New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
        $windowsReleaseDirectory = Join-Path $releaseDirectory 'windows'
        $androidReleaseDirectory = Join-Path $releaseDirectory 'android'
        $allowedReleaseNames = @('windows', 'android')
        Get-ChildItem -LiteralPath $releaseDirectory -Force |
            Where-Object { $_.Name -notin $allowedReleaseNames } |
            Remove-Item -Recurse -Force
        foreach ($releasePath in @($windowsReleaseDirectory, $androidReleaseDirectory)) {
            if (Test-Path -LiteralPath $releasePath) {
                Remove-Item -LiteralPath $releasePath -Recurse -Force
            }
        }

        Move-Item -LiteralPath $stagedWindowsDirectory.FullName -Destination $windowsReleaseDirectory
        Move-Item -LiteralPath $stagedAndroidDirectory.FullName -Destination $androidReleaseDirectory

        Write-Host "Windows release: $windowsReleaseDirectory"
        Write-Host "Android release: $androidReleaseDirectory"
    }
}
finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
    if (-not $CleanOnly) {
        try {
            Clear-FlutterBuildArtifacts
        }
        finally {
            Pop-Location
        }
    }
    else {
        Pop-Location
    }
}
