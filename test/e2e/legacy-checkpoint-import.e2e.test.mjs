import { after, before, describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { createServer, request as forwardHttp } from 'node:http'
import { chmod, copyFile, link, mkdir, mkdtemp, readFile, readdir, realpath, rename, rm, stat, symlink, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { DatabaseSync } from 'node:sqlite'
import { gunzipSync, gzipSync } from 'node:zlib'
import { allocPorts, spawnAria2, stopInstance } from './helpers/aria2-process.mjs'
import { Aria2Rpc } from './helpers/rpc-client.mjs'
import { encode } from './helpers/bencode.mjs'
const secret='legacy-fixture-secret', payload=Buffer.alloc(98305,0x63)
const previousBin=process.env.ARIA2_SCHEMA4_BIN || process.env.ARIA2_SCHEMA3_BIN
const previousSchema=process.env.ARIA2_SCHEMA4_BIN ? 4 : 3
const inspect='aria2.inspectLegacyCheckpointV1', importMethod='aria2.importLegacyCheckpointV1', reconcile='aria2.reconcileLegacyCheckpointV1'
let root, server, url, nativeControl, nativePartial, requests=[]
const nativePayload=Buffer.alloc(4194304)
for(let i=0;i<nativePayload.length;i++)nativePayload[i]=i%251
const fixture=new URL('./fixtures/legacy-v1/',import.meta.url)
function sha(b){return createHash('sha256').update(b).digest('hex')}
function control({ torrent=false, bitfield=0x80, partial=true }={}) {
  const bytes=Buffer.alloc((torrent?20:0)+39+(partial?13:0));let p=0
  const u=(n,size)=>{size===8?bytes.writeBigUInt64BE(BigInt(n),p):size===4?bytes.writeUInt32BE(n,p):bytes.writeUInt16BE(n,p);p+=size}
  u(1,2);u(torrent?1:0,4);u(torrent?20:0,4);if(torrent){bytes.fill(0x68,p,p+20);p+=20}
  u(32768,4);u(payload.length,8);u(0,8);u(1,4);bytes[p++]=bitfield;u(partial?1:0,4)
  if(partial){u(1,4);u(32768,4);u(1,4);bytes[p++]=0x80}
  assert.equal(p,bytes.length);return bytes
}
async function until(fn,predicate,timeout=15000){const end=Date.now()+timeout;let value;do{value=await fn();if(predicate(value))return value;await new Promise(r=>setTimeout(r,25))}while(Date.now()<end);assert.fail(`condition timed out: ${JSON.stringify(value)}`)}
async function engine(t,dir,{ sqlite=true, auth=true, bin, extra=[] }={}) {
  await mkdir(dir,{recursive:true});const { rpcPort, listenPort }=allocPorts()
  const proc=await spawnAria2({bin,args:[
    '--no-conf=true','--no-netrc=true','--enable-rpc=true','--rpc-listen-all=false',`--rpc-listen-port=${rpcPort}`,
    ...(auth?[`--rpc-secret=${secret}`]:[]),'--enable-dht=false','--enable-dht6=false','--enable-peer-exchange=false','--bt-enable-lpd=false',
    '--disable-ipv6=true','--interface=127.0.0.1',`--listen-port=${listenPort}`,'--all-proxy=','--max-concurrent-downloads=2','--file-allocation=none','--auto-file-renaming=false','--allow-overwrite=false',
    '--console-log-level=error','--summary-interval=0',`--dir=${dir}`,`--log=${dir}/engine.log`,'--log-level=debug',
    `--save-session=${dir}/session.txt`,'--force-save=true','--save-session-interval=1',`--enable-sqlite3-persistence=${sqlite}`,`--sqlite3-db-path=${dir}/session.db`,
    '--split=1','--max-connection-per-server=1','--max-tries=1','--seed-time=0','--pause=true',...extra,
  ]});t.after(()=>stopInstance(proc));const rpc=new Aria2Rpc({port:rpcPort,secret:auth?secret:''})
  await until(()=>rpc.call('aria2.getVersion').catch(()=>null),Boolean)
  return {rpc,dir,listenPort,stop:signal=>stopInstance(proc,signal)}
}
function db(e,fn){const d=new DatabaseSync(`${e.dir}/session.db`);try{d.exec('PRAGMA busy_timeout=5000');return fn(d)}finally{d.close()}}
function count(e,table){return db(e,d=>d.prepare(`SELECT count(*) AS n FROM ${table}`).get().n)}
async function request(e,name,{ bytes=control(), data=payload, gid='1234567890abcdef', token=`legacy_test_${name}_token` }={}) {
  const target=path.join(e.dir,`${name}.bin`);await writeFile(target,data,{mode:0o600})
  const s=await stat(target,{bigint:true});const observed=await e.rpc.call(inspect,[{controlFile:bytes.toString('base64')}])
  return {token,gid,controlFile:bytes.toString('base64'),controlDigest:sha(bytes),targetPath:target,
    expected:{kind:observed.kind,totalLength:observed.totalLength,pieceLength:observed.pieceLength,infoHash:observed.infoHash},
    files:[{path:target,offset:'0',length:observed.totalLength,identity:{device:String(s.dev),inode:String(s.ino),size:String(s.size),mtimeNs:String(s.mtimeNs),ctimeNs:String(s.ctimeNs)}}]}
}
function query(req){return {token:req.token,targetPath:req.targetPath}}

function trackerInfoHash(rawUrl) {
  const value=/(?:[?&])info_hash=([^&]*)/.exec(rawUrl)?.[1]??''
  const bytes=[]
  for(let i=0;i<value.length;i++) {
    if(value[i]==='%'&&/^[0-9a-f]{2}$/i.test(value.slice(i+1,i+3))) {
      bytes.push(parseInt(value.slice(i+1,i+3),16));i+=2
    } else bytes.push(value[i]==='+'?32:value.charCodeAt(i))
  }
  return Buffer.from(bytes).toString('hex')
}

async function localTracker(t,seeder,downloader,infoHash) {
  const announces=[],rejected=[]
  const compact=Buffer.alloc(6);compact.set([127,0,0,1]);compact.writeUInt16BE(seeder.listenPort,4)
  const tracker=createServer((req,res)=>{
    const parsed=new URL(req.url,'http://127.0.0.1')
    const port=Number(parsed.searchParams.get('port'))
    const valid=req.socket.remoteAddress==='127.0.0.1'&&parsed.pathname==='/announce'&&
      trackerInfoHash(req.url)===infoHash&&[seeder.listenPort,downloader.listenPort].includes(port)
    if(!valid)rejected.push(req.url)
    else announces.push({port,event:parsed.searchParams.get('event')})
    res.end(encode(valid?{interval:1,'min interval':1,complete:1,incomplete:1,
      peers:port===downloader.listenPort?compact:Buffer.alloc(0)}:{'failure reason':'nonlocal or unexpected test peer'}))
  })
  await new Promise(r=>tracker.listen(0,'127.0.0.1',r))
  t.after(async()=>{tracker.closeAllConnections();await new Promise(r=>tracker.close(r))})
  return {url:`http://127.0.0.1:${tracker.address().port}/announce`,announces,rejected}
}

async function btFiles(e,name,files) {
  const target=`${e.dir}/${name}`;await mkdir(target)
  let offset=0
  const entries=[]
  for(const {name:fileName,bytes,length=bytes.length} of files) {
    const file=`${target}/${fileName}`;await writeFile(file,bytes,{mode:0o600})
    const s=await stat(file,{bigint:true})
    entries.push({path:file,offset:String(offset),length:String(length),identity:{
      device:String(s.dev),inode:String(s.ino),size:String(s.size),mtimeNs:String(s.mtimeNs),ctimeNs:String(s.ctimeNs)}})
    offset+=length
  }
  return {target,entries}
}

async function importBt(e,name,controlBytes,files) {
  const {target,entries}=await btFiles(e,name,files)
  const observed=await e.rpc.call(inspect,[{controlFile:controlBytes.toString('base64')}])
  const req={token:`legacy_local_peer_${name}_token`,gid:'fedcba9876543210',targetPath:target,
    controlFile:controlBytes.toString('base64'),controlDigest:sha(controlBytes),
    expected:{kind:observed.kind,totalLength:observed.totalLength,pieceLength:observed.pieceLength,infoHash:observed.infoHash},files:entries}
  assert.equal((await e.rpc.call(importMethod,[req])).status,'created');assert.equal(count(e,'task'),0)
  return req
}

function localBtOptions(e,tracker) {
  return {dir:e.dir,pause:'true','check-integrity':'true','bt-seed-unverified':'false',
    'bt-hash-check-seed':'false','seed-time':'0','seed-ratio':'0','file-allocation':'none',
    'enable-peer-exchange':'false','bt-enable-lpd':'false','bt-exclude-tracker':'*','bt-tracker':tracker.url}
}

async function seedLocalTorrent(e,tracker,torrent,name,files) {
  await btFiles(e,name,files)
  const gid=await e.rpc.addTorrent(torrent.toString('base64'),{...localBtOptions(e,tracker),
    gid:'0123456789abcdef',pause:'false','bt-hash-check-seed':'true','seed-time':'1','seed-ratio':'1000'})
  const total=files.reduce((sum,f)=>sum+f.bytes.length,0)
  const ready=await until(()=>e.rpc.tellStatus(gid),s=>s.status==='error'||
    (s.status==='active'&&s.completedLength===String(total)&&s.seeder==='true'))
  assert.equal(ready.status,'active',ready.errorMessage)
  await until(async()=>tracker.announces.filter(a=>a.port===e.listenPort).length,n=>n>0)
  return gid
}

async function assertPausedWithoutPeerTraffic(e,seeder,seedGid,tracker,req,torrent,selection) {
  await e.rpc.addTorrent(torrent.toString('base64'),{...localBtOptions(e,tracker),gid:req.gid,'select-file':selection})
  const before=await Promise.all(req.files.map(f=>readFile(f.path)))
  const progress=db(e,d=>d.prepare('SELECT * FROM task_progress').get())
  for(let i=0;i<5;i++) {
    const paused=await e.rpc.tellStatus(req.gid);assert.equal(paused.status,'paused')
    assert.equal(paused.connections,'0');assert.equal(paused.uploadLength,'0')
    assert.equal(tracker.announces.filter(a=>a.port===e.listenPort).length,0)
    assert.equal((await seeder.rpc.tellStatus(seedGid)).uploadLength,'0')
    await new Promise(r=>setTimeout(r,100))
  }
  assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'created')
  assert.deepEqual(db(e,d=>d.prepare('SELECT * FROM task_progress').get()),progress)
  assert.deepEqual(await Promise.all(req.files.map(f=>readFile(f.path))),before)
}

// This is a synthetic shared-piece boundary sample, not a snapshot generated
// by Motrix v1.8.19. The actual v1 fixture/provenance above remains unmodified.
function syntheticSharedPieceSample() {
  const pieceLength=16384,first=Buffer.alloc(8192,0x31),second=Buffer.alloc(24576,0x42)
  const logical=Buffer.concat([first,second])
  const pieces=Buffer.concat([0,16384].map(offset=>createHash('sha1').update(logical.subarray(offset,offset+pieceLength)).digest()))
  const info={files:[{length:first.length,path:['neighbor.bin']},{length:second.length,path:['selected.bin']}],
    name:'shared-piece-synthetic','piece length':pieceLength,pieces,private:1}
  const infoHash=createHash('sha1').update(encode(info)).digest()
  const torrent=encode({info})
  const bytes=Buffer.alloc(59);let p=0
  const u=(n,size)=>{size===8?bytes.writeBigUInt64BE(BigInt(n),p):size===4?bytes.writeUInt32BE(n,p):bytes.writeUInt16BE(n,p);p+=size}
  u(1,2);u(1,4);u(20,4);infoHash.copy(bytes,p);p+=20;u(pieceLength,4);u(logical.length,8);u(0,8);u(1,4);bytes[p++]=0x40;u(0,4)
  assert.equal(p,bytes.length)
  return {torrent,control:bytes,infoHash:infoHash.toString('hex'),first,second,logical}
}

async function durableBtRequest(e,name) {
  const controlBytes=await readFile(new URL('fixture-bundle.aria2',fixture))
  const torrent=await readFile(new URL('a16dc78c94ce589ed4666ab32285f2d188edf26f.torrent',fixture))
  const req=await importBt(e,'fixture-bundle',controlBytes,[{name:'first.bin',bytes:Buffer.alloc(16384,1)},
    {name:'second.bin',bytes:Buffer.alloc(16384,2)}])
  const backup=`${root}/backups/${name}`;await mkdir(backup,{recursive:true})
  const metadataFile=`${backup}/${sha(torrent)}.torrent`;await writeFile(metadataFile,torrent,{mode:0o600})
  const legacyMetadata=`${e.dir}/a16dc78c94ce589ed4666ab32285f2d188edf26f.torrent`
  await writeFile(legacyMetadata,Buffer.from('protected legacy metadata bytes'),{mode:0o600})
  return {req,metadataFile,torrent,legacyMetadata,activation:{token:req.token,targetPath:req.targetPath,
    metadataFile,metadataDigest:sha(torrent),metadata:torrent.toString('base64'),options:{gid:req.gid,pause:'true',dir:e.dir,
      'select-file':'1','check-integrity':'true','bt-seed-unverified':'false','bt-hash-check-seed':'false',
      'seed-time':'0','seed-ratio':'0','rpc-save-upload-metadata':'false'}}}
}

async function dropActivationResponse(t,e,activation) {
  // Forward the actual RPC, then close the client socket as soon as the
  // engine sends response headers. The caller sees transport failure even
  // though the engine has already met its durable-before-response contract.
  let complete
  const acknowledged=new Promise(resolve=>complete=resolve)
  const proxy=createServer((req,res)=>{
    const upstream=forwardHttp(e.rpc.url,{method:'POST',headers:{'content-type':'application/json',
      'content-length':req.headers['content-length']}},reply=>{
      const chunks=[];reply.on('data',b=>chunks.push(b));reply.on('end',()=>complete(JSON.parse(Buffer.concat(chunks))))
      res.destroy()
    })
    upstream.on('error',error=>res.destroy(error));req.pipe(upstream)
  })
  await new Promise(r=>proxy.listen(0,'127.0.0.1',r))
  t.after(async()=>{proxy.closeAllConnections();await new Promise(r=>proxy.close(r))})
  const client=new Aria2Rpc({port:proxy.address().port,secret})
  await assert.rejects(client.call('aria2.addLegacyTorrentV1',[activation]),/fetch failed|socket|terminated/)
  const actual=await acknowledged;assert.equal(actual.result,activation.options.gid,JSON.stringify(actual))
}
before(async()=>{
  root=await realpath(await mkdtemp(path.join(tmpdir(),'aria2-legacy-import-')))
  nativeControl=await readFile(new URL('partial.bin.aria2',fixture));nativePartial=gunzipSync(await readFile(new URL('partial.bin.gz',fixture)))
  server=createServer((req,res)=>{requests.push({url:req.url,range:req.headers.range});
    if(req.url==='/chunked'){res.writeHead(200,{'transfer-encoding':'chunked'});res.end(nativePayload);return}
    if(req.url==='/gzip'){const compressed=gzipSync(nativePayload);res.writeHead(200,{'content-encoding':'gzip','content-length':compressed.length});res.end(compressed);return}
    if(req.url==='/zero'){res.writeHead(200,{'content-length':'0'});res.end();return}
    const source=req.url==='/native'?nativePayload:payload;const range=/^bytes=(\d+)-(\d*)$/.exec(req.headers.range??'');let body=source
    if(range){const first=Number(range[1]),last=range[2]?Number(range[2]):source.length-1;body=source.subarray(first,last+1);res.statusCode=206;res.setHeader('content-range',`bytes ${first}-${last}/${source.length}`)}
    res.setHeader('content-length',body.length);res.setHeader('accept-ranges','bytes');res.setHeader('etag','"legacy-test-stable-object"');res.end(body)
  });await new Promise(r=>server.listen(0,'127.0.0.1',r));url=`http://127.0.0.1:${server.address().port}/fixture`
})
after(async()=>{await new Promise(r=>server.close(r));if(process.env.ARIA2_E2E_KEEP_TMP)console.log(`workDir kept: ${root}`);else await rm(root,{recursive:true,force:true})})
describe('legacy checkpoint engine import',()=>{
  it('inspects native bitfields and partial blocks without database, file or network mutation',async t=>{
    const e=await engine(t,`${root}/inspect`);const bytes=control(),originalRequests=requests.length
    const result=await e.rpc.call(inspect,[{controlFile:bytes.toString('base64')}])
    assert.deepEqual(result,{version:'1',format:'aria2-v1',kind:'http',totalLength:'98305',pieceLength:'32768',infoHash:'',uploadLength:'0',completedLength:'49152',controlDigest:sha(bytes),ranges:[{offset:'0',length:'49152'}]})
    assert.equal(count(e,'task_progress'),0);assert.equal(count(e,'legacy_checkpoint_import'),0);assert.equal(count(e,'task'),0);assert.equal(requests.length,originalRequests)
    const bt=await e.rpc.call(inspect,[{controlFile:control({torrent:true}).toString('base64')}]);assert.equal(bt.infoHash,'68'.repeat(20));assert.equal(bt.kind,'bittorrent')
  })
  it('decodes unmodified HTTP and BT controls from the bundled Motrix v1.8.19 engine',async t=>{
    const e=await engine(t,`${root}/real-inspect`);const observed=await e.rpc.call(inspect,[{controlFile:nativeControl.toString('base64')}])
    assert.equal(observed.completedLength,'1638400');assert.equal(observed.totalLength,'4194304');assert.equal(observed.pieceLength,'1048576');assert.equal(observed.controlDigest,'40a6f7572e913ad63f0b2e867f26367e30c0cad754389963cb2f2645088dc7c7')
    assert.deepEqual(observed.ranges,[{offset:'0',length:'1638400'}]);assert.equal(sha(nativePayload),'a117210941a0b00dcb2d8577e680d84b6fa0eaf760d2afc654c953b9859d54fa')
    const bytes=await readFile(new URL('fixture-bundle.aria2',fixture));const bt=await e.rpc.call(inspect,[{controlFile:bytes.toString('base64')}]);assert.equal(bt.kind,'bittorrent');assert.equal(bt.infoHash,'45c7c651e500cc0ceaa544d6fbafcb21b8cd52f8');assert.equal(bt.totalLength,'32768');assert.equal(bt.pieceLength,'16384');assert.equal(bt.completedLength,'0')
  })
  it('fails closed when SQLite persistence or authentication is unavailable',async t=>{
    for(const [name,opts] of [['text',{sqlite:false}],['noauth',{auth:false}]]){
      const e=await engine(t,`${root}/${name}`,opts);assert.ok(!(await e.rpc.call('aria2.getVersion')).enabledFeatures.includes('LegacyCheckpointImportV1'))
      await assert.rejects(e.rpc.call(inspect,[{controlFile:control().toString('base64')}]),/unavailable/)
      assert.ok(!(await e.rpc.call('aria2.getVersion')).enabledFeatures.includes('LegacyTorrentMetadataV1'))
      await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[{}]),/unavailable/)
    }
  })
  it('commits checkpoint+receipt atomically, rejects changed inputs and reconciles across save/restart',async t=>{
    const dir=`${root}/receipt`;let e=await engine(t,dir);const req=await request(e,'receipt');const beforeRequests=requests.length
    assert.deepEqual(await e.rpc.call(reconcile,[query(req)]),{version:'1',status:'absent',...query(req)})
    db(e,d=>d.exec("CREATE TRIGGER reject_receipt BEFORE INSERT ON legacy_checkpoint_import BEGIN SELECT RAISE(ABORT,'injected'); END"))
    await assert.rejects(e.rpc.call(importMethod,[req]),/write failed/);assert.equal(count(e,'task_progress'),0);assert.equal(count(e,'legacy_checkpoint_import'),0)
    db(e,d=>d.exec('DROP TRIGGER reject_receipt'))
    const receipt=await e.rpc.call(importMethod,[req]);assert.equal(receipt.status,'created');assert.equal(count(e,'task'),0)
    assert.deepEqual(await e.rpc.call(importMethod,[req]),receipt);assert.deepEqual(await e.rpc.call(reconcile,[query(req)]),receipt)
    await assert.rejects(e.rpc.call(importMethod,[{...req,gid:'1111111111111111'}]),/input conflict/)
    await assert.rejects(e.rpc.call(importMethod,[{...req,token:'unrelated_checkpoint_token'}]),/conflict/)
    await e.rpc.call('aria2.saveSession');assert.equal(count(e,'task_progress'),1);assert.equal(requests.length,beforeRequests)
    await e.stop('SIGKILL');e=await engine(t,dir);assert.deepEqual(await e.rpc.call(reconcile,[query(req)]),receipt)
    db(e,d=>d.prepare('DELETE FROM task_progress WHERE out_path=?').run(req.targetPath))
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed');assert.equal((await e.rpc.call(importMethod,[req])).status,'consumed');assert.equal(count(e,'task_progress'),0)
  })
  it('rejects traversal, symlinks, hardlinks, identity changes and unowned writable files',async t=>{
    const e=await engine(t,`${root}/paths`),req=await request(e,'safe')
    const variants=[{...req,targetPath:`${e.dir}/../paths/safe.bin`},{...req,controlDigest:'00'.repeat(32)},
      {...req,files:[{...req.files[0],identity:{...req.files[0].identity,size:'900719925474099999999'}}]},
      {...req,expected:{...req.expected,totalLength:'98306'}}]
    for(const bad of variants)await assert.rejects(e.rpc.call(importMethod,[bad]))
    const sym=`${e.dir}/alias.bin`;await symlink(req.targetPath,sym);await assert.rejects(e.rpc.call(importMethod,[{...req,targetPath:sym,files:[{...req.files[0],path:sym}]}]),/safe path/)
    await link(req.targetPath,`${e.dir}/hard.bin`);await assert.rejects(e.rpc.call(importMethod,[req]),/exclusively owned/);await rm(`${e.dir}/hard.bin`)
    await chmod(req.targetPath,0o666);await assert.rejects(e.rpc.call(importMethod,[req]),/exclusively owned/);await chmod(req.targetPath,0o600)
    await rename(req.targetPath,`${req.targetPath}.old`);await writeFile(req.targetPath,payload,{mode:0o600});await assert.rejects(e.rpc.call(importMethod,[req]),/identity changed/)
    assert.equal(count(e,'task_progress'),0)
  })
  it('rejects malformed codec input, noncanonical base64 and range claims beyond payload length',async t=>{
    const e=await engine(t,`${root}/malformed`),bytes=control()
    for(const b of [bytes.subarray(0,bytes.length-1),Buffer.concat([bytes,Buffer.from([0])]),Buffer.from(bytes)]){
      if(b.length===bytes.length)b[1]=0
      await assert.rejects(e.rpc.call(inspect,[{controlFile:b.toString('base64')}]))
    }
    await assert.rejects(e.rpc.call(inspect,[{controlFile:bytes.toString('base64')+'\n'}]))
    const req=await request(e,'short');await writeFile(req.targetPath,Buffer.alloc(1));const s=await stat(req.targetPath,{bigint:true})
    req.files[0].identity={device:String(s.dev),inode:String(s.ino),size:String(s.size),mtimeNs:String(s.mtimeNs),ctimeNs:String(s.ctimeNs)}
    await assert.rejects(e.rpc.call(importMethod,[req]),/ranges exceed/)
  })
  it('rejects waiting task GID/path conflicts',async t=>{
    const e=await engine(t,`${root}/conflicts`),req=await request(e,'waiting')
    await e.rpc.call('aria2.addUri',[[url],{gid:req.gid,pause:'true',dir:e.dir,out:'waiting.bin'}])
    await assert.rejects(e.rpc.call(importMethod,[req]),/already owned|conflict/)
    await assert.rejects(e.rpc.call(importMethod,[{...req,gid:'1111111111111111'}]),/already owned|conflict/)
    assert.equal(count(e,'legacy_checkpoint_import'),0)
  })
  it('keeps consumed reconciliation monotonic when a deleted checkpoint is recreated',async t=>{
    const e=await engine(t,`${root}/monotonic`),req=await request(e,'monotonic');await e.rpc.call(importMethod,[req])
    db(e,d=>d.exec('CREATE TABLE archived_progress AS SELECT * FROM task_progress; DELETE FROM task_progress'))
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed')
    db(e,d=>d.exec('INSERT INTO task_progress SELECT * FROM archived_progress'))
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed');assert.equal((await e.rpc.call(importMethod,[req])).status,'consumed')
    assert.equal(db(e,d=>d.prepare('SELECT consumed FROM legacy_checkpoint_import').get().consumed),2)
  })
  it('invalidates missing payload state and allows a fresh task after restart pruning',async t=>{
    const dir=`${root}/missing`;let e=await engine(t,dir);const req=await request(e,'missing');await e.rpc.call(importMethod,[req]);await rm(req.targetPath);await e.rpc.call('aria2.saveSession')
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed');await e.stop();e=await engine(t,dir);assert.equal(count(e,'task_progress'),0)
    const gid=await e.rpc.call('aria2.addUri',[[url],{gid:'1111111111111111',pause:'true',dir:e.dir,out:'missing.bin'}]);await e.rpc.unpause(gid)
    const done=await until(()=>e.rpc.tellStatus(gid),s=>['complete','error'].includes(s.status));assert.equal(done.status,'complete',done.errorMessage);assert.equal(sha(await readFile(req.targetPath)),sha(payload));assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed')
  })
  it('preserves original progress and payload on native GID, piece-length and destructive-open rejection',async t=>{
    for(const [name,options] of [['wrong-gid',{gid:'1111111111111111'}],['wrong-piece',{'piece-length':'2097152'}],['destructive',{'header':['If-Modified-Since: Wed, 01 Jan 2020 00:00:00 GMT'],'allow-overwrite':'true'}],['control-removal',{'remove-control-file':'true','allow-overwrite':'true'}]]){
      const e=await engine(t,`${root}/${name}`),req=await request(e,name,{bytes:nativeControl,data:nativePartial});await e.rpc.call(importMethod,[req])
      const original=db(e,d=>d.prepare('SELECT * FROM task_progress').get()),before=await readFile(req.targetPath)
      const gid=await e.rpc.call('aria2.addUri',[[url.replace('/fixture','/native')],{gid:req.gid,pause:'true',dir:e.dir,out:`${name}.bin`,'piece-length':'1048576',...options}]);await e.rpc.unpause(gid)
      const terminal=await until(()=>e.rpc.tellStatus(gid),s=>['error','complete'].includes(s.status));assert.equal(terminal.status,'error',name);assert.deepEqual(await readFile(req.targetPath),before);assert.deepEqual(db(e,d=>d.prepare('SELECT * FROM task_progress').get()),original)
      assert.match(await readFile(`${e.dir}/engine.log`,'utf8'),/metadata or GID mismatch|refuses destructive/)
    }
  })
  it('preserves imported payload for chunked, gzip and zero-length HTTP restart paths',async t=>{
    for(const route of ['chunked','gzip','zero']){
      const e=await engine(t,`${root}/${route}`),req=await request(e,route,{bytes:nativeControl,data:nativePartial});await e.rpc.call(importMethod,[req]);const original=db(e,d=>d.prepare('SELECT * FROM task_progress').get())
      await e.rpc.call('aria2.addUri',[[url.replace('/fixture',`/${route}`)],{gid:req.gid,pause:'true',dir:e.dir,out:`${route}.bin`,'piece-length':'1048576','allow-overwrite':'true','http-accept-gzip':'true'}]);await e.rpc.unpause(req.gid);await until(()=>e.rpc.tellStatus(req.gid),s=>s.status==='error')
      assert.deepEqual(await readFile(req.targetPath),nativePartial);assert.deepEqual(db(e,d=>d.prepare('SELECT * FROM task_progress').get()),original);assert.match(await readFile(`${e.dir}/engine.log`,'utf8'),/refuses destructive payload restart/)
    }
  })
  it('rejects a symlink replacement before any writable payload handle',async t=>{
    const e=await engine(t,`${root}/symlink-restore`),req=await request(e,'symlink',{bytes:nativeControl,data:nativePartial});await e.rpc.call(importMethod,[req])
    const original=db(e,d=>d.prepare('SELECT * FROM task_progress').get()),victim=`${e.dir}/unrelated.bin`,bytes=Buffer.from('protected unrelated bytes');await writeFile(victim,bytes,{mode:0o600});await rename(req.targetPath,`${req.targetPath}.old`);await symlink(victim,req.targetPath)
    await e.rpc.call('aria2.addUri',[[url.replace('/fixture','/native')],{gid:req.gid,pause:'true',dir:e.dir,out:'symlink.bin','piece-length':'1048576','file-allocation':'trunc'}]);await e.rpc.unpause(req.gid);await until(()=>e.rpc.tellStatus(req.gid),s=>s.status==='error')
    assert.deepEqual(await readFile(victim),bytes);assert.deepEqual(await readFile(`${req.targetPath}.old`),nativePartial);assert.deepEqual(db(e,d=>d.prepare('SELECT * FROM task_progress').get()),original)
    assert.match(await readFile(`${e.dir}/engine.log`,'utf8'),/safe path open failed/)
  })
  it('restores a real multi-file BT checkpoint with selection and verified existing pieces',async t=>{
    const e=await engine(t,`${root}/bt-restore`),target=`${e.dir}/fixture-bundle`;await mkdir(target)
    const entries=[]
    for(const [name,value,offset] of [['first.bin',1,0],['second.bin',2,16384]]){
      const file=`${target}/${name}`;await writeFile(file,Buffer.alloc(16384,value),{mode:0o600});const s=await stat(file,{bigint:true})
      entries.push({path:file,offset:String(offset),length:'16384',identity:{device:String(s.dev),inode:String(s.ino),size:String(s.size),mtimeNs:String(s.mtimeNs),ctimeNs:String(s.ctimeNs)}})
    }
    const bytes=await readFile(new URL('fixture-bundle.aria2',fixture)),torrent=await readFile(new URL('a16dc78c94ce589ed4666ab32285f2d188edf26f.torrent',fixture))
    const req={token:'legacy_bt_native_receipt_token',gid:'fedcba9876543210',targetPath:target,controlFile:bytes.toString('base64'),controlDigest:sha(bytes),expected:{kind:'bittorrent',totalLength:'32768',pieceLength:'16384',infoHash:'45c7c651e500cc0ceaa544d6fbafcb21b8cd52f8'},files:entries}
    const beforeRequests=requests.length;await e.rpc.call(importMethod,[req]);assert.equal(count(e,'task'),0)
    await e.rpc.addTorrent(torrent.toString('base64'),{gid:req.gid,pause:'true',dir:e.dir,'select-file':'1','check-integrity':'true','bt-seed-unverified':'false','seed-time':'0','seed-ratio':'0'})
    const paused=await e.rpc.tellStatus(req.gid);assert.equal(paused.status,'paused');assert.equal(paused.files[0].selected,'true');assert.equal(paused.files[1].selected,'false');assert.equal(requests.length,beforeRequests)
    await e.rpc.unpause(req.gid);const done=await until(()=>e.rpc.tellStatus(req.gid),s=>['complete','error'].includes(s.status));assert.equal(done.status,'complete',done.errorMessage);assert.equal(done.completedLength,'16384');assert.equal(done.uploadLength,'0');assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed')
    assert.equal(sha(await readFile(entries[0].path)),sha(Buffer.alloc(16384,1)));assert.equal(sha(await readFile(entries[1].path)),sha(Buffer.alloc(16384,2)))
  })
  it('records the ordinary unsaved-metadata paused BT restart failure without inventing a recoverable task',async t=>{
    const dir=`${root}/bt-undurable`;let e=await engine(t,dir,{extra:['--save-session-interval=0']})
    const {req,torrent}=await durableBtRequest(e,'undurable')
    await e.rpc.addTorrent(torrent.toString('base64'),{gid:req.gid,pause:'true',dir:e.dir,
      'select-file':'1','rpc-save-upload-metadata':'false','check-integrity':'true'})
    assert.equal((await e.rpc.tellStatus(req.gid)).status,'paused');assert.equal(count(e,'task'),0)
    await e.stop('SIGKILL');e=await engine(t,dir,{extra:['--save-session-interval=0']})
    await assert.rejects(e.rpc.tellStatus(req.gid),/not found/)
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'created')
    t.diagnostic('baseline reproduced: ordinary addTorrent + rpc-save-upload-metadata=false loses paused task on restart')
  })
  it('atomically binds a read-only metadata reference and restores a lost paused RPC across shutdown and SIGKILL',async t=>{
    for(const signal of ['SIGTERM','SIGKILL']) {
      const dir=`${root}/bt-durable-${signal}`;let e=await engine(t,dir,{extra:['--save-session-interval=0']})
      const state=await durableBtRequest(e,signal),before=await readFile(state.legacyMetadata)
      assert.ok((await e.rpc.call('aria2.getVersion')).enabledFeatures.includes('LegacyTorrentMetadataV1'))
      await dropActivationResponse(t,e,state.activation)
      assert.equal(count(e,'task'),1);assert.equal(count(e,'legacy_torrent_metadata'),1)
      assert.equal((await e.rpc.tellStatus(state.req.gid)).status,'paused')
      await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[state.activation]),/already bound/)
      await e.stop(signal);e=await engine(t,dir,{extra:['--save-session-interval=0']})
      const paused=await e.rpc.tellStatus(state.req.gid)
      assert.equal(paused.status,'paused');assert.equal(paused.gid,state.req.gid)
      assert.equal(paused.files[0].selected,'true');assert.equal(paused.files[1].selected,'false')
      assert.equal(paused.files[0].path,state.req.files[0].path)
      assert.equal((await e.rpc.call(reconcile,[query(state.req)])).status,'created')
      await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[state.activation]),/already bound/)
      assert.deepEqual(await readFile(state.legacyMetadata),before);assert.deepEqual(await readFile(state.metadataFile),state.torrent)
      await e.rpc.unpause(state.req.gid)
      const done=await until(()=>e.rpc.tellStatus(state.req.gid),s=>['complete','error'].includes(s.status))
      assert.equal(done.status,'complete',done.errorMessage);assert.equal(done.completedLength,'16384')
      assert.equal((await e.rpc.call(reconcile,[query(state.req)])).status,'consumed')
      assert.equal(sha(await readFile(state.req.files[0].path)),sha(Buffer.alloc(16384,1)))
      assert.deepEqual(await readFile(state.legacyMetadata),before)
    }
  })
  it('rejects replaced, changed, missing and symlink metadata after restart without ordinary-loader fallback',async t=>{
    for(const variant of ['replaced','changed','missing','symlink']) {
      const dir=`${root}/bt-metadata-${variant}`;let e=await engine(t,dir,{extra:['--save-session-interval=0']})
      const state=await durableBtRequest(e,variant)
      await e.rpc.call('aria2.addLegacyTorrentV1',[state.activation])
      const progress=db(e,d=>d.prepare('SELECT * FROM task_progress').get())
      const payloadBefore=await Promise.all(state.req.files.map(f=>readFile(f.path)))
      await e.stop('SIGKILL')
      if(variant==='changed')await writeFile(state.metadataFile,Buffer.from('changed metadata bytes'),{mode:0o600})
      else {
        await rename(state.metadataFile,`${state.metadataFile}.old`)
        if(variant==='replaced')await writeFile(state.metadataFile,state.torrent,{mode:0o600})
        if(variant==='symlink')await symlink(`${state.metadataFile}.old`,state.metadataFile)
      }
      e=await engine(t,dir,{extra:['--save-session-interval=0']})
      await assert.rejects(e.rpc.tellStatus(state.req.gid),/not found/)
      assert.equal((await e.rpc.call(reconcile,[query(state.req)])).status,'consumed')
      assert.equal(count(e,'task'),1);assert.equal(db(e,d=>d.prepare('SELECT invalidated FROM legacy_torrent_metadata').get().invalidated),1)
      await e.rpc.call('aria2.saveSession')
      assert.equal(count(e,'task'),1);assert.deepEqual(db(e,d=>d.prepare('SELECT * FROM task_progress').get()),progress)
      assert.deepEqual(await Promise.all(state.req.files.map(f=>readFile(f.path))),payloadBefore)
      if(variant==='symlink'||variant==='replaced')await rm(state.metadataFile)
      if(variant==='changed')await writeFile(state.metadataFile,state.torrent,{mode:0o600})
      else await rename(`${state.metadataFile}.old`,state.metadataFile)
      await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[state.activation]),/intact created receipt/)
      await e.stop();e=await engine(t,dir,{extra:['--save-session-interval=0']})
      await assert.rejects(e.rpc.tellStatus(state.req.gid),/not found/)
      assert.equal((await e.rpc.call(reconcile,[query(state.req)])).status,'consumed')
    }
  })
  it('rolls back metadata grants with a failed task insert and rejects mismatched metadata bytes',async t=>{
    const e=await engine(t,`${root}/bt-metadata-atomic`),state=await durableBtRequest(e,'atomic')
    for(const activation of [{...state.activation,metadataDigest:'00'.repeat(32)},
      {...state.activation,metadata:Buffer.from('different bytes').toString('base64')},
      {...state.activation,options:{...state.activation.options,pause:'false'}}]) {
      await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[activation]))
    }
    db(e,d=>d.prepare("INSERT INTO task(gid,state,serialized,queue_position,digest,created_at,updated_at) VALUES(?,'paused','unrelated durable row',0,X'01',1,1)").run(state.req.gid))
    await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[state.activation]),/durable task GID conflict/)
    assert.equal(db(e,d=>d.prepare('SELECT serialized FROM task WHERE gid=?').get(state.req.gid)).serialized,'unrelated durable row')
    assert.equal(count(e,'legacy_torrent_metadata'),0)
    db(e,d=>d.prepare('DELETE FROM task WHERE gid=?').run(state.req.gid))
    db(e,d=>d.exec("CREATE TRIGGER reject_task BEFORE INSERT ON task BEGIN SELECT RAISE(ABORT,'injected durable task rejection'); END"))
    await assert.rejects(e.rpc.call('aria2.addLegacyTorrentV1',[state.activation]),/UPSERT task failed/)
    assert.equal(count(e,'task'),0);assert.equal(count(e,'legacy_torrent_metadata'),0)
    await assert.rejects(e.rpc.tellStatus(state.req.gid),/not found/)
    assert.equal((await e.rpc.call(reconcile,[query(state.req)])).status,'created')
    db(e,d=>d.exec('DROP TRIGGER reject_task'))
    assert.equal(await e.rpc.call('aria2.addLegacyTorrentV1',[state.activation]),state.req.gid)
  })
  it('requires the new runtime capability and makes the previous binary refuse schema5',{skip:!previousBin},async t=>{
    const old=await engine(t,`${root}/bt-old-capability`,{bin:previousBin})
    assert.ok(!(await old.rpc.call('aria2.getVersion')).enabledFeatures.includes('LegacyTorrentMetadataV1'))
    await assert.rejects(old.rpc.call('aria2.addLegacyTorrentV1',[{}]),/No such method|not found/)
    await old.stop()
    const current=await engine(t,`${root}/bt-schema5`),state=await durableBtRequest(current,'downgrade')
    await current.rpc.call('aria2.addLegacyTorrentV1',[state.activation]);await current.stop()
    const durableBefore=db(current,d=>d.prepare('SELECT * FROM task').all())
    const receiptsBefore=db(current,d=>d.prepare('SELECT * FROM legacy_checkpoint_import').all())
    const proc=await spawnAria2({bin:previousBin,args:['--no-conf=true','--no-netrc=true',
      '--enable-rpc=false','--enable-dht=false','--enable-sqlite3-persistence=true',`--sqlite3-db-path=${current.dir}/session.db`]})
    let output='';proc.stdout.on('data',b=>output+=b);proc.stderr.on('data',b=>output+=b)
    t.after(()=>stopInstance(proc))
    const code=await new Promise(r=>proc.once('exit',r));assert.notEqual(code,0)
    assert.match(output,/schema version 5 is newer/s)
    assert.ok(output.includes(`supports (${previousSchema})`),output)
    assert.deepEqual(db(current,d=>d.prepare('SELECT * FROM task').all()),durableBefore)
    assert.deepEqual(db(current,d=>d.prepare('SELECT * FROM legacy_checkpoint_import').all()),receiptsBefore)
    t.diagnostic(`schema${previousSchema} engine refused newer metadata grants without deleting tasks or receipts`)
  })
  for(const version of [3,4]) it(`upgrades schema${version} with a complete WAL snapshot and preserves paused GIDs`,async t=>{
    const dir=`${root}/upgrade-v${version}`;let e=await engine(t,dir)
    const gid='abcdef0123456789'
    await e.rpc.call('aria2.addUri',[[url],{gid,pause:'true',dir,out:'upgrade.bin'}])
    await e.stop()
    const writer=new DatabaseSync(`${dir}/session.db`)
    t.after(()=>writer.close())
    writer.exec(`PRAGMA wal_autocheckpoint=0; DROP TABLE legacy_torrent_metadata; ${version===3?'DROP TABLE legacy_checkpoint_file; DROP TABLE legacy_checkpoint_import;':''}
      CREATE TABLE upgrade_sentinel(value TEXT); INSERT INTO upgrade_sentinel VALUES('committed WAL');
      UPDATE meta SET value='${version}' WHERE key='schema_version'; PRAGMA user_version=${version};`)
    const tasks=writer.prepare('SELECT * FROM task').all()
    assert.ok((await stat(`${dir}/session.db-wal`)).size>0)
    const beforeRequests=requests.length
    e=await engine(t,dir)
    assert.equal((await e.rpc.tellStatus(gid)).status,'paused')
    assert.equal(requests.length,beforeRequests)
    const backups=(await readdir(dir)).filter(name=>name.startsWith(`session.db.pre-schema5-v${version}.`)&&name.endsWith('.db'))
    assert.equal(backups.length,1)
    const backup=new DatabaseSync(`${dir}/${backups[0]}`,{readOnly:true})
    try {
      assert.equal(backup.prepare('PRAGMA user_version').get().user_version,version)
      assert.equal(backup.prepare('PRAGMA quick_check').get().quick_check,'ok')
      assert.equal(backup.prepare('PRAGMA journal_mode').get().journal_mode,'delete')
      assert.equal(backup.prepare('SELECT value FROM upgrade_sentinel').get().value,'committed WAL')
      assert.deepEqual(backup.prepare('SELECT * FROM task').all(),tasks)
    } finally {backup.close()}
    await e.stop();e=await engine(t,dir)
    assert.equal((await e.rpc.tellStatus(gid)).status,'paused')
    assert.equal((await readdir(dir)).filter(name=>name.startsWith('session.db.pre-schema5-')).length,1)
  })
  it('fills missing native v1 BT pieces from an isolated loopback aria2 seeder after explicit activation',{timeout:60000},async t=>{
    const e=await engine(t,`${root}/bt-peer-native/download`),seeder=await engine(t,`${root}/bt-peer-native/seed`)
    const bytes=await readFile(new URL('fixture-bundle.aria2',fixture)),torrent=await readFile(new URL('a16dc78c94ce589ed4666ab32285f2d188edf26f.torrent',fixture))
    assert.equal(sha(bytes),'9eca779c68d5814eea38b1f4b2c7d73e528a88993f8ade862ca8db90adc5b7eb')
    assert.equal(sha(torrent),'76dc89394c6ebc46b5f863e526e037e3f3da37f0ee3e5a7069d73017bc90f10b')
    const tracker=await localTracker(t,seeder,e,'45c7c651e500cc0ceaa544d6fbafcb21b8cd52f8')
    const full=[{name:'first.bin',bytes:Buffer.alloc(16384,1)},{name:'second.bin',bytes:Buffer.alloc(16384,2)}]
    const seedGid=await seedLocalTorrent(seeder,tracker,torrent,'fixture-bundle',full)
    // The control/torrent are unmodified real v1 snapshots. This shorter
    // payload prefix is constructed from their known data recipe to force
    // peer recovery; it is not claimed to be the original saved BT payload.
    const req=await importBt(e,'fixture-bundle',bytes,[{...full[0],bytes:full[0].bytes.subarray(0,8192),length:16384},full[1]])
    const httpRequests=requests.length
    await assertPausedWithoutPeerTraffic(e,seeder,seedGid,tracker,req,torrent,'1')
    assert.equal((await e.rpc.tellStatus(req.gid)).files[1].selected,'false')
    await e.rpc.unpause(req.gid)
    const done=await until(()=>e.rpc.tellStatus(req.gid),s=>['complete','error'].includes(s.status),45000)
    assert.equal(done.status,'complete',done.errorMessage);assert.equal(done.completedLength,'16384')
    assert.equal(done.files[1].selected,'false')
    assert.equal(sha(await readFile(req.files[0].path)),sha(full[0].bytes))
    assert.equal(sha(await readFile(req.files[1].path)),sha(full[1].bytes))
    assert.ok(Number((await seeder.rpc.tellStatus(seedGid)).uploadLength)>=16384,'the real local seeder supplied the missing piece')
    assert.ok(tracker.announces.some(a=>a.port===e.listenPort))
    assert.deepEqual(tracker.rejected,[]);assert.equal(requests.length,httpRequests,'no HTTP/web-seed fallback')
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed')
    t.diagnostic(`native v1 missing-piece recovery: downloader uploadLength=${done.uploadLength}; no zero-upload guarantee`)
  })
  it('repairs an unselected adjacent file when a synthetic selected file shares its missing BT piece',{timeout:60000},async t=>{
    const sample=syntheticSharedPieceSample()
    const e=await engine(t,`${root}/bt-peer-shared/download`),seeder=await engine(t,`${root}/bt-peer-shared/seed`)
    const tracker=await localTracker(t,seeder,e,sample.infoHash)
    const full=[{name:'neighbor.bin',bytes:sample.first},{name:'selected.bin',bytes:sample.second}]
    const seedGid=await seedLocalTorrent(seeder,tracker,sample.torrent,'shared-piece-synthetic',full)
    const partial=Buffer.concat([Buffer.alloc(8192),sample.second.subarray(8192)])
    const req=await importBt(e,'shared-piece-synthetic',sample.control,[{name:'neighbor.bin',bytes:Buffer.alloc(8192)},
      {name:'selected.bin',bytes:partial}])
    const inspection=await e.rpc.call(inspect,[{controlFile:req.controlFile}])
    assert.equal(inspection.completedLength,'16384');assert.deepEqual(inspection.ranges,[{offset:'16384',length:'16384'}])
    const neighborBefore=await readFile(req.files[0].path),httpRequests=requests.length
    await assertPausedWithoutPeerTraffic(e,seeder,seedGid,tracker,req,sample.torrent,'2')
    const paused=await e.rpc.tellStatus(req.gid)
    assert.equal(paused.files[0].selected,'false');assert.equal(paused.files[1].selected,'true')
    await e.rpc.unpause(req.gid)
    const done=await until(()=>e.rpc.tellStatus(req.gid),s=>['complete','error'].includes(s.status),45000)
    assert.equal(done.status,'complete',done.errorMessage)
    // Native task progress rounds the selection to complete pieces, so it
    // includes the adjacent 8 KiB although only the 24 KiB file is selected.
    assert.equal(done.totalLength,'32768');assert.equal(done.completedLength,'32768')
    assert.equal(done.files[1].length,'24576');assert.equal(done.files[1].completedLength,'24576')
    assert.equal(done.files[0].selected,'false');assert.equal(done.files[1].selected,'true')
    const neighbor=await readFile(req.files[0].path),selected=await readFile(req.files[1].path)
    assert.notDeepEqual(neighbor,neighborBefore,'native BT writes the unselected bytes needed for the shared piece')
    assert.equal(sha(neighbor),sha(sample.first));assert.equal(sha(selected),sha(sample.second))
    assert.equal(sha(Buffer.concat([neighbor,selected])),sha(sample.logical))
    assert.ok(Number((await seeder.rpc.tellStatus(seedGid)).uploadLength)>=16384,'a real peer supplied shared piece zero')
    assert.ok(tracker.announces.some(a=>a.port===e.listenPort))
    assert.deepEqual(tracker.rejected,[]);assert.equal(requests.length,httpRequests,'no HTTP/web-seed fallback')
    assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed')
    t.diagnostic(`synthetic shared-piece recovery: unselected neighbor changed; downloader uploadLength=${done.uploadLength}`)
  })
  it('restores native partial blocks under the reserved GID only after explicit unpause',async t=>{
    const e=await engine(t,`${root}/restore`),req=await request(e,'restore',{bytes:nativeControl,data:nativePartial});const beforeRequests=requests.length
    await e.rpc.call(importMethod,[req]);await e.rpc.call('aria2.addUri',[[url.replace('/fixture','/native')],{gid:req.gid,pause:'true',dir:e.dir,out:'restore.bin','piece-length':'1048576','file-allocation':'none'}])
    assert.equal((await e.rpc.tellStatus(req.gid)).status,'paused');assert.equal(requests.length,beforeRequests);assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'created')
    await e.rpc.unpause(req.gid);const result=await until(()=>e.rpc.tellStatus(req.gid),s=>['complete','error'].includes(s.status))
    assert.equal(result.status,'complete',result.errorMessage);assert.equal(sha(await readFile(req.targetPath)),sha(nativePayload));assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed')
    assert.ok(requests.slice(beforeRequests).some(r=>r.range==='bytes=1638400-4194303'),JSON.stringify(requests.slice(beforeRequests)))
    assert.equal(count(e,'task_progress'),1);assert.equal((await e.rpc.call(importMethod,[req])).status,'consumed')
  })
  it('rechecks imported payload identity before native restoration',async t=>{
    const e=await engine(t,`${root}/changed`),req=await request(e,'changed',{bytes:nativeControl,data:nativePartial});await e.rpc.call(importMethod,[req]);await rename(req.targetPath,`${req.targetPath}.old`);await writeFile(req.targetPath,payload,{mode:0o600})
    await e.rpc.call('aria2.addUri',[[url.replace('/fixture','/native')],{gid:req.gid,pause:'true',dir:e.dir,out:'changed.bin','piece-length':'1048576'}]);const beforeRequests=requests.length
    await e.rpc.unpause(req.gid);const result=await until(()=>e.rpc.tellStatus(req.gid),s=>s.status==='error');assert.equal(result.status,'error');assert.match(await readFile(`${e.dir}/engine.log`,'utf8'),/payload changed before restoration/)
    // HTTP metadata acquisition may occur before native file restoration, but
    // identity rejection prevents writable payload handles and continuation.
    assert.equal(sha(await readFile(req.targetPath)),sha(payload));assert.equal((await e.rpc.call(reconcile,[query(req)])).status,'consumed');assert.ok(requests.length>=beforeRequests)
  })
})
