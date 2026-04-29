// http_server.cpp — TomoToolNX
// CRITICAL: IMG_Load is NOT thread-safe on Atmosphere.
// HandleImport() queues an ImportJob; the main thread executes it via FinishImport().

#include "http_server.h"
#include "ugc_scanner.h"
#include "texture_processor.h"
#include "backup.h"

#include <switch.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

static void MkdirP(const char* path) { mkdir(path, 0777); }

static Thread      s_thread;
static bool        s_running       = false;
static bool        s_pendingCommit = false;
static int         s_port          = 8080;
static std::string s_ugcPath;
static Mutex       s_mutex;

static constexpr int LOG_RING_SIZE = 64;
static HttpServer::LogEntry s_logRing[LOG_RING_SIZE];
static int   s_logWrite = 0, s_logRead = 0;
static Mutex s_logMutex;

static void SrvLog(const std::string& text, bool isError = false) {
    FILE* f = fopen("/switch/tomodachi-ugc/debug.log", "a");
    if (f) { fprintf(f, "[HTTP %llu] %s%s\n", (unsigned long long)armGetSystemTick(), isError?"ERROR: ":"", text.c_str()); fclose(f); }
    mutexLock(&s_logMutex);
    s_logRing[s_logWrite % LOG_RING_SIZE] = {text, isError};
    s_logWrite++;
    if (s_logWrite - s_logRead > LOG_RING_SIZE) s_logRead = s_logWrite - LOG_RING_SIZE;
    mutexUnlock(&s_logMutex);
}

enum class ImportState { Idle, Queued, InProgress, Done };
static Mutex                 s_importMutex;
static ImportState           s_importState  = ImportState::Idle;
static HttpServer::ImportJob s_importJob;
static std::string           s_importResult;

static const char* HTML_UI = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TomoToolNX</title>
<link rel="preconnect" href="https://db.onlinewebfonts.com">
<style>
@import url(https://db.onlinewebfonts.com/c/c0ac47345094ba750ea41ff69cbf35c3?family=Mikhak+Regular);
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0f0f0f;--surface:#1a1a1a;--surface2:#222;--border:#2e2e2e;
  --text:#e8e8e8;--muted:#666;--accent:#c8a96e;--accent2:#7eb8c8;
  --ok:#6abf7b;--err:#c86a6a;
}
body{font-family:'Mikhak Regular',system-ui,sans-serif;background:var(--bg);color:var(--text);height:100vh;display:flex;flex-direction:column;overflow:hidden;font-size:14px}
header{background:var(--surface);padding:10px 16px;display:flex;align-items:center;gap:10px;border-bottom:1px solid var(--border);flex-shrink:0}
header h1{font-size:15px;font-weight:400;letter-spacing:.08em;color:var(--accent)}
header h1 span{color:var(--muted);font-size:11px;margin-left:6px;letter-spacing:0}
#hdr-count{font-size:11px;color:var(--muted);margin-left:auto}
.preview-area{flex-shrink:0;height:44vh;display:flex;align-items:center;justify-content:center;background:#080808;position:relative;overflow:hidden}
.preview-area img{max-width:100%;max-height:100%;object-fit:contain;display:block;image-rendering:pixelated}
.preview-placeholder{color:#333;font-size:12px;letter-spacing:.1em;text-transform:uppercase}
.checker{background-image:repeating-conic-gradient(#111 0% 25%,#0d0d0d 0% 50%);background-size:16px 16px}
.info-bar{background:var(--surface);padding:5px 16px;font-size:11px;color:var(--muted);border-top:1px solid var(--border);border-bottom:1px solid var(--border);min-height:26px;letter-spacing:.04em;flex-shrink:0}
.list-wrap{flex:1;overflow-y:auto;background:var(--bg)}
.list-wrap::-webkit-scrollbar{width:4px}
.list-wrap::-webkit-scrollbar-thumb{background:var(--border)}
.list-label{padding:7px 16px;font-size:10px;color:var(--muted);letter-spacing:.12em;text-transform:uppercase;border-bottom:1px solid var(--border);position:sticky;top:0;background:var(--bg);z-index:1}
.entry{padding:9px 16px;cursor:pointer;border-bottom:1px solid #181818;display:flex;align-items:center;gap:8px;transition:background .1s}
.entry:hover{background:var(--surface)}
.entry.active{background:var(--surface2);border-left:2px solid var(--accent)}
.entry-name{font-size:13px;flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;letter-spacing:.02em}
.tag{font-size:10px;background:#2a2000;color:#a07830;padding:1px 5px;border-radius:2px;letter-spacing:.04em}
.toolbar{background:var(--surface);padding:9px 16px;display:flex;gap:8px;border-top:1px solid var(--border);align-items:center;flex-shrink:0}
.btn{padding:6px 14px;border:1px solid var(--border);border-radius:3px;font-size:12px;font-family:inherit;letter-spacing:.06em;cursor:pointer;background:var(--surface2);color:var(--text);transition:border-color .15s,color .15s}
.btn:disabled{opacity:.35;cursor:not-allowed}
.btn:hover:not(:disabled){border-color:var(--accent);color:var(--accent)}
.btn-primary{border-color:#3a5a3a;color:var(--ok)}
.btn-primary:hover:not(:disabled){border-color:var(--ok);color:var(--ok)}
.status{font-size:11px;padding:4px 10px;border-radius:2px;flex:1;min-width:0;letter-spacing:.04em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.status.ok{color:var(--ok)}
.status.err{color:var(--err)}
.status.info{color:var(--muted)}
.overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:100;align-items:center;justify-content:center}
.overlay.open{display:flex}
.dialog{background:var(--surface);border:1px solid var(--border);border-radius:4px;padding:24px;max-width:400px;width:90%;text-align:center}
.dialog h2{font-size:14px;font-weight:400;letter-spacing:.06em;margin-bottom:10px;color:var(--accent)}
.dialog p{font-size:12px;color:var(--muted);margin-bottom:20px;line-height:1.6}
.dialog-btns{display:flex;gap:10px;justify-content:center}
.spinner{display:none;width:28px;height:28px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin .7s linear infinite;margin:0 auto 14px}
@keyframes spin{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<header>
  <h1>TomoToolNX <span>tomodachi life ugc editor</span></h1>
  <span id="hdr-count"></span>
</header>
<div class="preview-area checker">
  <div class="preview-placeholder" id="placeholder">no selection</div>
  <img id="preview-img" style="display:none" alt="">
</div>
<div class="info-bar" id="info-bar"></div>
<div class="list-wrap">
  <div class="list-label">textures</div>
  <div id="list"></div>
</div>
<div class="toolbar">
  <button class="btn btn-primary" id="btn-import" disabled onclick="doImport()">import</button>
  <button class="btn" id="btn-export" disabled onclick="doExport()">export</button>
  <button class="btn" onclick="loadList()">refresh</button>
  <div class="status info" id="status"></div>
</div>
<div class="overlay" id="overlay">
  <div class="dialog">
    <div class="spinner" id="spinner"></div>
    <h2 id="dlg-title">confirm</h2>
    <p id="dlg-msg"></p>
    <div class="dialog-btns" id="dlg-btns">
      <button class="btn btn-primary" onclick="modalYes()">yes</button>
      <button class="btn" onclick="modalClose()">cancel</button>
    </div>
  </div>
</div>
<input type="file" id="file-input" accept="image/png,image/jpeg,image/webp" style="display:none" onchange="fileChosen()">
<script>
let entries=[],selected=null,pendingFile=null;
async function loadList(){
  setStatus('loading...','info');
  const d=await(await fetch('/api/list')).json();
  entries=d.entries||[];
  const l=document.getElementById('list');l.innerHTML='';
  entries.forEach(e=>{
    const div=document.createElement('div');
    div.className='entry'+(selected&&selected.stem===e.stem?' active':'');
    div.innerHTML='<span class="entry-name">'+e.stem+'</span>'+(e.hasThumb?'<span class="tag">thumb</span>':'');
    div.onclick=()=>selectEntry(e);l.appendChild(div);
  });
  document.getElementById('hdr-count').textContent=entries.length+' textures';
  setStatus('','info');
}
async function selectEntry(e){
  selected=e;
  document.querySelectorAll('.entry').forEach((el,i)=>el.classList.toggle('active',entries[i].stem===e.stem));
  document.getElementById('btn-import').disabled=false;
  document.getElementById('btn-export').disabled=false;
  document.getElementById('placeholder').style.display='none';
  document.getElementById('preview-img').style.display='none';
  document.getElementById('info-bar').textContent='';
  setStatus('loading '+e.stem+'...','info');
  const img=document.getElementById('preview-img');
  img.onload=()=>{
    img.style.display='block';
    document.getElementById('info-bar').textContent=e.stem+'   '+img.naturalWidth+' x '+img.naturalHeight+(e.hasThumb?'   thumb':'')+(e.hasCanvas?'   canvas':'');
    setStatus('','info');
  };
  img.onerror=()=>{img.style.display='none';document.getElementById('placeholder').style.display='';document.getElementById('placeholder').textContent='decode failed';setStatus('decode error','err');};
  img.src='/api/preview?stem='+encodeURIComponent(e.stem)+'&t='+Date.now();
}
function doExport(){if(!selected)return;const a=document.createElement('a');a.href='/api/export?stem='+encodeURIComponent(selected.stem);a.download=selected.stem+'.png';a.click();setStatus('exported '+selected.stem+'.png','ok');}
function doImport(){if(!selected)return;document.getElementById('file-input').click();}
function fileChosen(){
  const fi=document.getElementById('file-input');if(!fi.files.length)return;pendingFile=fi.files[0];fi.value='';
  if(selected.hasThumb)showDialog('regenerate thumbnail?','regenerate the thumbnail from the imported image?',true);
  else uploadFile(false);
}
function showDialog(title,msg,btns){
  document.getElementById('dlg-title').textContent=title;
  document.getElementById('dlg-msg').textContent=msg;
  document.getElementById('dlg-btns').style.display=btns?'flex':'none';
  document.getElementById('spinner').style.display='none';
  document.getElementById('overlay').classList.add('open');
}
function showSpinner(msg){
  document.getElementById('dlg-title').textContent=msg;
  document.getElementById('dlg-msg').textContent='';
  document.getElementById('dlg-btns').style.display='none';
  document.getElementById('spinner').style.display='block';
  document.getElementById('overlay').classList.add('open');
}
function modalClose(){document.getElementById('overlay').classList.remove('open');pendingFile=null;}
function modalYes(){uploadFile(true);}
async function uploadFile(regenThumb){
  showSpinner('importing...');
  const fd=new FormData();fd.append('file',pendingFile);fd.append('stem',selected.stem);fd.append('regenThumb',regenThumb?'1':'0');
  const d=await(await fetch('/api/import',{method:'POST',body:fd})).json();
  modalClose();
  if(d.ok){setStatus('imported'+(regenThumb?' + thumb':''),'ok');await loadList();const fresh=entries.find(e=>e.stem===selected.stem);if(fresh)selectEntry(fresh);}
  else setStatus('failed: '+d.error,'err');
  pendingFile=null;
}
function setStatus(msg,cls){const el=document.getElementById('status');el.textContent=msg;el.className='status '+cls;}
loadList();
</script>
</body>
</html>
)HTML";

static void SendAll(int fd,const char* data,size_t len){size_t sent=0;while(sent<len){ssize_t n=send(fd,data+sent,len-sent,0);if(n>0)sent+=n;else if(n<0&&errno==EAGAIN)svcSleepThread(1000000ULL);else break;}}
static void SendStr(int fd,const std::string& s){SendAll(fd,s.c_str(),s.size());}
static void Send200(int fd,const std::string& ct,const std::string& body){std::string h="HTTP/1.1 200 OK\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";SendStr(fd,h);SendStr(fd,body);}
static void Send200Bin(int fd,const std::string& ct,const std::vector<uint8_t>& body,const std::string& disp=""){std::string h="HTTP/1.1 200 OK\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nAccess-Control-Allow-Origin: *\r\n";if(!disp.empty())h+="Content-Disposition: "+disp+"\r\n";h+="Connection: close\r\n\r\n";SendStr(fd,h);SendAll(fd,(const char*)body.data(),body.size());}
static void Send404(int fd){SendStr(fd,"HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot found");}
static void Send500(int fd,const std::string& msg){std::string esc;for(char c:msg){if(c=='"')esc+="\\\"";else if(c=='\\')esc+="\\\\";else esc+=c;}std::string body="{\"ok\":false,\"error\":\""+esc+"\"}";std::string h="HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n";SendStr(fd,h);SendStr(fd,body);}

struct Request{std::string method,path,query,body,contentType;size_t contentLength=0;};
static std::string UrlDecode(const std::string& s){std::string out;for(size_t i=0;i<s.size();i++){if(s[i]=='%'&&i+2<s.size()){int v=0;sscanf(s.c_str()+i+1,"%2x",&v);out+=(char)v;i+=2;}else if(s[i]=='+')out+=' ';else out+=s[i];}return out;}
static std::string GetQueryParam(const std::string& q,const std::string& key){std::string search=key+"=";size_t pos=q.find(search);if(pos==std::string::npos)return"";pos+=search.size();size_t end=q.find('&',pos);return UrlDecode(end==std::string::npos?q.substr(pos):q.substr(pos,end-pos));}

static bool ReadRequest(int fd,Request& req){
    std::string raw;char buf[4096];
    while(true){ssize_t n=recv(fd,buf,sizeof(buf)-1,0);if(n<=0)return false;raw.append(buf,n);if(raw.find("\r\n\r\n")!=std::string::npos)break;if(raw.size()>65536)return false;}
    size_t hdrEnd=raw.find("\r\n\r\n");std::string headers=raw.substr(0,hdrEnd);req.body=raw.substr(hdrEnd+4);
    size_t sp1=headers.find(' '),sp2=headers.find(' ',sp1+1);if(sp1==std::string::npos||sp2==std::string::npos)return false;
    req.method=headers.substr(0,sp1);std::string pf=headers.substr(sp1+1,sp2-sp1-1);size_t qp=pf.find('?');
    if(qp!=std::string::npos){req.path=pf.substr(0,qp);req.query=pf.substr(qp+1);}else req.path=pf;
    auto findHdr=[&](const std::string& name){std::string lo=name;for(auto& c:lo)c=tolower(c);size_t pos=0;while((pos=headers.find('\n',pos))!=std::string::npos){pos++;std::string line=headers.substr(pos,headers.find('\n',pos)-pos);std::string ll=line;for(auto& c:ll)c=tolower(c);if(ll.find(lo+":")!=0)continue;size_t vp=line.find(':')+1;while(vp<line.size()&&line[vp]==' ')vp++;std::string val=line.substr(vp);while(!val.empty()&&(val.back()=='\r'||val.back()=='\n'))val.pop_back();return val;}return std::string{};};
    std::string cl=findHdr("content-length");if(!cl.empty())req.contentLength=(size_t)atoll(cl.c_str());
    req.contentType=findHdr("content-type");
    while(req.body.size()<req.contentLength){ssize_t n=recv(fd,buf,std::min(sizeof(buf)-1,req.contentLength-req.body.size()),0);if(n<=0)break;req.body.append(buf,n);}
    return true;
}

struct FormField{std::string name,filename,data;};
static std::vector<FormField> ParseMultipart(const std::string& body,const std::string& ct){
    std::vector<FormField> fields;size_t bp=ct.find("boundary=");if(bp==std::string::npos)return fields;
    std::string delim="--"+ct.substr(bp+9);while(!delim.empty()&&(delim.back()=='\r'||delim.back()=='\n'||delim.back()==' '))delim.pop_back();
    size_t pos=0;
    while(true){
        size_t start=body.find(delim,pos);if(start==std::string::npos)break;start+=delim.size();if(start+2>body.size())break;if(body[start]=='-'&&body[start+1]=='-')break;
        if(body[start]=='\r')start++;if(body[start]=='\n')start++;
        size_t hEnd=body.find("\r\n\r\n",start);if(hEnd==std::string::npos)break;
        std::string ph=body.substr(start,hEnd-start);size_t ds=hEnd+4;size_t de=body.find("\r\n"+delim,ds);if(de==std::string::npos)break;
        FormField f;f.data=body.substr(ds,de-ds);
        size_t cdp=ph.find("Content-Disposition:");
        if(cdp!=std::string::npos){size_t eol=ph.find('\n',cdp);std::string cd=ph.substr(cdp,eol==std::string::npos?std::string::npos:eol-cdp);auto eq=[&](const std::string& key){size_t kp=cd.find(key+"=\"");if(kp==std::string::npos)return std::string{};kp+=key.size()+2;size_t ep=cd.find('"',kp);return ep==std::string::npos?std::string{}:cd.substr(kp,ep-kp);};f.name=eq("name");f.filename=eq("filename");}
        fields.push_back(std::move(f));pos=de+2;
    }
    return fields;
}

static std::vector<uint8_t> EncodePng(const RgbaImage& img){
    std::string tmp="/switch/tomodachi-ugc/.tmp_preview.png";
    SDL_Surface* s=SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(),img.width,img.height,32,img.width*4,0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    if(!s)return{};IMG_SavePNG(s,tmp.c_str());SDL_FreeSurface(s);
    std::vector<uint8_t> out;FILE* f=fopen(tmp.c_str(),"rb");if(f){fseek(f,0,SEEK_END);size_t sz=(size_t)ftell(f);fseek(f,0,SEEK_SET);out.resize(sz);fread(out.data(),1,sz,f);fclose(f);remove(tmp.c_str());}
    return out;
}

static void HandleList(int fd){
    auto entries=UgcScanner::Scan(s_ugcPath);
    SrvLog("WebUI: list ("+std::to_string(entries.size())+" textures)");
    std::string json="{\"entries\":[";
    for(size_t i=0;i<entries.size();i++){if(i)json+=",";json+="{\"stem\":\""+entries[i].stem+"\",\"hasThumb\":"+(entries[i].hasThumb()?"true":"false")+",\"hasCanvas\":"+(entries[i].hasCanvas()?"true":"false")+"}";}
    json+="]}";Send200(fd,"application/json",json);
}
static void HandlePreview(int fd,const std::string& query){
    std::string stem=GetQueryParam(query,"stem");if(stem.empty()){Send404(fd);return;}
    SrvLog("WebUI: preview "+stem);
    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){Send404(fd);return;}
    RgbaImage img;std::string err=TextureProcessor::DecodeFile(found->ugctexPath,img);if(!err.empty()){Send500(fd,err);return;}
    auto png=EncodePng(img);if(png.empty()){Send500(fd,"PNG encode failed");return;}
    Send200Bin(fd,"image/png",png);
}
static void HandleExport(int fd,const std::string& query){
    std::string stem=GetQueryParam(query,"stem");if(stem.empty()){Send404(fd);return;}
    SrvLog("WebUI: export "+stem);
    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){Send404(fd);return;}
    RgbaImage img;std::string err=TextureProcessor::DecodeFile(found->ugctexPath,img);if(!err.empty()){Send500(fd,err);return;}
    auto png=EncodePng(img);if(png.empty()){Send500(fd,"PNG encode failed");return;}
    Send200Bin(fd,"image/png",png,"attachment; filename=\""+stem+".png\"");
}

// HandleImport: does NOT call IMG_Load. Queues job for main thread.
static void HandleImport(int fd,const Request& req){
    auto fields=ParseMultipart(req.body,req.contentType);
    std::string stem,regenThumbStr;std::vector<uint8_t> fileData;std::string fileExt=".png";
    for(auto& f:fields){if(f.name=="stem")stem=f.data;else if(f.name=="regenThumb")regenThumbStr=f.data;else if(f.name=="file"){fileData.assign(f.data.begin(),f.data.end());if(!f.filename.empty()){size_t dot=f.filename.rfind('.');if(dot!=std::string::npos)fileExt=f.filename.substr(dot);}}}
    if(stem.empty()||fileData.empty()){SrvLog("Import: missing stem or file",true);Send500(fd,"Missing stem or file");return;}
    bool regenThumb=(regenThumbStr=="1");
    SrvLog("Import: received '"+stem+"' ("+std::to_string(fileData.size())+" bytes, ext="+fileExt+")");

    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){SrvLog("Import: entry not found: "+stem,true);Send500(fd,"Entry not found");return;}

    MkdirP("/switch/tomodachi-ugc");
    std::string tmpPath="/switch/tomodachi-ugc/.import_tmp"+fileExt;
    SrvLog("Import: writing temp file "+tmpPath);
    {FILE* f=fopen(tmpPath.c_str(),"wb");if(!f){SrvLog("Import: cannot write temp file",true);Send500(fd,"Cannot write temp file");return;}fwrite(fileData.data(),1,fileData.size(),f);fclose(f);}
    SrvLog("Import: temp file written OK ("+std::to_string(fileData.size())+" bytes)");

    SrvLog("Import: backing up original");BackupService::BackupEntry(*found);SrvLog("Import: backup done");

    TextureProcessor::ImportOptions opts;
    opts.pngPath=tmpPath;opts.destStem=found->directory()+"/"+found->stem;
    opts.writeCanvas=found->hasCanvas();opts.writeThumb=regenThumb;opts.noSrgb=false;opts.originalUgctexPath=found->ugctexPath;

    SrvLog("Import: queuing PNG decode on main thread (IMG_Load not thread-safe)");
    {mutexLock(&s_importMutex);s_importJob={opts,tmpPath};s_importState=ImportState::Queued;s_importResult="";mutexUnlock(&s_importMutex);}

    bool timedOut=true;
    for(int i=0;i<30000;i++){
        svcSleepThread(1000000ULL);
        mutexLock(&s_importMutex);bool done=(s_importState==ImportState::Done);mutexUnlock(&s_importMutex);
        if(done){timedOut=false;break;}
        if(i%1000==999)SrvLog("Import: waiting for main thread ("+std::to_string((i+1)/1000)+"s)...");
    }
    if(timedOut){SrvLog("Import: TIMED OUT",true);mutexLock(&s_importMutex);s_importState=ImportState::Idle;mutexUnlock(&s_importMutex);Send500(fd,"Import timed out");return;}

    std::string err;{mutexLock(&s_importMutex);err=s_importResult;s_importState=ImportState::Idle;mutexUnlock(&s_importMutex);}
    if(!err.empty()){SrvLog("Import: FAILED - "+err,true);Send500(fd,err);return;}

    SrvLog("Import: SUCCESS "+stem+(regenThumb?" (+thumb)":""));
    mutexLock(&s_mutex);s_pendingCommit=true;mutexUnlock(&s_mutex);
    Send200(fd,"application/json","{\"ok\":true}");
}

static void HandleConnection(int fd){
    Request req;if(!ReadRequest(fd,req)){close(fd);return;}
    if(req.method=="GET"&&req.path=="/"){SrvLog("WebUI: page loaded");Send200(fd,"text/html; charset=utf-8",std::string(HTML_UI));}
    else if(req.method=="GET"&&req.path=="/api/list")HandleList(fd);
    else if(req.method=="GET"&&req.path=="/api/preview")HandlePreview(fd,req.query);
    else if(req.method=="GET"&&req.path=="/api/export")HandleExport(fd,req.query);
    else if(req.method=="POST"&&req.path=="/api/import")HandleImport(fd,req);
    else Send404(fd);
    close(fd);
}

static void ServerThreadFunc(void*){
    int srv=socket(AF_INET,SOCK_STREAM,0);if(srv<0){s_running=false;return;}
    int yes=1;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(s_port);addr.sin_addr.s_addr=INADDR_ANY;
    if(bind(srv,(sockaddr*)&addr,sizeof(addr))<0||listen(srv,8)<0){close(srv);s_running=false;return;}
    fcntl(srv,F_SETFL,O_NONBLOCK);
    while(s_running){
        fd_set fds;FD_ZERO(&fds);FD_SET(srv,&fds);timeval tv{0,50000};
        if(select(srv+1,&fds,nullptr,nullptr,&tv)<=0)continue;
        sockaddr_in ca{};socklen_t cl=sizeof(ca);int client=accept(srv,(sockaddr*)&ca,&cl);if(client<0)continue;
        fcntl(client,F_SETFL,fcntl(client,F_GETFL,0)&~O_NONBLOCK);
        int nd=1;setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&nd,sizeof(nd));
        timeval rt{5,0};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&rt,sizeof(rt));
        timeval st{10,0};setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&st,sizeof(st));
        HandleConnection(client);
    }
    close(srv);
}

namespace HttpServer {
void Start(int port,const std::string& ugcPath){
    if(s_running)return;s_port=port;s_ugcPath=ugcPath;s_running=true;s_pendingCommit=false;
    mutexInit(&s_mutex);mutexInit(&s_logMutex);mutexInit(&s_importMutex);
    s_logWrite=0;s_logRead=0;s_importState=ImportState::Idle;
    MkdirP("/switch/tomodachi-ugc");
    threadCreate(&s_thread,ServerThreadFunc,nullptr,nullptr,128*1024,0x2C,-2);threadStart(&s_thread);
}
void Stop(){if(!s_running)return;s_running=false;threadWaitForExit(&s_thread);threadClose(&s_thread);}
bool IsRunning(){return s_running;}
bool HasPendingCommit(){mutexLock(&s_mutex);bool v=s_pendingCommit;mutexUnlock(&s_mutex);return v;}
void ClearPendingCommit(){mutexLock(&s_mutex);s_pendingCommit=false;mutexUnlock(&s_mutex);}
bool HasPendingImport(){mutexLock(&s_importMutex);bool v=(s_importState==ImportState::Queued);mutexUnlock(&s_importMutex);return v;}
ImportJob TakePendingImport(){mutexLock(&s_importMutex);ImportJob job=s_importJob;s_importState=ImportState::InProgress;mutexUnlock(&s_importMutex);return job;}
void FinishImport(const std::string& result){mutexLock(&s_importMutex);s_importResult=result;s_importState=ImportState::Done;mutexUnlock(&s_importMutex);}
void DrainLog(std::vector<LogEntry>& out){mutexLock(&s_logMutex);while(s_logRead<s_logWrite)out.push_back(s_logRing[s_logRead++%LOG_RING_SIZE]);mutexUnlock(&s_logMutex);}
} // namespace HttpServer
