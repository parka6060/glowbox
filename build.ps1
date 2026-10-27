param(
    [Parameter(Mandatory=$true)][string]$ObsSource,
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio",
    [string]$Output = (Join-Path $PSScriptRoot 'build'),
    [switch]$Installer
)
$ErrorActionPreference = 'Stop'
$ObsSource = (Resolve-Path -LiteralPath $ObsSource).Path
$ObsRoot = (Resolve-Path -LiteralPath $ObsRoot).Path
$Output = [IO.Path]::GetFullPath($Output)
foreach ($required in @("$ObsSource/libobs/obs.h", "$ObsRoot/bin/64bit/obs.dll", "$ObsRoot/bin/64bit/obs64.exe")) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required file missing: $required" }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++. This is only needed to build from source.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'No C++ build tools found. Install the Desktop development with C++ workload.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$environmentScript = Join-Path $Output 'compiler-environment.cmd'
@('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b 1', 'set') | Set-Content -LiteralPath $environmentScript -Encoding ASCII
$compilerEnvironment = & $env:ComSpec /d /c "`"$environmentScript`""
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the C++ compiler.' }
foreach ($line in $compilerEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}
$exports = & dumpbin /nologo /exports "$ObsRoot/bin/64bit/obs.dll"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read OBS exports.' }
$symbols = @($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)') { $matches[1] } })
if ($symbols.Count -lt 100) { throw 'Unexpected OBS DLL export table.' }
@('LIBRARY obs.dll', 'EXPORTS') + $symbols | Set-Content -LiteralPath "$Output/obs.def" -Encoding ASCII
if (Test-Path -LiteralPath "$Output/obs.lib") { Remove-Item -LiteralPath "$Output/obs.lib" }
Invoke-Checked 'lib' @('/nologo', "/def:$Output/obs.def", '/machine:x64', "/out:$Output/obs.lib")
$includes = @("/I$PSScriptRoot/src", "/I$PSScriptRoot/build-support", "/I$ObsSource/libobs")
$common = @('/nologo', '/std:c++17', '/EHsc', '/O2', '/MT', "/I$Output") + $includes
$nativeShaderRoot = Join-Path $PSScriptRoot 'shaders/native'
$nativeEffect = [IO.File]::ReadAllText((Join-Path $nativeShaderRoot 'renderer.effect'))
$localInclude = '(?m)^[ \t]*#include[ \t]+"([A-Za-z0-9_-]+\.effect)"[ \t]*\r?$'
$nativeEffect = [regex]::Replace($nativeEffect, $localInclude, [Text.RegularExpressions.MatchEvaluator]{
    param($include)
    $stage = [IO.File]::ReadAllText((Join-Path $nativeShaderRoot $include.Groups[1].Value))
    if ($stage -match '(?m)^[ \t]*#include\b') { throw 'Shader stages must not contain nested includes.' }
    return $stage
})
if ($nativeEffect -match '(?m)^[ \t]*#include\b') { throw 'Shader includes must name a local .effect stage.' }
$header = [Collections.Generic.List[string]]::new()
$header.Add('#pragma once')
$header.Add('static const char *const kRendererShaderParts[] = {')
for ($offset = 0; $offset -lt $nativeEffect.Length; $offset += 8000) {
    $part = $nativeEffect.Substring($offset, [Math]::Min(8000, $nativeEffect.Length - $offset))
    if ($part.Contains(')GBFX"')) { throw 'Preview shader contains the raw-string delimiter.' }
    $header.Add('R"GBFX(' + $part + ')GBFX",')
}
$header.Add('};')
[IO.File]::WriteAllText((Join-Path $Output 'renderer_shader.hpp'), ($header -join "`n"), [Text.UTF8Encoding]::new($false))
$pluginDir = Join-Path $Output 'plugin/obs-plugins/64bit'
$dataDir = Join-Path $Output 'plugin/data/obs-plugins/glowbox'
New-Item -ItemType Directory -Force -Path $pluginDir, $dataDir | Out-Null
$objects = @()
$sourceRoot = Join-Path $PSScriptRoot 'src'
foreach ($source in (Get-ChildItem -LiteralPath $sourceRoot -Filter '*.cpp' -Recurse | Sort-Object Name, FullName)) {
    $relative = $source.FullName.Substring($sourceRoot.Length + 1)
    $object = Join-Path (Join-Path $Output 'objects') ([IO.Path]::ChangeExtension($relative, '.obj'))
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $object) | Out-Null
    Invoke-Checked 'cl' ($common + @('/c', '/W4', $source.FullName, "/Fo:$object"))
    $objects += $object
}
if (!$objects.Count) { throw 'No C++ implementation files found in src.' }
Invoke-Checked 'link' (@('/nologo', '/DLL', "/OUT:$pluginDir/glowbox.dll") + $objects + @("$Output/obs.lib"))
$shaderRoot = [IO.Path]::GetFullPath("$PSScriptRoot/shaders")
$shaderManifest = "manifest.txt"
$shaderStages = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$shaderParts = foreach ($line in (Get-Content -LiteralPath (Join-Path $shaderRoot $shaderManifest))) {
    $part = $line.Trim()
    if (!$part -or $part.StartsWith('#')) { continue }
    if ($part -notmatch '^(?:[A-Za-z0-9_-]+/)*[A-Za-z0-9_-]+\.effect$') { throw "Expected a relative .effect stage path: $part" }
    if (!$shaderStages.Add($part)) { throw 'Shader manifest must contain unique stages.' }
    $partPath = [IO.Path]::GetFullPath((Join-Path $shaderRoot $part))
    if (!$partPath.StartsWith($shaderRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Shader path must stay within shaders/: $part"
    }
    [IO.File]::ReadAllText($partPath)
}
if (!$shaderStages.Count) { throw 'Shader manifest must contain unique stages.' }
[IO.File]::WriteAllText("$dataDir/compositor.effect", ($shaderParts -join "`n"), [Text.UTF8Encoding]::new($false))
foreach ($image in @('loading.png','loading-failed.png','spinner.png')) {
    Copy-Item -LiteralPath "$PSScriptRoot/assets/$image" -Destination "$dataDir/$image"
}

if ($Installer) {
    $csc = "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
    if (!(Test-Path -LiteralPath $csc)) { throw 'The .NET Framework C# compiler is missing.' }
    $installerSource = Join-Path $PSScriptRoot 'installer\GlowboxSetup.cs'
    $installerManifest = Join-Path $PSScriptRoot 'installer\glowbox-setup.manifest'
    $installerExe = Join-Path $Output 'Glowbox-Setup.exe'
    $productVersion = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'VERSION') -Raw).Trim()
    if ($productVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid VERSION.' }
    $installerVersionSource = Join-Path $Output 'installer-version.cs'
    @(
        'using System.Reflection;',
        ('[assembly: AssemblyVersion("' + $productVersion + '.0")]'),
        ('[assembly: AssemblyFileVersion("' + $productVersion + '.0")]'),
        ('[assembly: AssemblyInformationalVersion("' + $productVersion + '-preview")]')
    ) | Set-Content -LiteralPath $installerVersionSource -Encoding UTF8
    $apiHeader = [IO.File]::ReadAllText((Join-Path $ObsSource 'libobs/obs-config.h'))
    $apiParts = foreach ($part in @('MAJOR','MINOR')) {
        $match = [regex]::Match($apiHeader, "(?m)^#define\s+LIBOBS_API_${part}_VER\s+(\d+)")
        if (!$match.Success) { throw "Cannot read OBS API version: $part" }
        $match.Groups[1].Value
    }
    $apiMetadata = Join-Path $Output 'obs-api.txt'
    ($apiParts -join '.') | Set-Content -LiteralPath $apiMetadata -Encoding ASCII
    $dllPayload = [IO.Path]::GetFullPath("$pluginDir/glowbox.dll")
    $effectPayload = [IO.Path]::GetFullPath("$dataDir/compositor.effect")
    Invoke-Checked $csc @('/nologo', '/target:winexe', '/platform:x64', "/out:$installerExe", '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll', "/win32manifest:$installerManifest", "/resource:$dllPayload,glowbox.dll", "/resource:$effectPayload,compositor.effect", "/resource:$dataDir/loading.png,loading.png", "/resource:$dataDir/loading-failed.png,loading-failed.png", "/resource:$dataDir/spinner.png,spinner.png", "/resource:$apiMetadata,obs-api.txt", $installerSource, $installerVersionSource)
}
Get-FileHash -LiteralPath "$pluginDir/glowbox.dll", "$dataDir/compositor.effect" -Algorithm SHA256 | Format-Table -AutoSize
Write-Host "Build complete: $Output"
