$ErrorActionPreference='Stop'
$reg=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters',$true)
try{if([int]$reg.GetValue('UnconfirmedStarts',99) -ne 0){throw 'Content control has not confirmed start'};$reg.Flush();'confirmed_budget_flushed=0';'persistent_policy='+$reg.GetValue('EnableFullWddm')}finally{$reg.Dispose()}
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'now='+(Get-Date).ToString('s')
