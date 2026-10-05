$ErrorActionPreference='Stop'
$d='P:\bc-250\scratch\g0-hosted\dwm035'
$xpath="*[System[Provider[@Name='Microsoft-Windows-DxgKrnl'] and (EventID=184 or EventID=166 or EventID=44 or EventID=171 or EventID=172 or EventID=215)]]"
$events=@(Get-WinEvent -Path "$d\gpu.etl" -Oldest -FilterXPath $xpath -ErrorAction Stop)
$rows=@(foreach($e in $events){
 [xml]$xml=$e.ToXml();$fields=[ordered]@{}
 foreach($item in $xml.Event.EventData.Data){$fields[[string]$item.Name]=[string]$item.'#text'}
 @{utc=$e.TimeCreated.ToUniversalTime().ToString('o');pid=$e.ProcessId;tid=$e.ThreadId;id=$e.Id;fields=$fields;properties=@($e.Properties|ForEach-Object {$_.Value})}
})
$rows | ConvertTo-Json -Depth 8 | Set-Content "$d\present-events.json" -Encoding UTF8
$events|Group-Object Id|Select-Object Name,Count|ConvertTo-Json
