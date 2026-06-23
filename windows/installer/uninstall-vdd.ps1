# Best-effort removal of the MttVDD virtual display driver + device on uninstall.
# The app installs hardware ID Root\MttVDD at runtime (Extended mode). This
# script removes the device and the driver package if present. All errors are
# ignored so uninstall succeeds even when the driver was never installed.
$ErrorActionPreference = 'SilentlyContinue'

# 1) Remove the virtual display device instance(s).
Get-PnpDevice | Where-Object { $_.InstanceId -like 'ROOT\MTTVDD*' } | ForEach-Object {
    pnputil /remove-device $_.InstanceId | Out-Null
}

# 2) Delete the driver package (oemXX.inf whose original name is mttvdd.inf).
$published = $null
foreach ($line in (pnputil /enum-drivers)) {
    if ($line -match 'Published Name\s*:\s*(oem\d+\.inf)') { $published = $Matches[1] }
    if ($line -match 'Original Name\s*:\s*mttvdd\.inf' -and $published) {
        pnputil /delete-driver $published /uninstall /force | Out-Null
        $published = $null
    }
}

exit 0
