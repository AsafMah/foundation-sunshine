# Behaviour of tools/usbip-win2-root.ps1 -Action install/repair (Ensure-RootDevice) on fake SetupAPI records: which devices
# it disables or removes. Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests/tools/test_usbip_win2_root.ps1
param([string]$Script = (Join-Path $PSScriptRoot '..\..\tools\usbip-win2-root.ps1'))
$ErrorActionPreference = 'Stop'
. $Script
$ErrorActionPreference = 'Stop'

$script:FakeRecords = @()
$script:Calls = @()
function Get-DeviceRecords([bool]$AllClasses) { return $script:FakeRecords }
function Close-DeviceRecords($Records) { }
function Set-StringProperty([IntPtr]$Set, $Data, [uint32]$Property, [string]$Value) { }
function Set-DeviceState([IntPtr]$Set, $Data, [uint32]$State) { $script:Calls += "state $($Data.Id) $State" }
function Remove-DeviceRecord($Record) { $script:Calls += "remove $($Record.InstanceId)"; return $true }
function New-RootDevice { throw 'the test never expects a new root device' }

function Fake([string]$Id, [string]$Friendly) {
  [pscustomobject]@{ Set = [IntPtr]::Zero; Data = [pscustomobject]@{ Id = $Id }; InstanceId = $Id; HardwareId = 'ROOT\USBIP_WIN2\UDE'
    Description = 'USBip 3.X Emulated Host Controller'; FriendlyName = $Friendly }
}
$failures = @()
function Expect([string]$Name, [object[]]$Records, [string]$Kept, [string[]]$Calls) {
  $script:FakeRecords = $Records
  $script:Calls = @()
  $root = @(Ensure-RootDevice)[-1]  # it also writes a "Reusing root device" line
  $got = @($script:Calls)
  if ($root.InstanceId -ne $Kept -or ($got -join '|') -ne ($Calls -join '|')) {
    $script:failures += "${Name}: kept $($root.InstanceId) (want $Kept), calls [$($got -join '; ')] (want [$($Calls -join '; ')])"
  }
  else { Write-Output "ok - $Name" }
}

# The usbip-win2 installer's own controller is ROOT\USB\0000; without an ALKAIDUSB node it is the sole match and is reused.
# Disabling it as "legacy" left the native DualSense path with no controller at all.
Expect 'the reused sole controller stays enabled' @((Fake 'ROOT\USB\0000' '')) 'ROOT\USB\0000' @()
Expect 'a reused controller is kept whatever the case of its id' @((Fake 'root\usb\0000' '')) 'root\usb\0000' @()
# With an ALKAIDUSB node present, ROOT\USB\0000 is a duplicate: removed, then disabled as the legacy controller.
Expect 'the legacy controller beside ALKAIDUSB is removed and disabled' @((Fake 'ROOT\USB\0001' 'ALKAIDUSB'), (Fake 'ROOT\USB\0000' '')) 'ROOT\USB\0001' @('remove ROOT\USB\0000', "state ROOT\USB\0000 $DICS_DISABLE")
Expect 'nothing is disabled when there is no legacy controller' @((Fake 'ROOT\USB\0001' 'ALKAIDUSB')) 'ROOT\USB\0001' @()

if ($failures.Count) { $failures | ForEach-Object { Write-Output "FAIL - $_" }; exit 1 }
Write-Output 'usbip-win2 root device selection: PASS'
