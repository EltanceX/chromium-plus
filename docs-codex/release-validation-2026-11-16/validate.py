import datetime, hashlib, json, pathlib, shutil, subprocess, sys, time, traceback
repo=pathlib.Path(r'C:\src\src'); build=repo/'out/Release'; run=pathlib.Path(__file__).resolve().parent
node=shutil.which('node'); python=sys.executable
suites=[
 ('chromium-plus','chrome/test/chromium_plus_smoke_test.py',15,[]),
 ('parser','chrome/test/pyscript_smoke_test.mjs',27,[]),
 ('settings','chrome/test/pyscript_settings_smoke_test.mjs',12,[]),
 ('mario-logic','chrome/test/pyscript_mario_unittest.py',29,[]),
 ('mcbe','chrome/test/mcbe_smoke_test.mjs',17,[]),
 ('mcje','chrome/test/mcje_smoke_test.mjs',33,[]),
 ('dom','chrome/test/pyscript_dom_smoke_test.mjs',17,[]),
 ('events','chrome/test/pyscript_event_smoke_test.mjs',34,[]),
 ('animation','chrome/test/pyscript_animation_smoke_test.mjs',19,[]),
 ('input','chrome/test/pyscript_input_smoke_test.mjs',25,[]),
 ('canvas','chrome/test/pyscript_canvas_smoke_test.mjs',23,[]),
 ('browser-services','chrome/test/pyscript_browser_smoke_test.mjs',15,[]),
 ('mario','chrome/test/pyscript_mario_smoke_test.mjs',35,[]),
 ('mario-hardware','chrome/test/pyscript_mario_smoke_test.mjs',36,['--graphics=default','--soak-seconds=60']),
]
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as s:
  while data:=s.read(2**20):h.update(data)
 return h.hexdigest()
inputs=['chrome.exe','chrome.dll','chrome_elf.dll','args.gn']
record={'startedAt':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sourceCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),'buildDirectory':str(build),'outputDirectory':str(run),'workingTreeStatus':subprocess.check_output(['git','status','--short'],cwd=repo,text=True).strip(),'sourcePatchSha256':hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=repo)).hexdigest(),'nativeUnitTestsRun':False,'state':'running','checks':{},'inputSha256':{name:sha(build/name) for name in inputs},'passed':False}
def save():
 p=run/'summary.tmp';p.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n');p.replace(run/'summary.json')
save()
try:
 for name,script,expected,extra in suites:
  output=run/(name+'.json');command=[node if script.endswith('.mjs') else python,str(repo/script)]
  if name!='mario-logic':command+=['--chrome-binary='+str(build/'chrome.exe')] if script.endswith('.mjs') else ['--chrome-binary',str(build/'chrome.exe')]
  command+=['--output='+str(output)] if script.endswith(('.mjs','unittest.py')) else ['--output',str(output)]
  command+=extra
  record['currentSuite']=name;save();print('Starting '+name,flush=True);start=time.monotonic()
  check={'command':command,'expected':expected}
  try:
   with (run/(name+'.log')).open('wb') as stream:
    process=subprocess.run(command,cwd=repo,stdout=stream,stderr=subprocess.STDOUT,timeout=300,creationflags=subprocess.CREATE_NO_WINDOW)
   check['exitCode']=process.returncode
   if output.exists():
    data=json.loads(output.read_text(encoding='utf-8'))
    cases=data if isinstance(data,list) else data.get('cases',data.get('results',[]))
    check.update(total=len(cases),passed=sum(c.get('passed') is True for c in cases),suitePassed=data.get('passed',True) if isinstance(data,dict) else True)
    if isinstance(data,dict):check['sandboxDisabled']=data.get('sandboxDisabled',False)
   check['verified']=(check.get('exitCode')==0 and check.get('total')==expected and check.get('passed')==expected and check.get('suitePassed') is True and check.get('sandboxDisabled',False) is False)
  except Exception:
   check.update(verified=False,error=traceback.format_exc())
  check['elapsedSeconds']=round(time.monotonic()-start,2);record['checks'][name]=check;save()
  print(json.dumps({'suite':name,**check},ensure_ascii=False),flush=True)
  if name in ('chromium-plus','parser') and not check['verified']:
   raise RuntimeError('Core smoke failed; stop wider validation and inspect '+name)
 record['finalSha256']={name:sha(build/name) for name in inputs}
 record['inputsUnchanged']=record['inputSha256']==record['finalSha256'];record['passed']=record['inputsUnchanged'] and all(c['verified'] for c in record['checks'].values())
 record['state']='succeeded' if record['passed'] else 'failed'
except BaseException:
 record.update(state='failed',error=traceback.format_exc());print(record['error'],flush=True)
finally:
 record['finishedAt']=datetime.datetime.now(datetime.timezone.utc).isoformat();save()
print(json.dumps({'state':record['state'],'passed':record['passed'],'suites':len(record['checks']),'summary':str(run/'summary.json')},ensure_ascii=False),flush=True)
raise SystemExit(0 if record['passed'] else 1)
