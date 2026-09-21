// Real local RustFS; random test credentials never appear in argv or artifacts.
const {spawn,spawnSync}=require('node:child_process');
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),net=require('node:net');
const {setTimeout:delay}=require('node:timers/promises');
const root=path.resolve(__dirname,'..'),run=crypto.randomBytes(8).toString('hex');
const dir=path.join(root,'out','storage-rustfs',run);fs.mkdirSync(dir,{recursive:true});
const access='test'+run,secret=crypto.randomBytes(24).toString('hex');
const env={...process.env,RUSTFS_ACCESS_KEY:access,RUSTFS_SECRET_KEY:secret,RUSTFS_CONSOLE_ENABLE:'false',RUST_LOG:'warn',
 VISION_S3_ACCESS:access,VISION_S3_SECRET:secret};
const tool=path.join(root,'out/build/windows-core/apps/storage-tool/Release/vision-storage-tool.exe');
function curl(port,method,key) {
 return spawnSync(path.join(root,'开发环境/SDK/native/bin/curl.exe'),
 ['--config','-','--silent','--fail','--max-time','2','--aws-sigv4','aws:amz:us-east-1:s3','-X',method,`http://127.0.0.1:${port}/${key}`],
 {env,input:`user = "${access}:${secret}"\n`,encoding:'utf8',windowsHide:true,timeout:4000});
}
function execute(request,expected=0,operation='upload') {
 const file=path.join(dir,'request.json');fs.writeFileSync(file,JSON.stringify(request));
 const p=spawnSync(tool,[operation,file],{env,windowsHide:true,encoding:'utf8',timeout:30000});
 if(p.error||p.status!==expected)throw Error(`Storage command unexpected exit ${p.status}: ${p.stderr}`);
 return p.stdout;
}
(async()=>{
 const reservation=net.createServer();await new Promise(r=>reservation.listen(0,'127.0.0.1',r));const port=reservation.address().port;
 await new Promise(r=>reservation.close(r));const data=path.join(dir,'rustfs');fs.mkdirSync(data);
 const child=spawn(path.join(root,'开发环境/SDK/RustFS/rustfs.exe'),['server','--address',`127.0.0.1:${port}`,data],
 {cwd:data,env,windowsHide:true,stdio:['ignore','pipe','pipe']});
 let error;child.on('error',e=>error=e);
 const log=b=>fs.appendFileSync(path.join(dir,'service.log'),b.toString().replaceAll(secret,'[REDACTED]').replaceAll(access,'[REDACTED]'));
 child.stdout.on('data',log);child.stderr.on('data',log);
 try {
  let ready=false;for(let n=0;n<60;n++){if(error)throw error;if(child.exitCode!==null)throw Error('RustFS exited');if(curl(port,'GET','').status===0){ready=true;break;}await delay(200);}
  if(!ready||curl(port,'PUT','inspection-test').status!==0)throw Error('RustFS start/bucket failed');
  const store=path.join(dir,'store');fs.mkdirSync(store);
  const request={root:store,endpoint:`http://127.0.0.1:${port}`,bucket:'inspection-test',key:'images/one.pgm',image_id:'image-1',
    file:path.join(root,'examples/models/black.pgm')};
  const first=JSON.parse(execute(request));if(first.state!=='Available')throw Error('Image not available');
  const again=JSON.parse(execute(request));if(again.hash!==first.hash)throw Error('Idempotency');
  execute({...request,image_id:'image-2',bucket:'missing-bucket'},1);
  execute({...request,image_id:'image-3',endpoint:'http://127.0.0.1:1'},1);
  execute({...request,image_id:'collision'},1);
  if(curl(port,'PUT','missing-bucket').status!==0)throw Error('Recovery bucket');
  const recovery=JSON.parse(execute({root:store},1,'resume-uploads'));
  if(recovery.completed!==1||recovery.failed!==1)throw Error('Persisted upload recovery');
  execute({root:store},1,'resume-uploads'); // Third unavailable attempt terminates.
  const settled=JSON.parse(execute({root:store},0,'resume-uploads'));
  if(settled.completed||settled.failed)throw Error('Upload retry did not terminate');
  console.log('PASS RustFS: actual signed PUT/GET hash, duplicate upload, destination collision, missing bucket, restart resume, bounded retry');
  fs.writeFileSync(path.join(dir,'result.json'),JSON.stringify({passed:true,checks:8,serviceStopped:false}));
 } finally {
  if(child.exitCode===null&&!error){const ended=new Promise(r=>child.once('exit',r));child.kill();await ended;}
  const resultFile=path.join(dir,'result.json');
  if(fs.existsSync(resultFile)){const result=JSON.parse(fs.readFileSync(resultFile));result.serviceStopped=child.exitCode!==null||child.signalCode!==null;fs.writeFileSync(resultFile,JSON.stringify(result));}
 }
})().catch(e=>{console.error(e.message);process.exitCode=1;});
