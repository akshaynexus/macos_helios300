#!/bin/sh
# Capture three snapshots to show that the receiver keeps polling after start().
set -eu
cd "$(dirname "$0")/.."
i=1
while [ "$i" -le 3 ]; do
    echo "== WMI runtime sample $i/3 =="
    date
    ioreg -r -c Ath10kPCIDevice -l -w 0 | \
        awk '/"Ath10k(PLL|IRQ|Spectrum|ManagementAttempts|QoS|WMM|APMaintenance|TXTID|TXAC|TXAdmission|FWTx|FWRetry|FWFail|LinkNoise|LinkRate|LinkTXRate|LinkRXRate|FirmwareStats|PCIeIdle|PCIeTarget|PCIeWake|PCIeRuntime|WMIUnknown|WMILastUnknown|WMITemperature|WMIRFKill|WMIPeerKickout|WMIKeyNotification|WMIHostMemory|Station|DriverVersion|ChannelPolicy|CSA|Regulatory|Power|Prepared|PayloadProfile|BMI|TargetWasAlreadyAwake|Wake|FirmwareHT|FirmwareVHT|FirmwareRF|APDTIM|APLivenessProbe|APHT|APVHT|PeerHT|PeerVHT|LocalHT|LocalVHT|NegotiatedVHT|AMPDU|AMSDU|Aggregation|BA|Disconnect|Security|PortAuthorized|Connection|Ethernet|DataTx|DataRx|PacketDMA|Rx|HTT|WMIRadio|WMIVdevCreate|WMIScan|BSS|WMIEcho|WMIRuntime|WMICredits|WMIInitSucceeded|WMIInitRequestedChainMask|WMIRFChains|PayloadCache|OutputQueue|RuntimeMSI|RuntimeIRQ|RuntimeWorker|RuntimeActive|BootFirmwareCrashIndicated|DMAQuarantine)/'
    if [ "$i" -lt 3 ]; then sleep 10; fi
    i=$((i + 1))
done
sh scripts/test-info.sh

# Read-only network diagnostics; never change another interface's configuration.
echo "== macOS interfaces and DHCP =="
networksetup -listallhardwareports || true
for interface in $(ifconfig -l); do
    case "$interface" in
        en[0-9]*)
            ifconfig "$interface" || true
            ipconfig getpacket "$interface" || true
            ;;
    esac
done
