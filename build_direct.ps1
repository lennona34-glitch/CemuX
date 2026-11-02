param(
    [string]$RootDir = $PSScriptRoot
)

$ErrorActionPreference = "Stop"

if (-not $RootDir) { $RootDir = $PSScriptRoot }
$Root = (Resolve-Path $RootDir).Path
$FastSrc = "$Root\dependencies\vcpkg\buildtrees\_fast_src"
$ZlibSrc = "$FastSrc\zlib-1.3.1"
$VcpkgInstalled = "$Root\build\vcpkg_installed\x64-windows-static"
$IncDir = "$VcpkgInstalled\include"
$LibDir = "$VcpkgInstalled\lib"
$ObjRoot = "$Root\build\direct_objs"
$LogFile = "$Root\build\direct_build.log"

New-Item -ItemType Directory -Force -Path $IncDir, $LibDir, $ObjRoot, "$Root\bin" | Out-Null
"" | Set-Content -Path $LogFile -Encoding UTF8

function Log-Msg([string]$msg) {
    $line = "[$(Get-Date -Format 'HH:mm:ss')] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

Log-Msg "=== CemuX Fast Direct Build Starting ==="
Log-Msg "Repository Root: $Root"

# Step 0: Discovering MSVC & Windows SDK
$ClInPath = Get-Command "cl.exe" -ErrorAction SilentlyContinue
$LinkInPath = Get-Command "link.exe" -ErrorAction SilentlyContinue
if ($ClInPath -and $LinkInPath -and $env:INCLUDE -and $env:LIB) {
    Log-Msg "Using existing Visual Studio environment from PATH."
    $ClExe   = $ClInPath.Source
    $LibExe  = (Get-Command "lib.exe").Source
    $LinkExe = $LinkInPath.Source
    $Ml64Exe = (Get-Command "ml64.exe").Source
    $RcExe   = (Get-Command "rc.exe").Source
} else {
    Log-Msg "Discovering MSVC & Windows SDK..."
    $MsvcBin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64" -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
    if (-not $MsvcBin) {
        $MsvcBin = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64" -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
    }
    if (-not $MsvcBin) {
        throw "Could not find Visual Studio 2022 x64 MSVC compiler. Please run from Developer Command Prompt or install VS 2022."
    }
    $MsvcRoot = $MsvcBin.Parent.Parent.Parent.FullName
    $ClExe   = Join-Path $MsvcBin.FullName "cl.exe"
    $LibExe  = Join-Path $MsvcBin.FullName "lib.exe"
    $LinkExe = Join-Path $MsvcBin.FullName "link.exe"
    $Ml64Exe = Join-Path $MsvcBin.FullName "ml64.exe"

    $SdkIncVer = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Include\10.*" -Directory | Sort-Object Name | Select-Object -Last 1
    $SdkVer = $SdkIncVer.Name
    $SdkRoot = "C:\Program Files (x86)\Windows Kits\10"
    $RcExe = Join-Path "$SdkRoot\bin\$SdkVer\x64" "rc.exe"

    $IncludePaths = @(
        "$MsvcRoot\include",
        "$MsvcRoot\atlmfc\include",
        "$SdkRoot\Include\$SdkVer\ucrt",
        "$SdkRoot\Include\$SdkVer\um",
        "$SdkRoot\Include\$SdkVer\shared",
        "$SdkRoot\Include\$SdkVer\winrt",
        "$SdkRoot\Include\$SdkVer\cppwinrt"
    ) | Where-Object { Test-Path $_ }

    $LibPaths = @(
        "$MsvcRoot\lib\x64",
        "$MsvcRoot\atlmfc\lib\x64",
        "$SdkRoot\Lib\$SdkVer\ucrt\x64",
        "$SdkRoot\Lib\$SdkVer\um\x64"
    ) | Where-Object { Test-Path $_ }

    $env:INCLUDE = ($IncludePaths -join ";")
    $env:LIB     = ($LibPaths -join ";")
    $env:PATH    = "$($MsvcBin.FullName);$SdkRoot\bin\$SdkVer\x64;$env:PATH"
}

Log-Msg "CL: $ClExe"
Log-Msg "LINK: $LinkExe"

# Step 1: Staging Headers & Config Files if needed
$GlslSrc = "$FastSrc\glslang-14.2.0"
$SdlSrc = "$FastSrc\SDL-release-2.30.3"
$WxSrc = "$FastSrc\wxWidgets-3.2.5"
$ZipSrc = "$FastSrc\libzip-1.10.1"

if (-not (Test-Path "$IncDir\wx\setup.h") -or -not (Test-Path "$IncDir\glslang\build_info.h")) {
    Log-Msg "=== Step 1: Staging Headers & Config Files ==="

    if (-not (Test-Path "$IncDir\rapidjson\rapidjson.h")) {
        Copy-Item "$FastSrc\rapidjson-a95e013\include\rapidjson" "$IncDir\rapidjson" -Recurse -Force
    }
    if (-not (Test-Path "$IncDir\glm\glm.hpp")) {
        Copy-Item "$FastSrc\glm-1.0.1\glm" "$IncDir\glm" -Recurse -Force
    }
    if (-not (Test-Path "$IncDir\fmt\core.h")) {
        Copy-Item "$FastSrc\fmt-10.2.1\include\fmt" "$IncDir\fmt" -Recurse -Force
    }
    Copy-Item "$FastSrc\pugixml-1.14\src\*.hpp" $IncDir -Force
    New-Item -ItemType Directory -Force -Path "$IncDir\hidapi" | Out-Null
    Copy-Item "$FastSrc\hidapi-hidapi-0.14.0\hidapi\hidapi.h" "$IncDir\hidapi\hidapi.h" -Force

    Copy-Item "$ZlibSrc\zlib.h" $IncDir -Force
    Copy-Item "$ZlibSrc\zconf.h.in" "$IncDir\zconf.h" -Force
    Copy-Item "$ZlibSrc\zconf.h.in" "$ZlibSrc\zconf.h" -Force

    Copy-Item "$FastSrc\zstd-1.5.6\lib\zstd.h", "$FastSrc\zstd-1.5.6\lib\zdict.h", "$FastSrc\zstd-1.5.6\lib\zstd_errors.h" $IncDir -Force
    Copy-Item "$FastSrc\libpng-1.6.43\scripts\pnglibconf.h.prebuilt" "$FastSrc\libpng-1.6.43\pnglibconf.h" -Force
    Copy-Item "$FastSrc\libpng-1.6.43\png.h", "$FastSrc\libpng-1.6.43\pngconf.h", "$FastSrc\libpng-1.6.43\pnglibconf.h" $IncDir -Force

    New-Item -ItemType Directory -Force -Path "$IncDir\libusb-1.0" | Out-Null
    Copy-Item "$FastSrc\libusb-1.0.27\libusb\libusb.h" "$IncDir\libusb-1.0\libusb.h" -Force
    Copy-Item "$FastSrc\libusb-1.0.27\libusb\libusb.h" "$IncDir\libusb.h" -Force

    $ZipConfContent = @"
#ifndef _HAD_ZIPCONF_H
#define _HAD_ZIPCONF_H
#define LIBZIP_VERSION "1.10.1"
#define LIBZIP_VERSION_MAJOR 1
#define LIBZIP_VERSION_MINOR 10
#define LIBZIP_VERSION_MICRO 1
#define ZIP_STATIC 1
#define _Nullable
#define _Nonnull
#include <stdint.h>
typedef int8_t zip_int8_t;
typedef uint8_t zip_uint8_t;
typedef int16_t zip_int16_t;
typedef uint16_t zip_uint16_t;
typedef int32_t zip_int32_t;
typedef uint32_t zip_uint32_t;
typedef int64_t zip_int64_t;
typedef uint64_t zip_uint64_t;
#define ZIP_INT8_MIN (-ZIP_INT8_MAX-1)
#define ZIP_INT8_MAX 0x7f
#define ZIP_UINT8_MAX 0xff
#define ZIP_INT16_MIN (-ZIP_INT16_MAX-1)
#define ZIP_INT16_MAX 0x7fff
#define ZIP_UINT16_MAX 0xffff
#define ZIP_INT32_MIN (-ZIP_INT32_MAX-1L)
#define ZIP_INT32_MAX 0x7fffffffL
#define ZIP_UINT32_MAX 0xffffffffLU
#define ZIP_INT64_MIN (-ZIP_INT64_MAX-1LL)
#define ZIP_INT64_MAX 0x7fffffffffffffffLL
#define ZIP_UINT64_MAX 0xffffffffffffffffULL
#endif
"@
    Set-Content -Path "$ZipSrc\lib\zipconf.h" -Value $ZipConfContent -Encoding ASCII
    Set-Content -Path "$IncDir\zipconf.h" -Value $ZipConfContent -Encoding ASCII
    Copy-Item "$ZipSrc\lib\zip.h" "$IncDir\zip.h" -Force

    $ZipConfigH = @"
#ifndef HAD_CONFIG_H
#define HAD_CONFIG_H
#ifndef _HAD_ZIPCONF_H
#include "zipconf.h"
#endif
#define ENABLE_FDOPEN 1
#define HAVE__CLOSE 1
#define HAVE__DUP 1
#define HAVE__FDOPEN 1
#define HAVE__FILENO 1
#define HAVE__SETMODE 1
#define HAVE__SNPRINTF 1
#define HAVE__SNPRINTF_S 1
#define HAVE__SNWPRINTF_S 1
#define HAVE__STRDUP 1
#define HAVE__STRICMP 1
#define HAVE__STRTOI64 1
#define HAVE__STRTOUI64 1
#define HAVE__UNLINK 1
#define HAVE_FILENO 1
#define HAVE_LOCALTIME_S 1
#define HAVE_MEMCPY_S 1
#define HAVE_SETMODE 1
#define HAVE_SNPRINTF 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR_S 1
#define HAVE_STRICMP 1
#define HAVE_STRNCPY_S 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STDBOOL_H 1
#define HAVE_CRYPTO 1
#define HAVE_WINDOWS_CRYPTO 1
#define SIZEOF_OFF_T 4
#define SIZEOF_SIZE_T 8
#define PACKAGE "libzip"
#define VERSION "1.10.1"
#endif
"@
    Set-Content -Path "$ZipSrc\lib\config.h" -Value $ZipConfigH -Encoding ASCII

    $zipHText = Get-Content "$ZipSrc\lib\zip.h" -Raw
    $zipIntHText = Get-Content "$ZipSrc\lib\zipint.h" -Raw
    $errLines = [regex]::Matches($zipHText, '#define ZIP_ER_([A-Z0-9_]+) ([0-9]+)[ \t]+/\* ([LNSZ]+) ([-0-9a-zA-Z, '']*) \*/')
    $detailLines = [regex]::Matches($zipIntHText, '#define ZIP_ER_DETAIL_([A-Z0-9_]+) ([0-9]+)[ \t]+/\* ([EG]+) ([-0-9a-zA-Z, '']*) \*/')
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('#include "zipint.h"')
    [void]$sb.AppendLine('#define L ZIP_ET_LIBZIP')
    [void]$sb.AppendLine('#define N ZIP_ET_NONE')
    [void]$sb.AppendLine('#define S ZIP_ET_SYS')
    [void]$sb.AppendLine('#define Z ZIP_ET_ZLIB')
    [void]$sb.AppendLine('#define E ZIP_DETAIL_ET_ENTRY')
    [void]$sb.AppendLine('#define G ZIP_DETAIL_ET_GLOBAL')
    [void]$sb.AppendLine('const struct _zip_err_info _zip_err_str[] = {')
    foreach ($m in $errLines) {
        $t = $m.Groups[3].Value.Trim()
        $d = $m.Groups[4].Value.Trim()
        [void]$sb.AppendLine("    { $t, `"$d`" },")
    }
    [void]$sb.AppendLine('};')
    [void]$sb.AppendLine('const int _zip_err_str_count = sizeof(_zip_err_str)/sizeof(_zip_err_str[0]);')
    [void]$sb.AppendLine('const struct _zip_err_info _zip_err_details[] = {')
    foreach ($m in $detailLines) {
        $t = $m.Groups[3].Value.Trim()
        $d = $m.Groups[4].Value.Trim()
        [void]$sb.AppendLine("    { $t, `"$d`" },")
    }
    [void]$sb.AppendLine('};')
    [void]$sb.AppendLine('const int _zip_err_details_count = sizeof(_zip_err_details)/sizeof(_zip_err_details[0]);')
    Set-Content -Path "$ZipSrc\lib\zip_err_str.c" -Value $sb.ToString() -Encoding ASCII

    if (-not (Test-Path "$IncDir\curl\curl.h")) {
        Copy-Item "$FastSrc\curl-curl-8_8_0\include\curl" "$IncDir\curl" -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path "$IncDir\glslang\Include", "$IncDir\glslang\Public", "$IncDir\glslang\MachineIndependent", "$IncDir\glslang\SPIRV" | Out-Null
    $BuildInfoH = @"
#ifndef GLSLANG_BUILD_INFO_H
#define GLSLANG_BUILD_INFO_H
#define GLSLANG_VERSION_MAJOR 14
#define GLSLANG_VERSION_MINOR 2
#define GLSLANG_VERSION_PATCH 0
#define GLSLANG_VERSION_FLAVOR ""
#endif
"@
    Set-Content -Path "$IncDir\glslang\build_info.h" -Value $BuildInfoH -Encoding ASCII
    Copy-Item "$GlslSrc\glslang\Public\*" "$IncDir\glslang\Public\" -Force
    Copy-Item "$GlslSrc\glslang\Include\*" "$IncDir\glslang\Include\" -Force
    Copy-Item "$GlslSrc\glslang\MachineIndependent\*.h" "$IncDir\glslang\MachineIndependent\" -Force
    Copy-Item "$GlslSrc\SPIRV\*.h", "$GlslSrc\SPIRV\*.hpp" "$IncDir\glslang\SPIRV\" -Force

    New-Item -ItemType Directory -Force -Path "$IncDir\SDL2" | Out-Null
    Copy-Item "$SdlSrc\include\*.h" "$IncDir\SDL2\" -Force
    Copy-Item "$SdlSrc\include\*.h" "$IncDir\" -Force

    if (-not (Test-Path "$IncDir\wx\wx.h")) {
        Copy-Item "$WxSrc\include\wx" "$IncDir\wx" -Recurse -Force
    }
    $SetupH = Get-Content "$WxSrc\include\wx\msw\setup.h" -Raw
    $SetupH = $SetupH.Replace("defined(WXUSINGDLL)", "0")
    $SetupH = [regex]::Replace($SetupH, '#define\s+wxUSE_MEDIACTRL\s+1', '#define wxUSE_MEDIACTRL 0')
    $SetupH = [regex]::Replace($SetupH, '#define\s+wxUSE_WEBVIEW\s+1', '#define wxUSE_WEBVIEW 0')
    $SetupH = [regex]::Replace($SetupH, '#define\s+wxUSE_WEBVIEW_IE\s+1', '#define wxUSE_WEBVIEW_IE 0')
    $SetupH = [regex]::Replace($SetupH, '#define\s+wxUSE_WEBVIEW_EDGE\s+1', '#define wxUSE_WEBVIEW_EDGE 0')
    $SetupH = [regex]::Replace($SetupH, '#define\s+wxUSE_STC\s+1', '#define wxUSE_STC 0')
    Set-Content -Path "$WxSrc\include\wx\setup.h" -Value $SetupH -Encoding ASCII
    Set-Content -Path "$IncDir\wx\setup.h" -Value $SetupH -Encoding ASCII

    New-Item -ItemType Directory -Force -Path "$Root\dependencies\cubeb\include\cubeb" | Out-Null
    Set-Content -Path "$Root\dependencies\cubeb\include\cubeb\cubeb_export.h" -Value "#pragma once`r`n#define CUBEB_EXPORT`r`n" -Encoding ASCII
    Set-Content -Path "$Root\dependencies\cubeb\include\cubeb_export.h" -Value "#pragma once`r`n#define CUBEB_EXPORT`r`n" -Encoding ASCII

    Log-Msg "Headers and configs staged."
} else {
    Log-Msg "Headers already staged."
}

# Batch compile helper function
function Invoke-ClBatch {
    param(
        [string]$BatchName,
        [string[]]$SourceFiles,
        [string[]]$ClFlags,
        [string]$OutputLib
    )
    if ($OutputLib -and (Test-Path $OutputLib)) {
        Log-Msg "[$BatchName] Skipping (already exists: $OutputLib)"
        return
    }
    $BatchObjDir = Join-Path $ObjRoot $BatchName
    $WrapperDir  = Join-Path $BatchObjDir "_wrappers"
    New-Item -ItemType Directory -Force -Path $BatchObjDir, $WrapperDir | Out-Null

    $SeenBasenames = @{}
    $EffectiveSources = New-Object System.Collections.Generic.List[string]
    $ExtraIncludes = New-Object System.Collections.Generic.HashSet[string]
    $dupIdx = 0

    foreach ($src in $SourceFiles) {
        $fullSrc = [System.IO.Path]::GetFullPath($src)
        $base = [System.IO.Path]::GetFileNameWithoutExtension($fullSrc).ToLowerInvariant()
        $ext  = [System.IO.Path]::GetExtension($fullSrc)
        if ($SeenBasenames.ContainsKey($base)) {
            $dupIdx++
            $objPath = Join-Path $BatchObjDir "dup_${dupIdx}_${base}.obj"
            if (-not (Test-Path $objPath)) {
                $wrapFile = Join-Path $WrapperDir "dup_${dupIdx}_${base}${ext}"
                Copy-Item -Path $fullSrc -Destination $wrapFile -Force
                $EffectiveSources.Add($wrapFile)
                [void]$ExtraIncludes.Add([System.IO.Path]::GetDirectoryName($fullSrc))
            }
        } else {
            $SeenBasenames[$base] = $true
            $objPath = Join-Path $BatchObjDir "$base.obj"
            if (-not (Test-Path $objPath)) {
                $EffectiveSources.Add($fullSrc)
            }
        }
    }

    if ($EffectiveSources.Count -gt 0) {
        $RspLines = New-Object System.Collections.Generic.List[string]
        $RspLines.Add("/nologo")
        $RspLines.Add("/c")
        foreach ($f in $ClFlags) {
            $RspLines.Add($f)
        }
        foreach ($inc in $ExtraIncludes) {
            $cleanInc = $inc.TrimEnd('\')
            $RspLines.Add("/I`"$cleanInc`"")
        }
        if ($IncludePaths) {
            foreach ($sysInc in $IncludePaths) {
                $RspLines.Add("/I`"$($sysInc.TrimEnd('\'))`"")
            }
        }
        foreach ($s in $EffectiveSources) {
            $RspLines.Add("`"$s`"")
        }

        $RspFile = Join-Path $BatchObjDir "compile.rsp"
        Set-Content -Path $RspFile -Value ($RspLines -join "`r`n") -Encoding ASCII

        Log-Msg "[$BatchName] Compiling $($EffectiveSources.Count) pending files..."
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        Push-Location $BatchObjDir
        try {
            $out = & $ClExe "@$RspFile" 2>&1
            $exitCode = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        $sw.Stop()
        if ($exitCode -ne 0) {
            $errText = ($out | Out-String)
            Add-Content -Path $LogFile -Value $errText -Encoding UTF8
            throw "[$BatchName] cl.exe failed with exit code $exitCode! Output:`n$errText"
        }
        Log-Msg "[$BatchName] Compiled in $([math]::Round($sw.Elapsed.TotalSeconds, 1))s."
    } else {
        Log-Msg "[$BatchName] All object files up to date."
    }

    if ($OutputLib) {
        $LibRsp = Join-Path $BatchObjDir "lib.rsp"
        $Objs = Get-ChildItem "$BatchObjDir\*.obj" | ForEach-Object { "`"$($_.FullName)`"" }
        Set-Content -Path $LibRsp -Value ($Objs -join "`r`n") -Encoding ASCII
        & $LibExe /NOLOGO "/OUT:$OutputLib" "@$LibRsp" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "[$BatchName] lib.exe failed!" }
        Log-Msg "[$BatchName] Created $OutputLib ($($Objs.Count) objects)."
    }
}

# Compile Boost Libraries if needed
if (-not (Test-Path "$LibDir\boost_program_options.lib") -or -not (Test-Path "$LibDir\boost_filesystem.lib") -or -not (Test-Path "$LibDir\boost_nowide.lib")) {
    Log-Msg "=== Compiling Boost Libraries ==="
    $BoostObjDir = Join-Path $ObjRoot "boost"
    $PoDir = Join-Path $BoostObjDir "po"
    $FsDir = Join-Path $BoostObjDir "fs"
    $NwDir = Join-Path $BoostObjDir "nw"
    New-Item -ItemType Directory -Force -Path $PoDir, $FsDir, $NwDir | Out-Null

    # 1. boost_nowide
    $nowideSources = Get-ChildItem "$FastSrc\boost_nowide\src\*.cpp" | ForEach-Object { "`"$($_.FullName)`"" }
    & $ClExe /nologo /c /TP /std:c++17 /EHsc /GR /MT /O2 /Ob2 /DNDEBUG /DWIN32 /D_WINDOWS /DBOOST_ALL_NO_LIB /DBOOST_NOWIDE_NO_LIB=1 "/I$IncDir" "/Fo$NwDir\" $nowideSources
    $nwObjs = Get-ChildItem "$NwDir\*.obj" | ForEach-Object { "`"$($_.FullName)`"" }
    & $LibExe /nologo "/out:$LibDir\boost_nowide.lib" $nwObjs
    Copy-Item "$LibDir\boost_nowide.lib" "$LibDir\libboost_nowide-vc143-mt-s-x64-1_85.lib" -Force

    # 2. boost_program_options
    $poSources = Get-ChildItem "$FastSrc\boost_program_options\src\*.cpp" | ForEach-Object { "`"$($_.FullName)`"" }
    & $ClExe /nologo /c /TP /std:c++17 /EHsc /GR /MT /O2 /Ob2 /DNDEBUG /DWIN32 /D_WINDOWS /DBOOST_ALL_NO_LIB /DBOOST_PROGRAM_OPTIONS_NO_LIB=1 "/I$IncDir" "/Fo$PoDir\" $poSources
    $poObjs = Get-ChildItem "$PoDir\*.obj" | ForEach-Object { "`"$($_.FullName)`"" }
    & $LibExe /nologo "/out:$LibDir\boost_program_options.lib" $poObjs
    Copy-Item "$LibDir\boost_program_options.lib" "$LibDir\libboost_program_options-vc143-mt-s-x64-1_85.lib" -Force

    # 3. boost_filesystem
    $fsSources = Get-ChildItem "$FastSrc\boost_filesystem\src\*.cpp" | ForEach-Object { "`"$($_.FullName)`"" }
    & $ClExe /nologo /c /TP /std:c++20 /EHsc /GR /MT /O2 /Ob2 /DNDEBUG /DWIN32 /D_WINDOWS /DBOOST_ALL_NO_LIB /DBOOST_FILESYSTEM_STATIC_LINK=1 "/I$IncDir" "/Fo$FsDir\" $fsSources
    $fsObjs = Get-ChildItem "$FsDir\*.obj" | ForEach-Object { "`"$($_.FullName)`"" }
    & $LibExe /nologo "/out:$LibDir\boost_filesystem.lib" $fsObjs
    Copy-Item "$LibDir\boost_filesystem.lib" "$LibDir\libboost_filesystem-vc143-mt-s-x64-1_85.lib" -Force
}

# === Call 1: deps_c.lib ===
$DepsCFiles = New-Object System.Collections.Generic.List[string]
Get-ChildItem "$ZlibSrc\*.c" | ForEach-Object { $DepsCFiles.Add($_.FullName) }
Get-ChildItem "$FastSrc\libpng-1.6.43\*.c" | Where-Object { $_.Name -notmatch "^(example|pngtest)" } | ForEach-Object { $DepsCFiles.Add($_.FullName) }
$DepsCFiles.Add("$FastSrc\libpng-1.6.43\intel\intel_init.c")
$DepsCFiles.Add("$FastSrc\libpng-1.6.43\intel\filter_sse2_intrinsics.c")
Get-ChildItem "$FastSrc\zstd-1.5.6\lib\common\*.c", "$FastSrc\zstd-1.5.6\lib\compress\*.c", "$FastSrc\zstd-1.5.6\lib\decompress\*.c", "$FastSrc\zstd-1.5.6\lib\dictBuilder\*.c" | ForEach-Object { $DepsCFiles.Add($_.FullName) }
$DepsCFiles.Add("$FastSrc\hidapi-hidapi-0.14.0\windows\hid.c")
@(
    "core.c", "descriptor.c", "hotplug.c", "io.c", "strerror.c", "sync.c",
    "os\events_windows.c", "os\threads_windows.c", "os\windows_common.c", "os\windows_usbdk.c", "os\windows_winusb.c"
) | ForEach-Object { $DepsCFiles.Add("$FastSrc\libusb-1.0.27\libusb\$_") }

$ZipCMake = Get-Content "$ZipSrc\lib\CMakeLists.txt" -Raw
$ZipBaseMatches = [regex]::Matches($ZipCMake, '(?m)^\s*(zip_[a-zA-Z0-9_-]+\.c)\s*$')
foreach ($m in $ZipBaseMatches) {
    $zname = $m.Groups[1].Value
    if ($zname -notmatch 'stdio_named|random_unix|win32') {
        $DepsCFiles.Add("$ZipSrc\lib\$zname")
    }
}
@(
    "zip_err_str.c",
    "zip_source_file_win32.c", "zip_source_file_win32_named.c", "zip_source_file_win32_utf16.c",
    "zip_source_file_win32_utf8.c", "zip_source_file_win32_ansi.c", "zip_random_win32.c",
    "zip_crypto_win.c", "zip_winzip_aes.c", "zip_source_winzip_aes_decode.c", "zip_source_winzip_aes_encode.c"
) | ForEach-Object { $DepsCFiles.Add("$ZipSrc\lib\$_") }

$Ih264Cmake = Get-Content "$Root\dependencies\ih264d\CMakeLists.txt" -Raw
$Ih264Matches = [regex]::Matches($Ih264Cmake, '(decoder/[^\s\)]+\.c|common/[^\s\)]+\.c)')
foreach ($m in $Ih264Matches) {
    $rel = $m.Groups[1].Value.Replace('/', '\')
    if ($rel -notmatch 'arm') { $DepsCFiles.Add("$Root\dependencies\ih264d\$rel") }
}
$DepsCFiles.Add("$Root\dependencies\cubeb\src\cubeb.c")
$DepsCFiles.Add("$Root\dependencies\cubeb\src\cubeb_strings.c")
$DepsCFiles.Add("$Root\dependencies\ZArchive\src\sha_256.c")

$DepsCFlags = @(
    "/TC", "/MT", "/O2", "/Ob2", "/DNDEBUG", "/W0",
    "/D_CRT_SECURE_NO_WARNINGS", "/D_CRT_NONSTDC_NO_DEPRECATE", "/DWIN32", "/D_WINDOWS",
    "/DPNG_INTEL_SSE", "/DZIP_STATIC",
    "/DCUBEB_WASAPI", "/D_USE_MATH_DEFINES", "/DX86", "/DDISABLE_AVX2",
    "/I`"$IncDir`"",
    "/I`"$ZlibSrc`"",
    "/I`"$FastSrc\libpng-1.6.43`"",
    "/I`"$FastSrc\zstd-1.5.6\lib`"",
    "/I`"$FastSrc\zstd-1.5.6\lib\common`"",
    "/I`"$FastSrc\hidapi-hidapi-0.14.0\hidapi`"",
    "/I`"$FastSrc\libusb-1.0.27\msvc`"",
    "/I`"$FastSrc\libusb-1.0.27\libusb`"",
    "/I`"$FastSrc\libusb-1.0.27\libusb\os`"",
    "/I`"$ZipSrc\lib`"",
    "/I`"$Root\dependencies\ih264d`"",
    "/I`"$Root\dependencies\ih264d\common`"",
    "/I`"$Root\dependencies\ih264d\common\x86`"",
    "/I`"$Root\dependencies\ih264d\decoder`"",
    "/I`"$Root\dependencies\ih264d\decoder\x86`"",
    "/I`"$Root\dependencies\cubeb\include`"",
    "/I`"$Root\dependencies\cubeb\src`""
)
Invoke-ClBatch -BatchName "deps_c" -SourceFiles $DepsCFiles -ClFlags $DepsCFlags -OutputLib "$LibDir\deps_c.lib"

# === Call 2: libcurl.lib ===
$CurlFiles = Get-ChildItem "$FastSrc\curl-curl-8_8_0\lib\*.c", "$FastSrc\curl-curl-8_8_0\lib\vauth\*.c", "$FastSrc\curl-curl-8_8_0\lib\vtls\*.c", "$FastSrc\curl-curl-8_8_0\lib\vquic\*.c", "$FastSrc\curl-curl-8_8_0\lib\vssh\*.c" | ForEach-Object { $_.FullName }
$CurlFlags = @(
    "/TC", "/MT", "/O2", "/Ob2", "/DNDEBUG", "/W0",
    "/D_CRT_SECURE_NO_WARNINGS", "/D_CRT_NONSTDC_NO_DEPRECATE",
    "/DBUILDING_LIBCURL", "/DCURL_STATICLIB", "/DHTTP_ONLY",
    "/DCURL_DISABLE_LDAP", "/DCURL_DISABLE_LDAPS",
    "/DUSE_OPENSSL", "/DUSE_SCHANNEL", "/DUSE_WINDOWS_SSPI", "/DHAVE_LIBZ",
    "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WIN32_WINNT=0x0600",
    "/I`"$FastSrc\curl-curl-8_8_0\include`"",
    "/I`"$FastSrc\curl-curl-8_8_0\lib`"",
    "/I`"$IncDir`""
)
Invoke-ClBatch -BatchName "libcurl" -SourceFiles $CurlFiles -ClFlags $CurlFlags -OutputLib "$LibDir\libcurl.lib"

# === Call 3: SDL2.lib ===
$SdlVcxproj = Get-Content "$SdlSrc\VisualC\SDL\SDL.vcxproj" -Raw
$SdlMatches = [regex]::Matches($SdlVcxproj, 'ClCompile Include="\.\.\\\.\.\\(src\\[^"]+\.c)"')
$SdlFiles = foreach ($m in $SdlMatches) { Join-Path $SdlSrc $m.Groups[1].Value }
$SdlFlags = @(
    "/TC", "/MT", "/O2", "/Ob2", "/DNDEBUG", "/W0",
    "/D_CRT_SECURE_NO_WARNINGS", "/DHAVE_LIBC", "/D_WINDOWS", "/DWIN32",
    "/DSDL_STATIC_LIB",
    "/I`"$SdlSrc\include`"",
    "/I`"$SdlSrc\src`"",
    "/I`"$SdlSrc\src\video\khronos`""
)
Invoke-ClBatch -BatchName "sdl2" -SourceFiles $SdlFiles -ClFlags $SdlFlags -OutputLib "$LibDir\SDL2.lib"

# === Call 4: wx3rdparty.lib ===
$WxMakefile = Get-Content "$WxSrc\build\msw\makefile.vc" -Raw
$Wx3rdMatches = [regex]::Matches($WxMakefile, '(?m)^\$\(OBJS\)\\(wxregex|wxjpeg|wxtiff|wxexpat|wxzlib)_[A-Za-z0-9_]+\.obj:\s+(\.\.\\\.\.\\[^\r\n]+)')
$Wx3rdFiles = New-Object System.Collections.Generic.List[string]
foreach ($m in $Wx3rdMatches) {
    $rel = $m.Groups[2].Value.Trim().Substring(6)
    $Wx3rdFiles.Add((Join-Path $WxSrc $rel))
}
$Wx3rdFiles.Add("$WxSrc\src\common\extended.c")
$Wx3rdFlags = @(
    "/TC", "/MT", "/O2", "/Ob2", "/DNDEBUG", "/W0",
    "/DWIN32", "/D_CRT_SECURE_NO_DEPRECATE=1", "/D_CRT_NON_CONFORMING_SWPRINTFS=1",
    "/D_SCL_SECURE_NO_WARNINGS=1", "/D_CRT_SECURE_NO_WARNINGS", "/D_CRT_NONSTDC_NO_WARNINGS",
    "/D__WX__", "/DHAVE_CONFIG_H", "/D_UNICODE", "/DUNICODE", "/D__WXMSW__", "/DWXBUILDING",
    "/I`"$WxSrc\include`"",
    "/I`"$WxSrc\3rdparty\pcre\src\wx`"",
    "/I`"$WxSrc\src\zlib`"",
    "/I`"$WxSrc\src\jpeg`"",
    "/I`"$WxSrc\src\tiff\libtiff`"",
    "/I`"$WxSrc\src\expat\expat\lib`""
)
Invoke-ClBatch -BatchName "wx3rdparty" -SourceFiles $Wx3rdFiles -ClFlags $Wx3rdFlags -OutputLib "$LibDir\wx3rdparty.lib"

# === Call 5: deps_cxx.lib ===
$DepsCxxFiles = New-Object System.Collections.Generic.List[string]
$DepsCxxFiles.Add("$FastSrc\fmt-10.2.1\src\format.cc")
$DepsCxxFiles.Add("$FastSrc\fmt-10.2.1\src\os.cc")
$DepsCxxFiles.Add("$FastSrc\pugixml-1.14\src\pugixml.cpp")
$DepsCxxFiles.Add("$Root\dependencies\ZArchive\src\zarchivereader.cpp")
$DepsCxxFiles.Add("$Root\dependencies\ZArchive\src\zarchivewriter.cpp")
@(
    "discord_register_win.cpp", "discord_rpc.cpp", "rpc_connection.cpp", "serialization.cpp", "connection_win.cpp"
) | ForEach-Object { $DepsCxxFiles.Add("$Root\dependencies\discord-rpc\src\$_") }
@(
    "cubeb_mixer.cpp", "cubeb_resampler.cpp", "cubeb_log.cpp", "cubeb_utils.cpp", "cubeb_wasapi.cpp"
) | ForEach-Object { $DepsCxxFiles.Add("$Root\dependencies\cubeb\src\$_") }
$DepsCxxFiles.Add("$Root\dependencies\cubeb\subprojects\speex\resample.c")
Get-ChildItem "$GlslSrc\glslang\GenericCodeGen\*.cpp", "$GlslSrc\glslang\MachineIndependent\*.cpp", "$GlslSrc\glslang\MachineIndependent\preprocessor\*.cpp", "$GlslSrc\glslang\HLSL\*.cpp", "$GlslSrc\glslang\CInterface\*.cpp", "$GlslSrc\glslang\ResourceLimits\*.cpp", "$GlslSrc\SPIRV\*.cpp", "$GlslSrc\SPIRV\CInterface\*.cpp" | Where-Object {
    $_.Name -notmatch "^main\.cpp$"
} | ForEach-Object { $DepsCxxFiles.Add($_.FullName) }
$DepsCxxFiles.Add("$GlslSrc\glslang\OSDependent\Windows\ossource.cpp")

$DepsCxxFlags = @(
    "/std:c++20", "/utf-8", "/EHsc", "/GR", "/MT", "/O2", "/Ob2", "/DNDEBUG", "/W0", "/bigobj",
    "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32", "/D_WINDOWS", "/DNOMINMAX",
    "/DCUBEB_WASAPI", "/DOUTSIDE_SPEEX", "/DFLOATING_POINT", "/DEXPORT=", "/DRANDOM_PREFIX=speex",
    "/D_USE_MATH_DEFINES", "/DENABLE_OPT=0", "/DENABLE_HLSL",
    "/I`"$IncDir`"",
    "/I`"$FastSrc\fmt-10.2.1\include`"",
    "/I`"$FastSrc\pugixml-1.14\src`"",
    "/I`"$Root\dependencies\ZArchive\include`"",
    "/I`"$Root\dependencies\discord-rpc\include`"",
    "/I`"$Root\dependencies\discord-rpc\src`"",
    "/I`"$Root\dependencies\cubeb\include`"",
    "/I`"$Root\dependencies\cubeb\src`"",
    "/I`"$Root\dependencies\cubeb\subprojects`"",
    "/I`"$GlslSrc`"",
    "/I`"$GlslSrc\glslang\..`"",
    "/I`"$IncDir\glslang`""
)
Invoke-ClBatch -BatchName "deps_cxx" -SourceFiles $DepsCxxFiles -ClFlags $DepsCxxFlags -OutputLib "$LibDir\deps_cxx.lib"

# === Call 6 & 7: wxmono.lib ===
if (-not (Test-Path "$LibDir\wxmono.lib")) {
    $WxObjDir = Join-Path $ObjRoot "wxmono"
    New-Item -ItemType Directory -Force -Path $WxObjDir | Out-Null

    $WxMonoMatches = [regex]::Matches($WxMakefile, '(?m)^\$\(OBJS\)\\(monolib|gllib)_[A-Za-z0-9_]+\.obj:\s+(\.\.\\\.\.\\[^\r\n]+)')
    $WxCppFiles = New-Object System.Collections.Generic.List[string]
    foreach ($m in $WxMonoMatches) {
        $rule = $m.Groups[0].Value
        $rel = $m.Groups[2].Value.Trim().Substring(6)
        if ($rel -match 'dummy\.cpp$' -or $rel -match 'extended\.c$' -or $rel -match 'webview' -or $rel -match 'mediactrl' -or $rel -match '\\stc\\' -or $rel -match '\\univ\\' -or $rule -match 'monolib_(generic_accel|generic_fdrepdlg|generic_listctrl|colrdlgg|dirdlgg|filedlgg|fontdlgg|mdig)\.obj:') {
            continue
        }
        $WxCppFiles.Add((Join-Path $WxSrc $rel))
    }

    $WxPchFile = Join-Path $WxObjDir "wxprec.pch"
    $WxCommonFlags = @(
        "/TP", "/std:c++17", "/utf-8", "/EHsc", "/GR", "/MT", "/O2", "/Ob2", "/DNDEBUG", "/W0", "/bigobj",
        "/DWIN32", "/D_CRT_SECURE_NO_DEPRECATE=1", "/D_CRT_NON_CONFORMING_SWPRINTFS=1",
        "/D_SCL_SECURE_NO_WARNINGS=1", "/D_CRT_SECURE_NO_WARNINGS",
        "/D__WXMSW__", "/D_UNICODE", "/DUNICODE", "/DWXBUILDING", "/DwxUSE_BASE=1",
        "/DwxUSE_MEDIACTRL=0", "/DwxUSE_WEBVIEW=0", "/DwxUSE_STC=0",
        "/I`"$WxSrc\include`"",
        "/I`"$IncDir`"",
        "/I`"$WxSrc\src\tiff\libtiff`"",
        "/I`"$WxSrc\src\jpeg`"",
        "/I`"$WxSrc\src\png`"",
        "/I`"$WxSrc\src\zlib`"",
        "/I`"$WxSrc\3rdparty\pcre\src\wx`"",
        "/I`"$WxSrc\src\expat\expat\lib`"",
        "/I`"$WxSrc\3rdparty\nanosvg\src`""
    )

    if (-not (Test-Path "$WxObjDir\dummy.obj")) {
        Log-Msg "[wxmono_pch] Creating wxWidgets precompiled header..."
        $SysIncFlags = if ($IncludePaths) { $IncludePaths | ForEach-Object { "/I`"$($_.TrimEnd('\'))`"" } } else { @() }
        $PchRsp = Join-Path $WxObjDir "pch.rsp"
        $PchLines = @("/nologo", "/c", "/Fo`"$WxObjDir\dummy.obj`"", "/Yc`"wx/wxprec.h`"", "/Fp`"$WxPchFile`"") + $WxCommonFlags + $SysIncFlags + @("`"$WxSrc\src\common\dummy.cpp`"")
        Set-Content -Path $PchRsp -Value ($PchLines -join "`r`n") -Encoding ASCII
        $out = & $ClExe "@$PchRsp" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "[wxmono_pch] failed: $($out | Out-String)" }
    }

    $WxBatchFlags = $WxCommonFlags + @("/Yu`"wx/wxprec.h`"", "/Fp`"$WxPchFile`"")
    Invoke-ClBatch -BatchName "wxmono" -SourceFiles $WxCppFiles -ClFlags $WxBatchFlags -OutputLib "$LibDir\wxmono.lib"
}

# === Call 8, 9, 10: CemuX C++ Sources ===
$CemuObjDir = Join-Path $ObjRoot "cemux"
New-Item -ItemType Directory -Force -Path $CemuObjDir | Out-Null
$CemuPchFile = Join-Path $CemuObjDir "cemu_pch.pch"

$CemuCommonFlags = @(
    "/TP", "/std:c++20", "/utf-8", "/EHsc", "/GR", "/MT", "/O2", "/Ob2", "/Oi", "/Ot", "/GT", "/fp:precise", "/DNDEBUG", "/W0", "/bigobj",
    "/DEMULATOR_VERSION_MAJOR=2", "/DEMULATOR_VERSION_MINOR=6", "/DEMULATOR_VERSION_PATCH=0",
    "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS",
    "/DCURL_STATICLIB", "/DZIP_STATIC", "/DVK_NO_PROTOTYPES", "/DVK_USE_PLATFORM_WIN32_KHR",
    "/DHAS_DIRECTINPUT", "/DENABLE_DISCORD_RPC", "/DHAS_HIDAPI", "/DSUPPORTS_WIIMOTE", "/DHAS_CUBEB=1",
    "/DUNICODE", "/D_UNICODE", "/D__WXMSW__", "/DGLM_ENABLE_EXPERIMENTAL",
    "/I`"$Root\src`"",
    "/I`"$Root\src\Cafe`"",
    "/I`"$Root\src\gui`"",
    "/I`"$Root\src\input`"",
    "/I`"$Root\src\Common`"",
    "/I`"$Root\src\Cemu`"",
    "/I`"$Root\src\config`"",
    "/I`"$Root\src\audio`"",
    "/I`"$Root\src\util`"",
    "/I`"$Root\src\imgui`"",
    "/I`"$Root\src\resource`"",
    "/I`"$IncDir`"",
    "/I`"$IncDir\SDL2`"",
    "/I`"$IncDir\libusb-1.0`"",
    "/I`"$FastSrc\hidapi-hidapi-0.14.0\hidapi`"",
    "/I`"$IncDir\glslang`"",
    "/I`"$Root\dependencies\Vulkan-Headers\include`"",
    "/I`"$Root\dependencies\ih264d\common`"",
    "/I`"$Root\dependencies\ih264d\decoder`"",
    "/I`"$Root\dependencies\ZArchive\include`"",
    "/I`"$Root\dependencies\cubeb\include`"",
    "/I`"$Root\dependencies\discord-rpc\include`"",
    "/I`"$Root\dependencies\imgui`""
)

if (-not (Test-Path "$CemuObjDir\precompiled.obj")) {
    Log-Msg "[cemux_pch] Creating CemuX precompiled header..."
    $SysIncFlags = if ($IncludePaths) { $IncludePaths | ForEach-Object { "/I`"$($_.TrimEnd('\'))`"" } } else { @() }
    $CemuPchRsp = Join-Path $CemuObjDir "pch.rsp"
    $CemuPchLines = @("/nologo", "/c", "/Fo`"$CemuObjDir\precompiled.obj`"", "/Yc`"Common/precompiled.h`"", "/FI`"Common/precompiled.h`"", "/Fp`"$CemuPchFile`"") + $CemuCommonFlags + $SysIncFlags + @("`"$Root\src\Common\precompiled.cpp`"")
    Set-Content -Path $CemuPchRsp -Value ($CemuPchLines -join "`r`n") -Encoding ASCII
    $out = & $ClExe "@$CemuPchRsp" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "[cemux_pch] failed: $($out | Out-String)" }
}

# Clean up any non-Windows / ARM64 stub object files
Remove-Item "$ObjRoot\cemux_main\stub.obj", "$ObjRoot\cemux_main\systeminfostub.obj" -Force -ErrorAction SilentlyContinue

# Collect all Cemu .cpp files
$AllSrcCpp = Get-ChildItem "$Root\src\*.cpp" -Recurse | Where-Object {
    $p = $_.FullName
    $p -notmatch '\\unix\\' -and
    $p -notmatch 'ExceptionHandler_stub\.cpp$' -and
    $p -notmatch 'ExceptionHandler_posix\.cpp$' -and
    $p -notmatch 'ELFSymbolTable\.cpp$' -and
    $p -notmatch 'SystemInfo(Linux|Mac|Unix|Stub)\.cpp$' -and
    $p -notmatch '\\asm\\stub\.cpp$' -and
    $p -notmatch 'wxWayland\.cpp$' -and
    $p -notmatch 'L2CapWiimote\.cpp$' -and
    $p -notmatch 'MemMapperUnix\.cpp$' -and
    $p -notmatch 'FiberUnix\.cpp$' -and
    $p -notmatch '\\Metal\\' -and
    $p -notmatch 'precompiled\.cpp$' -and
    $p -notmatch 'CafeDefaultFont\.cpp$' -and
    $p -notmatch 'embedded\\resources\.cpp$'
} | ForEach-Object { $_.FullName }

$CemuPchBatchFlags = $CemuCommonFlags + @("/Yu`"Common/precompiled.h`"", "/FI`"Common/precompiled.h`"", "/Fp`"$CemuPchFile`"")
Invoke-ClBatch -BatchName "cemux_main" -SourceFiles $AllSrcCpp -ClFlags $CemuPchBatchFlags -OutputLib "$LibDir\cemux_main.lib"

# Non-PCH Cemu files (imgui + CafeDefaultFont.cpp + embedded/resources.cpp + OpenSSL ECX OIDs)
$OsslOidsCpp = Join-Path $CemuObjDir "ossl_ecx_oids.cpp"
$OsslOidsCode = @"
extern "C" {
extern const unsigned char ossl_der_oid_id_X25519[5]  = { 0x06, 0x03, 0x2B, 0x65, 0x6E };
extern const unsigned char ossl_der_oid_id_X448[5]    = { 0x06, 0x03, 0x2B, 0x65, 0x6F };
extern const unsigned char ossl_der_oid_id_Ed25519[5] = { 0x06, 0x03, 0x2B, 0x65, 0x70 };
extern const unsigned char ossl_der_oid_id_Ed448[5]   = { 0x06, 0x03, 0x2B, 0x65, 0x71 };
}
"@
Set-Content -Path $OsslOidsCpp -Value $OsslOidsCode -Encoding ASCII
$NonPchCpp = @(
    "$Root\src\resource\CafeDefaultFont.cpp",
    "$Root\src\resource\embedded\resources.cpp",
    "$Root\dependencies\imgui\imgui.cpp",
    "$Root\dependencies\imgui\imgui_draw.cpp",
    "$Root\dependencies\imgui\imgui_tables.cpp",
    "$Root\dependencies\imgui\imgui_widgets.cpp",
    $OsslOidsCpp
)
Invoke-ClBatch -BatchName "cemux_nopch" -SourceFiles $NonPchCpp -ClFlags $CemuPchBatchFlags -OutputLib "$LibDir\cemux_nopch.lib"

# === Call 11: MASM x64util_masm.asm ===
$AsmObj = Join-Path $CemuObjDir "x64util_masm.obj"
if (-not (Test-Path $AsmObj)) {
    Log-Msg "[masm] Assembling x64util_masm.asm..."
    & $Ml64Exe /nologo /c "/Fo$AsmObj" "$Root\src\asm\x64util_masm.asm"
    if ($LASTEXITCODE -ne 0) { throw "ml64.exe failed!" }
}

# === Call 12: RC cemu.rc ===
$ResObj = Join-Path $CemuObjDir "cemu.res"
if (-not (Test-Path $ResObj)) {
    Log-Msg "[rc] Compiling cemu.rc..."
    Push-Location "$Root\src"
    try {
        & $RcExe /nologo "/fo$ResObj" /d EMULATOR_VERSION_MAJOR=2 /d EMULATOR_VERSION_MINOR=6 /d EMULATOR_VERSION_PATCH=0 "/I$Root\src" "$Root\src\resource\cemu.rc"
        if ($LASTEXITCODE -ne 0) { throw "rc.exe failed!" }
    } finally {
        Pop-Location
    }
}

# === Call 13: Link Cemu_release.exe and CemuX.exe ===
$OutExe = "$Root\bin\Cemu_release.exe"
$OutExeX = "$Root\bin\CemuX.exe"
foreach ($exePath in @($OutExe, $OutExeX)) {
    if (Test-Path $exePath) {
        try {
            Remove-Item $exePath -Force -ErrorAction Stop
        } catch {
            Remove-Item "$exePath.old" -Force -ErrorAction SilentlyContinue
            Move-Item $exePath "$exePath.old" -Force -ErrorAction SilentlyContinue
        }
    }
}
Log-Msg "[link] Linking CemuX.exe..."

$MainObjs = Get-ChildItem "$ObjRoot\cemux_main\*.obj", "$ObjRoot\cemux_main\_wrappers\*.obj", "$ObjRoot\cemux_nopch\*.obj" | ForEach-Object { "`"$($_.FullName)`"" }
$BoostAndSslLibs = Get-ChildItem "$LibDir\*.lib" | Where-Object { $_.Name -notmatch "^(cemux_|libboost_)" } | ForEach-Object { "`"$($_.FullName)`"" }
$SysLibs = @(
    "kernel32.lib", "user32.lib", "gdi32.lib", "gdiplus.lib", "msimg32.lib", "comdlg32.lib",
    "winspool.lib", "winmm.lib", "shell32.lib", "shlwapi.lib", "comctl32.lib", "ole32.lib",
    "oleaut32.lib", "uuid.lib", "rpcrt4.lib", "advapi32.lib", "version.lib", "ws2_32.lib",
    "wininet.lib", "winhttp.lib", "imm32.lib", "uxtheme.lib", "setupapi.lib", "cfgmgr32.lib",
    "hid.lib", "bcrypt.lib", "crypt32.lib", "wldap32.lib", "normaliz.lib", "iphlpapi.lib",
    "secur32.lib", "opengl32.lib", "glu32.lib", "dinput8.lib", "dxguid.lib", "dsound.lib",
    "xinput.lib", "avrt.lib", "ksuser.lib", "mfplat.lib", "dbghelp.lib", "dwmapi.lib", "bthprops.lib"
)

$SysLibPathFlags = if ($LibPaths) { $LibPaths | ForEach-Object { "/LIBPATH:`"$($_.TrimEnd('\'))`"" } } else { @() }
$LinkRsp = Join-Path $CemuObjDir "link.rsp"
$LinkLines = @(
    "/NOLOGO",
    "/OUT:`"$OutExe`"",
    "/SUBSYSTEM:WINDOWS",
    "/MACHINE:X64",
    "/INCREMENTAL:NO",
    "/OPT:REF",
    "/OPT:ICF",
    "/MANIFEST:EMBED",
    "/MANIFESTINPUT:`"$Root\dist\windows\cemu.manifest`"",
    "/LIBPATH:`"$LibDir`"",
    "`"$CemuObjDir\precompiled.obj`"",
    "`"$AsmObj`"",
    "`"$ResObj`""
) + $SysLibPathFlags + $MainObjs + $BoostAndSslLibs + $SysLibs

Set-Content -Path $LinkRsp -Value ($LinkLines -join "`r`n") -Encoding ASCII
$linkOut = & $LinkExe "@$LinkRsp" 2>&1
if ($LASTEXITCODE -ne 0) {
    $errText = ($linkOut | Out-String)
    Add-Content -Path $LogFile -Value $errText -Encoding UTF8
    throw "[link] link.exe failed! Output:`n$errText"
}

Copy-Item $OutExe $OutExeX -Force
Log-Msg "=== BUILD SUCCESSFUL! Output: $OutExeX ($([math]::Round((Get-Item $OutExeX).Length / 1MB, 2)) MB) ==="
