# Downloads the official Streamline production runtime used by this release.
[CmdletBinding()]
param([string]$Archive, [string]$Destination = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$version = '2.14.16'
$url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip"
$archiveHash = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
$members = @('bin/x64/NvLowLatencyVk.dll','bin/x64/nvngx_dlss.dll','bin/x64/nvngx_dlssg.dll',
  'bin/x64/sl.common.dll','bin/x64/sl.dlss_g.dll','bin/x64/sl.dlss.dll','bin/x64/sl.interposer.dll',
  'bin/x64/sl.pcl.dll','bin/x64/sl.reflex.dll','license.txt','bin/x64/nvngx_dlss.license.txt',
  'bin/x64/reflex.license.txt','3rd-party-licenses.md')
$root = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Destination).Path)
$id = [Guid]::NewGuid().ToString('N')
$stage = Join-Path $root ("streamline-install-" + $id + ".partial")
$target = Join-Path $root 'streamline'
$previous = Join-Path $root ("streamline-backup-" + $id)
$download = $null
$lock = $null
$zip = $null
try {
  $lock = [IO.File]::Open((Join-Path $root '.streamline-install.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
  if (-not $Archive) {
    $download = Join-Path ([IO.Path]::GetTempPath()) ("lichii-streamline-" + $id + ".zip")
    Write-Host "Downloading the Streamline $version SDK from NVIDIA (276 MB)."
    $c = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($c) {
      & $c.Source --fail --location --retry 2 --output $download $url
      if ($LASTEXITCODE) { throw "Download failed (curl $LASTEXITCODE)." }
    } else {
      [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
      Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $download
    }
    $Archive = $download
  }
  if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash -ne $archiveHash) {
    throw "The archive does not match NVIDIA's published SHA-256 for Streamline $version."
  }
  [void][IO.Directory]::CreateDirectory($stage)
  $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Archive).Path)
  foreach ($member in $members) {
    $entry = $zip.GetEntry($member)
    if (-not $entry) { throw "Missing SDK member: $member" }
    $file = Join-Path $stage ([IO.Path]::GetFileName($member))
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $file)
    if ($file.EndsWith('.dll')) {
      $sig = Get-AuthenticodeSignature -LiteralPath $file
      if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch '(?:^|, )O=NVIDIA Corporation(?:,|$)') {
        throw "Windows could not verify NVIDIA's signature on $member."
      }
    }
  }
  $zip.Dispose()
  $zip = $null
  if ([IO.Directory]::Exists($target)) { [IO.Directory]::Move($target, $previous) }
  try { [IO.Directory]::Move($stage, $target) }
  catch {
    if ([IO.Directory]::Exists($previous) -and -not [IO.Directory]::Exists($target)) {
      [IO.Directory]::Move($previous, $target)
    }
    throw
  }
  Write-Host "Installed DLSS Super Resolution, Frame Generation and Reflex in $target"
  if ([IO.Directory]::Exists($previous)) { Write-Host "Previous runtime: $previous" }
} finally {
  if ($zip) { $zip.Dispose() }
  if ($lock) { $lock.Dispose() }
  if ($download -and [IO.File]::Exists($download)) { [IO.File]::Delete($download) }
  if ([IO.Directory]::Exists($stage) -and [IO.Directory]::GetParent($stage).FullName -eq $root) {
    [IO.Directory]::Delete($stage, $true)
  }
}
