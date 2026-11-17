param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'out/dependency-sources' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$helper = Join-Path ([IO.Path]::GetTempPath()) ('maskpilot-sources-' + [Guid]::NewGuid().ToString('N') + '.py')
@'
import concurrent.futures, hashlib, json, sys, urllib.request
from pathlib import Path
catalog=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
output=Path(sys.argv[2]);output.mkdir(parents=True,exist_ok=True)
for archive in catalog['archives']:
    target=output/archive['file']
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest()==archive['sha256']:
        print('Verified cached',archive['file'],flush=True);continue
    step=1024*1024;size=archive['size']
    def chunk(index):
        begin=index*step;end=min(size,begin+step)-1
        last=None
        for base in [catalog['mirrorUrl'],catalog['baseUrl']]*2:
            try:
                url=base+archive['file']+'?maskpilot-source-range='+str(index)
                request=urllib.request.Request(url,headers={'Range':f'bytes={begin}-{end}'})
                with urllib.request.urlopen(request,timeout=60) as response:
                    if response.status!=206 or response.headers.get('Content-Range')!=f'bytes {begin}-{end}/{size}':
                        raise ValueError('Source server did not honor the exact Range request')
                    data=response.read(step+1)
                    if len(data)!=end-begin+1:raise ValueError('Source response length mismatch')
                    return data
            except Exception as error:last=error
        raise RuntimeError('Source range download failed') from last
    partial=target.with_suffix(target.suffix+'.partial')
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool, partial.open('wb') as stream:
        for data in pool.map(chunk,range((size+step-1)//step)):stream.write(data)
    if hashlib.sha256(partial.read_bytes()).hexdigest()!=archive['sha256']:raise ValueError('Source archive hash mismatch')
    partial.replace(target);print('Verified',archive['file'],flush=True)
'@ | Set-Content -LiteralPath $helper -Encoding UTF8
try {
    & python $helper "$projectRoot/third_party/sources.json" $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Dependency source acquisition failed.' }
} finally { Remove-Item -LiteralPath $helper -Force }
