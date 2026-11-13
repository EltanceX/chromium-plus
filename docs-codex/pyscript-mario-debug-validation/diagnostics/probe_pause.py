import ctypes,json,re,subprocess,time
from pathlib import Path
from ctypes import wintypes
root=Path('out/Debug/pyscript-mario-debug-stability')
out=root/'pause-active-before.json'
log=Path(str(out)+'.chrome.log')
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenThread.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
k.OpenThread.restype=wintypes.HANDLE
for name in ('SuspendThread','ResumeThread'):
    fn=getattr(k,name); fn.argtypes=[wintypes.HANDLE]; fn.restype=wintypes.DWORD
k.CloseHandle.argtypes=[wintypes.HANDLE]
events=[]
with (root/'pause-active-before.log').open('w',encoding='utf-8') as driver:
    child=subprocess.Popen(['node','chrome/test/pyscript_mario_smoke_test.mjs','--chrome-binary=out/Debug/chrome.exe','--case-filter=full-level-route','--output='+str(out)],stdout=driver,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+70
        match=None
        while child.poll() is None and time.monotonic()<deadline:
            if log.exists():
                data=log.read_text(encoding='utf-8',errors='replace')
                match=re.search(r'\[(\d+):(\d+):[^\n]*ORACLE 1 [^\n]*\n[^\n]*full-level-route',data)
                if match: break
            time.sleep(.01)
        assert match, 'No full-level-route renderer log found'
        browser_pid,_=map(int,match.groups())
        command = '$p = Get-CimInstance Win32_Process -Filter "ParentProcessId=' + str(browser_pid) + '" | Where-Object { $_.CommandLine -match "--type=renderer" }; @($p | ForEach-Object { $proc = Get-Process -Id $_.ProcessId; [pscustomobject]@{ pid=$_.ProcessId; tid=($proc.Threads | Sort-Object StartTime | Select-Object -First 1).Id; cpu=$proc.TotalProcessorTime.TotalMilliseconds; commandLine=$_.CommandLine } }) | ConvertTo-Json -Compress'
        processes=json.loads(subprocess.check_output(['pwsh','-NoProfile','-Command',command],text=True,encoding='utf-8'))
        if isinstance(processes,dict): processes=[processes]
        renderer=max((p for p in processes if '--top-chrome-webui' not in p['commandLine']),key=lambda p:p['cpu'])
        pid,tid=renderer['pid'],renderer['tid']
        (root/'pause-active-processes.json').write_text(json.dumps(processes,indent=2)+'\n',encoding='utf-8')
        handle=k.OpenThread(2,False,tid)
        assert handle, ctypes.get_last_error()
        try:
            for i in range(150):
                if child.poll() is not None: break
                if 'python-budget' in log.read_text(encoding='utf-8',errors='replace'): break
                previous=k.SuspendThread(handle)
                if previous==0xffffffff: break
                try:
                    time.sleep(.020)
                finally:
                    assert k.ResumeThread(handle)!=0xffffffff
                events.append(dict(index=i,pid=pid,tid=tid,previousSuspendCount=previous,pauseMs=20))
                time.sleep(.001+(i%7)*.0003)
        finally:
            k.CloseHandle(handle)
        child.wait(timeout=100)
    finally:
        if child.poll() is None:
            child.terminate(); child.wait(timeout=10)
        (root/'pause-active-injection.json').write_text(json.dumps(dict(description='Controlled suspension of only the isolated test renderer main thread; each suspend paired with resume in finally.',events=events,driverExitCode=child.returncode),indent=2)+'\n',encoding='utf-8')
print('injections',len(events),'exit',child.returncode)
