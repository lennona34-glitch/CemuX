param(
    [string]$RootDir = $PSScriptRoot
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $RootDir) { $RootDir = $PSScriptRoot }
$Root = (Resolve-Path $RootDir).Path
$DlDir = "$Root\dependencies\vcpkg\downloads"
$SrcOut = "$Root\dependencies\vcpkg\buildtrees\_fast_src"
$Inst = "$Root\build\vcpkg_installed\x64-windows-static"
$IncDir = "$Inst\include"
$LibDir = "$Inst\lib"

New-Item -ItemType Directory -Force -Path $DlDir, $SrcOut, $IncDir, $LibDir | Out-Null

Write-Host "=== CemuX Dependency Verification & Preparation ==="

# 1. Base 3rd-Party Dependencies
$dependencies = @(
    @{ Name="rapidjson"; File="rapidjson-a95e013.tar.gz"; Url="https://github.com/Tencent/rapidjson/archive/a95e013b97ca6523f32da23f5095fcc9dd6067e5.tar.gz"; OutDir="rapidjson-a95e013"; Strip=1 },
    @{ Name="glm"; File="glm-1.0.1.tar.gz"; Url="https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz"; OutDir="glm-1.0.1"; Strip=1 },
    @{ Name="fmt"; File="fmt-10.2.1.tar.gz"; Url="https://github.com/fmtlib/fmt/archive/refs/tags/10.2.1.tar.gz"; OutDir="fmt-10.2.1"; Strip=1 },
    @{ Name="pugixml"; File="pugixml-v1.14.tar.gz"; Url="https://github.com/zeux/pugixml/archive/refs/tags/v1.14.tar.gz"; OutDir="pugixml-1.14"; Strip=1 },
    @{ Name="hidapi"; File="hidapi-0.14.0.tar.gz"; Url="https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz"; OutDir="hidapi-hidapi-0.14.0"; Strip=1 },
    @{ Name="zstd"; File="zstd-v1.5.6.tar.gz"; Url="https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz"; OutDir="zstd-1.5.6"; Strip=1 },
    @{ Name="libpng"; File="libpng-v1.6.43.tar.gz"; Url="https://github.com/glennrp/libpng/archive/refs/tags/v1.6.43.tar.gz"; OutDir="libpng-1.6.43"; Strip=1 },
    @{ Name="libusb"; File="libusb-v1.0.27.tar.gz"; Url="https://github.com/libusb/libusb/archive/refs/tags/v1.0.27.tar.gz"; OutDir="libusb-1.0.27"; Strip=1 },
    @{ Name="libzip"; File="libzip-v1.10.1.tar.gz"; Url="https://github.com/nih-at/libzip/archive/refs/tags/v1.10.1.tar.gz"; OutDir="libzip-1.10.1"; Strip=1 },
    @{ Name="curl"; File="curl-8_8_0.tar.gz"; Url="https://github.com/curl/curl/archive/refs/tags/curl-8_8_0.tar.gz"; OutDir="curl-curl-8_8_0"; Strip=1 },
    @{ Name="glslang"; File="glslang-14.2.0.tar.gz"; Url="https://github.com/KhronosGroup/glslang/archive/refs/tags/14.2.0.tar.gz"; OutDir="glslang-14.2.0"; Strip=1 },
    @{ Name="sdl2"; File="sdl2-release-2.30.3.tar.gz"; Url="https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.30.3.tar.gz"; OutDir="SDL-release-2.30.3"; Strip=1 },
    @{ Name="zlib"; File="zlib-1.3.1.tar.gz"; Url="https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz"; OutDir="zlib-1.3.1"; Strip=1 },
    @{ Name="wxwidgets"; File="wxWidgets-3.2.5.zip"; Url="https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.5/wxWidgets-3.2.5.zip"; OutDir="wxWidgets-3.2.5"; Strip=0 }
)

$wc = New-Object System.Net.WebClient
$wc.Headers.Add("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)")

foreach ($dep in $dependencies) {
    $outFile = Join-Path $DlDir $dep.File
    if (-not (Test-Path $outFile) -or (Get-Item $outFile).Length -lt 1000) {
        Write-Host "Downloading $($dep.Name)..."
        $wc.DownloadFile($dep.Url, $outFile)
    }

    $targetDir = Join-Path $SrcOut $dep.OutDir
    if (-not (Test-Path $targetDir)) {
        Write-Host "Extracting $($dep.File)..."
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        if ($dep.Strip -gt 0) {
            & C:\WINDOWS\system32\tar.exe -xf "$outFile" -C "$targetDir" --strip-components $($dep.Strip)
        } else {
            & C:\WINDOWS\system32\tar.exe -xf "$outFile" -C "$targetDir"
        }
    }
}

# 2. Boost Modules Extraction
$boostArchives = Get-ChildItem $DlDir -Filter "boostorg-*.tar.gz" -ErrorAction SilentlyContinue
if ($boostArchives.Count -gt 0) {
    if (-not (Test-Path "$IncDir\boost\version.hpp") -or -not (Test-Path "$IncDir\boost\filesystem.hpp")) {
        Write-Host "Extracting Boost modules & headers ($($boostArchives.Count) packages)..."
        foreach ($ba in $boostArchives) {
            $modName = $ba.Name -replace "^boostorg-", "" -replace "-boost-1\.85\.0\.tar\.gz$", ""
            $modDest = Join-Path $SrcOut "boost_$modName"
            if (-not (Test-Path $modDest)) {
                New-Item -ItemType Directory -Force -Path $modDest | Out-Null
                & C:\WINDOWS\system32\tar.exe -xf $ba.FullName -C $modDest --strip-components 1
            }
            $incBoost = Join-Path $modDest "include\boost"
            if (Test-Path $incBoost) {
                $targetBoost = Join-Path $IncDir "boost"
                New-Item -ItemType Directory -Force -Path $targetBoost | Out-Null
                robocopy "$incBoost" "$targetBoost" /E /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
            }
        }
    }
}

# 3. OpenSSL Check
if (-not (Test-Path "$LibDir\libcrypto.lib") -or -not (Test-Path "$LibDir\libssl.lib")) {
    # Check local vcpkg cache if available
    $vcpkgZip = Get-ChildItem "$env:LOCALAPPDATA\vcpkg\archives\*\*.zip" -ErrorAction SilentlyContinue | Where-Object {
        $_.Length -gt 40000000 -and $_.Length -lt 45000000
    } | Select-Object -First 1
    if ($vcpkgZip) {
        Write-Host "Extracting prebuilt OpenSSL from vcpkg cache ($($vcpkgZip.FullName))..."
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $z = [System.IO.Compression.ZipFile]::OpenRead($vcpkgZip.FullName)
        foreach ($entry in $z.Entries) {
            if ($entry.FullName -in @("CONTROL", "BUILD_INFO", "vcpkg_abi_info.txt") -or $entry.FullName.EndsWith("/")) { continue }
            $destPath = [System.IO.Path]::Combine($Inst, $entry.FullName.Replace('/', '\'))
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($destPath)) | Out-Null
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destPath, $true)
        }
        $z.Dispose()
    } else {
        Write-Warning "OpenSSL static libraries not found in $LibDir."
        Write-Warning "Please run 'vcpkg install openssl:x64-windows-static' or provide libcrypto.lib/libssl.lib in $LibDir."
    }
}

Write-Host "=== Dependencies Ready! ==="
