param([switch]$BundleFfmpeg, [string]$Version, [string]$ContentId, [string]$FfmpegLicense)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'

$publishArgs = @()
if ($Version) { $publishArgs += "-p:Version=$Version" }
if ($ContentId) { $publishArgs += "-p:ContentId=$ContentId" }

$libusbVersion = '1.0.27'
$libusbArchiveHash = '19835e290f46fab6bd8ce4be6ab7dc5209f1c04bad177065df485e51dc4118c8'
$libusbDllHash = 'd4e5db4fad8bef7201dc5a4a71ca997e6dccc9da25e8a988d4be63b13a02208d'
$libusbLicenseHash = '5df07007198989c622f5d41de8d703e7bef3d0e79d62e24332ee739a452af62a'
$cache = Join-Path (Join-Path $root 'build\cache') "libusb-$libusbVersion"
$libusbDll = Join-Path $cache 'libusb-1.0.dll'
$libusbLicense = Join-Path $cache 'LIBUSB-LICENSE.txt'

function Test-Hash($path, $hash) {
    (Test-Path $path) -and ((Get-FileHash $path -Algorithm SHA256).Hash -eq $hash)
}

if (-not ((Test-Hash $libusbDll $libusbDllHash) -and (Test-Hash $libusbLicense $libusbLicenseHash))) {
    $work = Join-Path $cache 'work'
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $work | Out-Null
    $archive = Join-Path $work 'libusb.7z'
    Invoke-WebRequest "https://github.com/libusb/libusb/releases/download/v$libusbVersion/libusb-$libusbVersion.7z" -OutFile $archive
    if (-not (Test-Hash $archive $libusbArchiveHash)) { throw "libusb archive checksum mismatch" }
    $extract = Join-Path $work 'x'
    New-Item -ItemType Directory -Force $extract | Out-Null
    tar -xf $archive -C $extract
    if ($LASTEXITCODE -ne 0) { throw "could not extract the libusb archive" }
    Copy-Item (Join-Path $extract 'VS2022\MS64\dll\libusb-1.0.dll') $libusbDll -Force
    Invoke-WebRequest "https://raw.githubusercontent.com/libusb/libusb/v$libusbVersion/COPYING" -OutFile $libusbLicense
    Remove-Item $work -Recurse -Force
    if (-not (Test-Hash $libusbDll $libusbDllHash)) { throw "libusb dll checksum mismatch" }
    if (-not (Test-Hash $libusbLicense $libusbLicenseHash)) { throw "libusb license checksum mismatch" }
}
$publishArgs += "-p:LibusbPath=$libusbDll"
$publishArgs += "-p:LibusbLicensePath=$libusbLicense"

if ($BundleFfmpeg) {
    $ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
    if (-not $ffmpeg) { throw "ffmpeg not found in PATH (winget install Gyan.FFmpeg)" }
    $publishArgs += "-p:FfmpegPath=$ffmpeg"
    if (-not $FfmpegLicense) {
        foreach ($name in 'FFMPEG-LICENSE.txt', 'LICENSE.txt', 'LICENSE') {
            $candidate = Join-Path (Split-Path $ffmpeg) $name
            if (Test-Path $candidate) { $FfmpegLicense = $candidate; break }
        }
    }
    if ($FfmpegLicense) { $publishArgs += "-p:FfmpegLicensePath=$FfmpegLicense" }
    Write-Host "Bundling ffmpeg from $ffmpeg"
}

dotnet publish (Join-Path $root 'NXStream\NXStream.csproj') -c Release -o $dist -nologo @publishArgs
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed" }

Write-Host "`nOK -> $dist\NXStream.exe"
