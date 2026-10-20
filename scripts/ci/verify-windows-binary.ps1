param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'ia32', 'arm64')]
    [string]$Architecture,
    [string]$Binary = 'bin/aria2c.exe'
)

$ErrorActionPreference = 'Stop'
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Binary))
if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
    throw 'Candidate is not a Windows executable'
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length -or
    [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x4550) {
    throw 'Candidate has an invalid PE header'
}
$expectedMachine = @{ x64 = 0x8664; ia32 = 0x014c; arm64 = 0xaa64 }
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne $expectedMachine[$Architecture]) {
    throw ('Expected {0} PE machine, found 0x{1:x4}' -f $Architecture, $machine)
}
if ($Architecture -eq 'arm64' -and
    [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'Arm64') {
    throw 'Windows ARM64 release validation requires a native ARM64 runner'
}
Write-Host ('Verified {0} PE machine 0x{1:x4}' -f $Architecture, $machine)
