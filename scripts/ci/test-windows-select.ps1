# Give the isolated Windows runner a non-link-local IPv6 address so aria2
# issues AAAA queries even on workers with only IPv4 internet connectivity.
# The fixture itself uses only ::1 and 127.0.0.0/8; no DNS settings change.
$ErrorActionPreference = 'Stop'
$index = (Get-NetIPAddress -AddressFamily IPv6 -IPAddress '::1' | Select-Object -First 1).InterfaceIndex
$testIp = New-NetIPAddress -InterfaceIndex $index -IPAddress 'fd42:2421::1' `
    -PrefixLength 128 -PolicyStore ActiveStore -SkipAsSource $true
try {
    $env:ARIA2_TEST_DUAL_STACK = '1'
    node --test test/e2e/select-dns.e2e.test.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Windows select DNS or connection failover regression' }
} finally {
    $testIp | Remove-NetIPAddress -Confirm:$false
}
