param(
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9.-]*$')]
    [string]$Address = '10.0.0.1',
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]*$')]
    [string]$RemoteUser = 'root',
    [ValidatePattern('^boot-[0-9]{6,}-[0-9a-f-]+$')]
    [string]$Boot,
    [string]$Destination = (Join-Path $PSScriptRoot '../planning/results')
)

$ErrorActionPreference = 'Stop'
$ssh = (Get-Command ssh.exe -ErrorAction Stop).Source
if (-not $Boot) {
    & $ssh -T "$RemoteUser@$Address" aawg-logs list
    if ($LASTEXITCODE -ne 0) { throw "SSH listing failed ($LASTEXITCODE)" }
    Write-Host 'Copy the truck boot name, then rerun with -Boot boot-...'
    exit 0
}

$folder = [IO.Path]::GetFullPath($Destination)
[IO.Directory]::CreateDirectory($folder) | Out-Null
$target = Join-Path $folder "$Boot.tar.gz"
$partial = "$target.partial"
if ([IO.File]::Exists($target)) { throw "Already downloaded: $target" }

# Copy raw SSH stdout. PowerShell 5 text redirection corrupts binary archives.
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $ssh
$start.Arguments = "-T $RemoteUser@$Address aawg-logs export $Boot"
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$process = New-Object Diagnostics.Process
$process.StartInfo = $start
$output = [IO.File]::Open($partial, [IO.FileMode]::CreateNew)
try {
    if (-not $process.Start()) { throw 'SSH did not start' }
    $process.StandardOutput.BaseStream.CopyTo($output)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "SSH export failed ($($process.ExitCode)); partial file: $partial" }
} finally {
    $output.Dispose()
    $process.Dispose()
}
[IO.File]::Move($partial, $target)
Get-FileHash -Algorithm SHA256 -LiteralPath $target
Write-Host "Downloaded: $target"
