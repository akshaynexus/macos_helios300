#!/usr/bin/env python3
"""Compare counters in test-runtime snapshots, excluding the trailing full ioreg dump."""
import argparse,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('logs',nargs='+',type=Path)
p.add_argument('--interval',type=float,default=10,help='nominal seconds between snapshots; default 10')
a=p.parse_args()
if a.interval<=0:p.error('--interval must be positive')
for path in a.logs:
 print('\n'+path.name)
 samples=[]
 for section in re.split(r'== WMI runtime sample \d+/\d+ ==',path.read_text())[1:]:
  values={}
  # First value wins: test-info prints another ioreg after sample 3.
  for key,value in re.findall(r'"(Ath10k[^"\n]+)" = ([^\n]+)',section):
   values.setdefault(key,value.strip().strip('"'))
  samples.append(values)
 if len(samples)<2:print('Insufficient samples');continue
 for key in ['DriverVersion','IRQMode','PLLStatus','PLLStep','PLLReferenceHz','ManagementAttempts','SpectrumRequests','SpectrumCompleted','SpectrumDropped','RuntimeIRQWorker','RuntimeMSIWorker','PayloadCache','RxDeliveryBatchEnabled','RxReserveEnabled','RxRateValid','RxRateMCS','RxRateNSS','RxRateWidthMHz','RxRateShortGI','ConnectionPHY','ConnectionPHYReason','DataTxCapacity','AMPDULimit','AMSDULimit','APVHTPresent','APVHTBasicMap','BAActive','BAAdded','BADeleted','RxSecurityLastReason','RxSecurityFC','RxSecurityHTTPeer','RxSecurityMSDUFlags','RxSecurityNativeHeaderValid']:
  print(key+': '+samples[-1].get('Ath10k'+key,'not captured'))
 print('Interval | RX Mbps* | TX Mbps* | TX stalls | CE mean ms | HTT mean ms | RX drops')
 for i,(before,after) in enumerate(zip(samples,samples[1:]),1):
  def delta(key):
   try:return int(after['Ath10k'+key])-int(before['Ath10k'+key])
   except (KeyError,ValueError):return None
  def rate(key):
   n=delta(key);return f'{n*8/a.interval/1e6:.2f}' if n is not None and n>=0 else 'N/A'
  def avg(prefix):
   n=delta(prefix+'TotalMs');d=delta(prefix+'Count')
   return f'{n/d:.2f}' if n is not None and d is not None and n>=0 and d>0 else 'N/A'
  print(f'{i}->{i+1} | {rate("DataRxBytes")} | {rate("DataTxBytes")} | {delta("DataTxStalls")} | {avg("DataTxCEObserved")} | {avg("DataTxHTTObserved")} | {delta("DataRxDropped")}')
  for prefix in ['RuntimeWorker','RxWorker','RxDelivery']:
   n=delta(prefix+'TotalNS');count=delta(prefix+'Count')
   if n is not None and count is not None and n>=0 and count>0:
    print(f'  {prefix} mean host us/call: {n/count/1000:.2f}')
  for key in ['RuntimeIRQWorkers','SpectrumRequests','SpectrumCompleted','SpectrumDropped','RxReserveMisses','RxReserveFailures','RxDeliveryFlushes','RxDirectBuffers','RxAssemblyCopies','DataTxWakeups','RxRateInvalid','RxDropSecurity','RxDropReplay','RxDropDecap','RxSecurityHeaderType','RxSecurityDirection',
              'RxSecurityProtected','RxSecurityMoreFragments','RxSecurityFragmentNumber',
              'RxSecurityTransmitter','RxSecurityMixedCipher']:
   n=delta(key)
   if n is not None and n>0:print(f'  {key}: +{n}')
 print('* Driver byte counters / nominal interval: not a speed-test result or PHY rate.')
