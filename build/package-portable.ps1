#requires -Version 5.1
<#
Build and publish the Notepad4-TxtSyntax x64 portable package.

The package contains only default INI templates, never active user INI files.
Publishing overlays the new files onto a copy of the existing portable folder,
then swaps the folders, so existing user settings and extra files are preserved.
#>
[CmdletBinding()]
param(
	[ValidateSet('Release')]
	[string]$Configuration = 'Release',

	[ValidateSet('x64')]
	[string]$Platform = 'x64',

	[string]$Destination,

	[string]$MSBuildPath,

	[switch]$SkipBuild,

	[switch]$KeepStage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($Destination)) {
	$Destination = Join-Path (Split-Path -Parent $repo) 'Notepad4-TxtSyntax-Portable'
}
$Destination = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\')
if ($Destination -eq [System.IO.Path]::GetPathRoot($Destination).TrimEnd('\')) {
	throw "Refusing to use a drive root as the portable destination: $Destination"
}
$zipPath = "$Destination.zip"

$binDir = Join-Path $repo "build\bin\$Configuration\$Platform"
$localeBinDir = Join-Path $binDir 'locale\zh-Hans'
$mainSolution = Join-Path $repo 'build\VisualStudio\Notepad4.sln'
$localeSolution = Join-Path $repo 'locale\Locale.sln'

function Find-MSBuild {
	if (-not [string]::IsNullOrWhiteSpace($MSBuildPath)) {
		if (-not (Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
			throw "MSBuild not found: $MSBuildPath"
		}
		return (Resolve-Path -LiteralPath $MSBuildPath).Path
	}

	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
		$found = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null |
			Select-Object -First 1
		if ($found -and (Test-Path -LiteralPath $found -PathType Leaf)) {
			return (Resolve-Path -LiteralPath $found).Path
		}
	}

	if ($env:VSINSTALLDIR) {
		$candidate = Join-Path $env:VSINSTALLDIR 'MSBuild\Current\Bin\MSBuild.exe'
		if (Test-Path -LiteralPath $candidate -PathType Leaf) {
			return (Resolve-Path -LiteralPath $candidate).Path
		}
	}

	throw 'MSBuild was not found. Pass -MSBuildPath or install the Visual Studio C++ build tools.'
}

function Invoke-MSBuildTarget([string]$ProjectOrSolution, [string]$Target) {
	Write-Host "Building $Target ($Configuration|$Platform)..."
	& $script:msbuildExe $ProjectOrSolution /nologo "/t:$Target" `
		"/p:Configuration=$Configuration" "/p:Platform=$Platform" `
		/m /nodeReuse:false /v:minimal
	if ($LASTEXITCODE -ne 0) {
		throw "Build failed: $ProjectOrSolution /t:$Target (exit code $LASTEXITCODE)"
	}
}

function Copy-PackageFile([string]$Source, [string]$Stage, [string]$RelativePath) {
	if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
		throw "Required build input is missing: $Source"
	}
	$target = Join-Path $Stage $RelativePath
	$parent = Split-Path -Parent $target
	New-Item -ItemType Directory -Path $parent -Force | Out-Null
	Copy-Item -LiteralPath $Source -Destination $target -Force
}

function Get-PEMachine([string]$Path) {
	$stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
	try {
		$reader = [System.IO.BinaryReader]::new($stream)
		if ($reader.ReadUInt16() -ne 0x5A4D) {
			throw "Not a PE file: $Path"
		}
		$stream.Position = 0x3C
		$peOffset = $reader.ReadInt32()
		$stream.Position = $peOffset
		if ($reader.ReadUInt32() -ne 0x00004550) {
			throw "Invalid PE signature: $Path"
		}
		return $reader.ReadUInt16()
	} finally {
		$stream.Dispose()
	}
}

function Read-Utf16Ini([string]$Path) {
	$bytes = [System.IO.File]::ReadAllBytes($Path)
	if ($bytes.Length -lt 2 -or $bytes[0] -ne 0xFF -or $bytes[1] -ne 0xFE) {
		throw "INI template is not UTF-16LE with BOM: $Path"
	}
	$text = [System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
	if ($text.Contains([char]0xFFFD) -or $text.Contains([char]0)) {
		throw "INI template contains invalid text: $Path"
	}
	return $text
}

function Assert-IniSections([string]$Path, [string[]]$Sections) {
	$text = Read-Utf16Ini $Path
	foreach ($section in $Sections) {
		if (-not $text.Contains($section)) {
			throw "Missing section $section in $Path"
		}
	}
	return $text
}

function Assert-NoActiveUserIni([string]$Stage) {
	$active = @(Get-ChildItem -LiteralPath $Stage -File -Recurse |
		Where-Object { $_.Name -match '\.ini$' })
	if ($active.Count -ne 0) {
		throw "Active INI files must not be packaged: $($active.Name -join ', ')"
	}
}

function New-DeterministicZip([string]$SourceDir, [string]$OutPath) {
	if (Test-Path -LiteralPath $OutPath) {
		throw "Temporary ZIP already exists: $OutPath"
	}

	$root = (Resolve-Path -LiteralPath $SourceDir).Path.TrimEnd('\')
	$files = @(Get-ChildItem -LiteralPath $root -File -Recurse |
		Sort-Object -Property FullName)
	$stream = [System.IO.File]::Open($OutPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
	try {
		$archive = [System.IO.Compression.ZipArchive]::new($stream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
		try {
			foreach ($file in $files) {
				$relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
				$entry = $archive.CreateEntry($relative, [System.IO.Compression.CompressionLevel]::Optimal)
				$entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
				$input = [System.IO.File]::OpenRead($file.FullName)
				try {
					$output = $entry.Open()
					try {
						$input.CopyTo($output)
					} finally {
						$output.Dispose()
					}
				} finally {
					$input.Dispose()
				}
			}
		} finally {
			$archive.Dispose()
		}
	} finally {
		$stream.Dispose()
	}
}

function Assert-NoRunningPortableProcess([string]$TargetDir) {
	$matches = @()
	foreach ($name in @('Notepad4', 'matepath')) {
		foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
			try {
				if ($process.Path -and $process.Path.StartsWith($TargetDir, [System.StringComparison]::OrdinalIgnoreCase)) {
					$matches += $process.Path
				}
			} catch {
				# Ignore processes whose executable path cannot be queried.
			}
		}
	}
	if ($matches.Count -ne 0) {
		throw "Close the portable build before publishing: $($matches -join ', ')"
	}
}

function Publish-Package([string]$Stage, [string]$ZipTemp, [string]$Prefix) {
	$parent = Split-Path -Parent $Destination
	if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
		New-Item -ItemType Directory -Path $parent -Force | Out-Null
	}

	$newTarget = "$Destination.new-$Prefix"
	$backupDir = "$Destination-Backup-$Prefix"
	if (Test-Path -LiteralPath $newTarget) { throw "Temporary target already exists: $newTarget" }
	if (Test-Path -LiteralPath $backupDir) { throw "Backup path already exists: $backupDir" }

	Assert-NoRunningPortableProcess $Destination

	New-Item -ItemType Directory -Path $newTarget -Force | Out-Null
	if (Test-Path -LiteralPath $Destination -PathType Container) {
		Get-ChildItem -LiteralPath $Destination -Force | ForEach-Object {
			Copy-Item -LiteralPath $_.FullName -Destination $newTarget -Recurse -Force
		}
	}

	Get-ChildItem -LiteralPath $Stage -Force | ForEach-Object {
		Copy-Item -LiteralPath $_.FullName -Destination $newTarget -Recurse -Force
	}

	foreach ($file in @(Get-ChildItem -LiteralPath $Stage -File -Recurse)) {
		$relative = $file.FullName.Substring($Stage.Length + 1)
		$published = Join-Path $newTarget $relative
		if (-not (Test-Path -LiteralPath $published -PathType Leaf)) {
			throw "Publish verification failed, missing: $relative"
		}
		$sourceHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
		$publishedHash = (Get-FileHash -LiteralPath $published -Algorithm SHA256).Hash
		if ($sourceHash -ne $publishedHash) {
			throw "Publish verification failed, hash mismatch: $relative"
		}
	}

	$backupCreated = $false
	$targetMoved = $false
	try {
		if (Test-Path -LiteralPath $Destination -PathType Container) {
			Move-Item -LiteralPath $Destination -Destination $backupDir
			$targetMoved = $true
		}

		Move-Item -LiteralPath $newTarget -Destination $Destination
		$backupCreated = $targetMoved
	} catch {
		if ($targetMoved -and -not (Test-Path -LiteralPath $Destination) -and (Test-Path -LiteralPath $backupDir)) {
			Move-Item -LiteralPath $backupDir -Destination $Destination
		}
		throw
	}

	if ($backupCreated -and (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
		$oldZip = Join-Path $backupDir 'Notepad4-TxtSyntax-Portable.zip'
		Copy-Item -LiteralPath $zipPath -Destination $oldZip -Force
	}
	Move-Item -LiteralPath $ZipTemp -Destination $zipPath -Force

	return [pscustomobject]@{
		Destination = $Destination
		ZipPath = $zipPath
		BackupDir = if ($backupCreated) { $backupDir } else { $null }
	}
}

$msbuildExe = Find-MSBuild
Write-Host "Repository: $repo"
Write-Host "MSBuild:    $msbuildExe"
Write-Host "Destination: $Destination"

if (-not $SkipBuild) {
	Invoke-MSBuildTarget $mainSolution 'Notepad4'
	Invoke-MSBuildTarget $mainSolution 'matepath'
	Invoke-MSBuildTarget $localeSolution 'Notepad4_zh-Hans_'
	Invoke-MSBuildTarget $localeSolution 'matepath_zh-Hans_'
} else {
	Write-Host 'Skipping build; using existing artifacts.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$suffix = [guid]::NewGuid().ToString('N').Substring(0, 8)
$stageRoot = Join-Path $repo "build\temp-portable-$stamp-$suffix"
$stage = Join-Path $stageRoot 'Portable'
$zipTemp = Join-Path $repo "build\temp-portable-$stamp-$suffix.zip"
New-Item -ItemType Directory -Path $stage -Force | Out-Null

try {
	Copy-PackageFile (Join-Path $binDir 'Notepad4.exe') $stage 'Notepad4.exe'
	Copy-PackageFile (Join-Path $binDir 'matepath.exe') $stage 'matepath.exe'
	Copy-PackageFile (Join-Path $repo 'License.txt') $stage 'License.txt'
	Copy-PackageFile (Join-Path $repo 'licenses\TXT-Syntax-MIT.txt') $stage 'licenses\TXT-Syntax-MIT.txt'
	Copy-PackageFile (Join-Path $repo 'licenses\matepath-License.txt') $stage 'licenses\matepath-License.txt'
	Copy-PackageFile (Join-Path $localeBinDir 'Notepad4.dll') $stage 'locale\zh-Hans\Notepad4.dll'
	Copy-PackageFile (Join-Path $localeBinDir 'matepath.dll') $stage 'locale\zh-Hans\matepath.dll'
	Copy-PackageFile (Join-Path $repo 'res\README-Portable.txt') $stage 'README-Portable.txt'

	$defaults = [ordered]@{
		'Notepad4.ini-default' = Join-Path $repo 'res\defaults\Notepad4.ini-default'
		'Notepad4 DarkTheme.ini-default' = Join-Path $repo 'res\defaults\Notepad4 DarkTheme.ini-default'
		'matepath.ini-default' = Join-Path $repo 'res\defaults\matepath.ini-default'
	}
	foreach ($entry in $defaults.GetEnumerator()) {
		Copy-PackageFile $entry.Value $stage $entry.Key
	}

	$themeNames = @(
		'Notepad4 Solarized Light',
		'Notepad4 Tango Warm Dark',
		'Notepad4 Gruvbox Dark',
		'Notepad4 Gruvbox Light'
	)
	foreach ($themeName in $themeNames) {
		Copy-PackageFile (Join-Path $repo "res\themes\$themeName.ini") $stage "$themeName.ini-default"
	}

	$requiredFiles = @(
		'Notepad4.exe',
		'matepath.exe',
		'License.txt',
		'README-Portable.txt',
		'licenses\TXT-Syntax-MIT.txt',
		'licenses\matepath-License.txt',
		'locale\zh-Hans\Notepad4.dll',
		'locale\zh-Hans\matepath.dll',
		'Notepad4.ini-default',
		'Notepad4 DarkTheme.ini-default',
		'matepath.ini-default',
		'Notepad4 Solarized Light.ini-default',
		'Notepad4 Tango Warm Dark.ini-default',
		'Notepad4 Gruvbox Dark.ini-default',
		'Notepad4 Gruvbox Light.ini-default'
	)
	foreach ($relative in $requiredFiles) {
		$path = Join-Path $stage $relative
		if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -le 0) {
			throw "Package validation failed, missing or empty: $relative"
		}
	}

	# The fresh ZIP must contain exactly the documented release files. This
	# catches tree.txt, test data, scripts, user INIs and backup archives.
	$allowedFiles = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	foreach ($relative in $requiredFiles) {
		$null = $allowedFiles.Add($relative)
	}
	$actualFiles = @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
		$_.FullName.Substring($stage.Length + 1)
	})
	$unexpectedFiles = @($actualFiles | Where-Object { -not $allowedFiles.Contains($_) })
	if ($unexpectedFiles.Count -ne 0 -or $actualFiles.Count -ne $requiredFiles.Count) {
		throw "Package validation failed, unexpected files: $($unexpectedFiles -join ', ')"
	}
	if (@($actualFiles | Where-Object { $_ -match '(^|\\)tree\.txt$' }).Count -ne 0) {
		throw 'Package validation failed, tree.txt must not be packaged.'
	}
	if (@($actualFiles | Where-Object { $_ -match '(^|\\)(Backup|backup)[^\\]*$|\.(bak|tmp|log|zip|7z|rar)$' }).Count -ne 0) {
		throw 'Package validation failed, backup or temporary files are present.'
	}
	$topLevelDirs = @(Get-ChildItem -LiteralPath $stage -Directory | ForEach-Object { $_.Name })
	if ($topLevelDirs.Count -ne 2 -or -not ($topLevelDirs -contains 'licenses') -or -not ($topLevelDirs -contains 'locale')) {
		throw "Package validation failed, unexpected top-level directories: $($topLevelDirs -join ', ')"
	}
	$licenseFiles = @(Get-ChildItem -LiteralPath (Join-Path $stage 'licenses') -File)
	if ($licenseFiles.Count -ne 2) {
		throw 'Package validation failed, licenses directory is incomplete.'
	}
	$localeFiles = @(Get-ChildItem -LiteralPath (Join-Path $stage 'locale\zh-Hans') -File)
	if ($localeFiles.Count -ne 2) {
		throw 'Package validation failed, locale/zh-Hans directory is incomplete.'
	}
	$rootFiles = @(Get-ChildItem -LiteralPath $stage -File)
	if ($rootFiles.Count -ne 11) {
		throw "Package validation failed, unexpected root file count: $($rootFiles.Count)"
	}

	Assert-NoActiveUserIni $stage
	$templateCount = @(Get-ChildItem -LiteralPath $stage -File -Recurse |
		Where-Object { $_.Name -match '\.ini-default$' }).Count
	if ($templateCount -ne 7) {
		throw "Package validation failed, expected 7 INI templates, found $templateCount"
	}

	$exeFiles = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object { $_.Extension -ieq '.exe' })
	if ($exeFiles.Count -ne 2 -or -not ($exeFiles.Name -contains 'Notepad4.exe') -or -not ($exeFiles.Name -contains 'matepath.exe')) {
		throw "Package validation failed, unexpected executable set: $($exeFiles.Name -join ', ')"
	}
	foreach ($executable in @($exeFiles + @(Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object { $_.Extension -ieq '.dll' }))) {
		$machine = Get-PEMachine $executable.FullName
		if ($machine -ne 0x8664) {
			throw "Package validation failed, not an x64 PE file: $($executable.Name) (0x$($machine.ToString('X4')))"
		}
	}

	$forbidden = @(Get-ChildItem -LiteralPath $stage -File -Recurse |
		Where-Object { $_.Name -match '\.(pdb|obj|lib|exp|ilk|cpp|cxx|hpp|h|bat|cmd|ps1|py|zip|7z|rar)$' })
	if ($forbidden.Count -ne 0) {
		throw "Package validation failed, forbidden files: $($forbidden.Name -join ', ')"
	}
	if (@(Get-ChildItem -LiteralPath $stage -Directory -Recurse |
		Where-Object { $_.Name -in @('.git', 'scripts') }).Count -ne 0) {
		throw 'Package validation failed, development directories are present.'
	}

	$mainIni = Assert-IniSections (Join-Path $stage 'Notepad4.ini-default') @('[Notepad4]', '[Settings2]', '[Styles]', '[TXT Syntax]')
	if (-not $mainIni.Contains('UILanguage=2052')) {
		throw 'Main default INI does not contain the expected UI language.'
	}
	$darkIni = Assert-IniSections (Join-Path $stage 'Notepad4 DarkTheme.ini-default') @('[Global Styles]', '[TXT Syntax]')
	if (-not $darkIni.Contains('Caret=')) {
		throw 'Dark default INI does not define the caret color.'
	}
	$mateIni = Assert-IniSections (Join-Path $stage 'matepath.ini-default') @('[Settings2]', '[Filters]')
	if (-not $mateIni.Contains('UILanguage=2052')) {
		throw 'matepath default INI does not contain the expected UI language.'
	}
	foreach ($themeName in $themeNames) {
		$themeIni = Assert-IniSections (Join-Path $stage "$themeName.ini-default") @('[Global Styles]', '[TXT Syntax]')
		if (-not $themeIni.Contains('Caret=')) {
			throw "Theme $themeName does not define the caret color."
		}
	}

	New-DeterministicZip $stage $zipTemp
	Write-Host 'Package contents:'
	Get-ChildItem -LiteralPath $stage -File -Recurse |
		Sort-Object -Property FullName |
		ForEach-Object {
			$relative = $_.FullName.Substring($stage.Length + 1)
			$hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
			Write-Host ("  {0,-52} {1,10}  {2}" -f $relative, $_.Length, $hash)
		}

	$result = Publish-Package $stage $zipTemp "$stamp-$suffix"
	$zipHash = (Get-FileHash -LiteralPath $result.ZipPath -Algorithm SHA256).Hash
	Write-Host ''
	Write-Host "Portable package published: $($result.Destination)"
	Write-Host "ZIP: $($result.ZipPath)"
	Write-Host "ZIP SHA-256: $zipHash"
	if ($result.BackupDir) {
		Write-Host "Previous portable folder preserved: $($result.BackupDir)"
	}
} finally {
	if (Test-Path -LiteralPath $zipTemp) {
		Remove-Item -LiteralPath $zipTemp -Force
	}
	if (-not $KeepStage -and (Test-Path -LiteralPath $stageRoot)) {
		Remove-Item -LiteralPath $stageRoot -Recurse -Force
	}
}
