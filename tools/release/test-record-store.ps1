# In-memory registry-shaped fixture. No host registry API is called.
$script:FixtureKeys=@{}
function New-RecordFixtureKey([string]$Path='') {
    if(-not $script:FixtureKeys.ContainsKey($Path)){$script:FixtureKeys[$Path]=@{}}
    $key=[pscustomobject]@{Path=$Path;Values=$script:FixtureKeys[$Path]}
    $key | Add-Member ScriptMethod GetValue {param($name,$default=$null) if($this.Values.ContainsKey($name)){return $this.Values[$name].value};return $default}
    $key | Add-Member ScriptMethod GetValueKind {param($name) return $this.Values[$name].kind}
    $key | Add-Member ScriptMethod SetValue {param($name,$value,$kind) $this.Values[$name]=@{value=$value;kind=$kind}}
    $key | Add-Member ScriptMethod DeleteValue {param($name,$throw=$true) $this.Values.Remove($name)}
    $key | Add-Member ScriptMethod GetValueNames {return @($this.Values.Keys)}
    $key | Add-Member ScriptMethod CreateSubKey {param($path) $full=if($this.Path){$this.Path+'\'+$path}else{$path};return New-RecordFixtureKey $full}
    $key | Add-Member ScriptMethod OpenSubKey {param($path,$write=$false) $full=if($this.Path){$this.Path+'\'+$path}else{$path};if($script:FixtureKeys.ContainsKey($full)){return New-RecordFixtureKey $full};return $null}
    $key | Add-Member ScriptMethod DeleteSubKeyTree {param($path,$throw=$true) $full=if($this.Path){$this.Path+'\'+$path}else{$path};foreach($k in @($script:FixtureKeys.Keys)){if($k -eq $full -or $k.StartsWith($full+'\')){$script:FixtureKeys.Remove($k)}}}
    $key | Add-Member ScriptMethod Dispose {}
    return $key
}
