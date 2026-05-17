// http_server.cpp — TomoToolNX
// CRITICAL: IMG_Load is NOT thread-safe on Atmosphere.
// HandleImport() queues an ImportJob; the main thread executes it via FinishImport().

#include "http_server.h"
#include "ugc_scanner.h"
#include "texture_processor.h"
#include "backup.h"
#include "mii_manager.h"
#include "save_editor.h"
#include "save_mount.h"
#include "habits_data.h"
#include "wishes_data.h"
#include "island_generator.h"

// Pre-gzipped WebUI HTML/JS bundle. The source-of-truth lives at
// data/webui.html; the Makefile gzips it at build time, devkitPro's bin2s
// embeds the bytes into .rodata, and we serve them verbatim with
// `Content-Encoding: gzip` so the browser decompresses (zero Switch CPU,
// ~340 KB saved on disk vs. shipping the raw HTML).
#include "webui_gz_bin.h"

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
#include <curl/curl.h>

static void MkdirP(const char* path) { mkdir(path, 0777); }
static bool FileExists(const std::string& p){struct stat st;return stat(p.c_str(),&st)==0;}

static Thread      s_thread;
static bool        s_running       = false;
static bool        s_pendingCommit          = false;
static bool        s_pendingMiiRefresh      = false;
static bool        s_pendingPlayerSavReload = false;
static bool        s_pendingMiiSavReload    = false;
static bool        s_pendingMapSavReload    = false;
static bool        s_saveWarnAcked          = false;
static int         s_port          = 8080;
static std::string s_ugcPath;
static Mutex       s_mutex;

static constexpr int LOG_RING_SIZE = 64;
static HttpServer::LogEntry s_logRing[LOG_RING_SIZE];
static int   s_logWrite = 0, s_logRead = 0;
static Mutex s_logMutex;

static void SrvLog(const std::string& text, bool isError = false) {
    FILE* f = fopen("/switch/TomoToolNX/debug.log", "a");
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
static Mutex                   s_bgMutex;
static ImportState             s_bgState  = ImportState::Idle;
static HttpServer::BgRemoveJob s_bgJob;
static std::string             s_bgResult;
static u64                   s_lastConnectTick = 0;

static void SendAll(int fd,const char* data,size_t len){size_t sent=0;while(sent<len){ssize_t n=send(fd,data+sent,len-sent,0);if(n>0)sent+=n;else if(n<0&&errno==EAGAIN)svcSleepThread(1000000ULL);else break;}}
static void SendStr(int fd,const std::string& s){SendAll(fd,s.c_str(),s.size());}
static void Send200(int fd,const std::string& ct,const std::string& body){std::string h="HTTP/1.1 200 OK\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";SendStr(fd,h);SendStr(fd,body);}
static void Send200Bin(int fd,const std::string& ct,const std::vector<uint8_t>& body,const std::string& disp=""){std::string h="HTTP/1.1 200 OK\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nAccess-Control-Allow-Origin: *\r\n";if(!disp.empty())h+="Content-Disposition: "+disp+"\r\n";h+="Connection: close\r\n\r\n";SendStr(fd,h);SendAll(fd,(const char*)body.data(),body.size());}
static void Send200Gzip(int fd,const std::string& ct,const unsigned char* data,size_t len){std::string h="HTTP/1.1 200 OK\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(len)+"\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";SendStr(fd,h);SendAll(fd,(const char*)data,len);}
static void Send404(int fd){SendStr(fd,"HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot found");}
static void Send302(int fd,const std::string& location){std::string h="HTTP/1.1 302 Found\r\nLocation: "+location+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";SendStr(fd,h);}
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
        if(body[start]=='\r')start++;
        if(body[start]=='\n')start++;
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
    std::string tmp="/switch/TomoToolNX/.tmp_preview.png";
    SDL_Surface* s=SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(),img.width,img.height,32,img.width*4,0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    if(!s)return{};
    IMG_SavePNG(s,tmp.c_str());SDL_FreeSurface(s);
    std::vector<uint8_t> out;FILE* f=fopen(tmp.c_str(),"rb");if(f){fseek(f,0,SEEK_END);size_t sz=(size_t)ftell(f);fseek(f,0,SEEK_SET);out.resize(sz);fread(out.data(),1,sz,f);fclose(f);remove(tmp.c_str());}
    return out;
}

static void HandleList(int fd){
    MiiManager::LoadUgcNames();
    auto entries=UgcScanner::Scan(s_ugcPath);
    SrvLog("WebUI: list ("+std::to_string(entries.size())+" textures)");
    std::string json="{\"entries\":[";
    for(size_t i=0;i<entries.size();i++){if(i)json+=",";std::string nm=MiiManager::GetUgcName(entries[i].stem);std::string ne;for(char c:nm){if(c=='"')ne+="\\\"";else if(c=='\\')ne+="\\\\";else ne+=c;}json+="{\"stem\":\""+entries[i].stem+"\",\"name\":\""+ne+"\",\"hasThumb\":"+(entries[i].hasThumb()?"true":"false")+",\"hasCanvas\":"+(entries[i].hasCanvas()?"true":"false")+"}";}
    json+="]}";Send200(fd,"application/json",json);
}
static void HandlePreview(int fd,const std::string& query){
    std::string stem=GetQueryParam(query,"stem");if(stem.empty()){Send404(fd);return;}
    SrvLog("WebUI: preview "+stem);
    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){Send404(fd);return;}
    RgbaImage img;std::string err=TextureProcessor::DecodeFile(found->ugctexPath,img,false);if(!err.empty()){Send500(fd,err);return;}
    auto png=EncodePng(img);if(png.empty()){Send500(fd,"PNG encode failed");return;}
    Send200Bin(fd,"image/png",png);
}
static void HandleExport(int fd,const std::string& query){
    std::string stem=GetQueryParam(query,"stem");if(stem.empty()){Send404(fd);return;}
    SrvLog("WebUI: export "+stem);
    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){Send404(fd);return;}
    RgbaImage img;std::string err=TextureProcessor::DecodeFile(found->ugctexPath,img,false);if(!err.empty()){Send500(fd,err);return;}
    auto png=EncodePng(img);if(png.empty()){Send500(fd,"PNG encode failed");return;}
    Send200Bin(fd,"image/png",png,"attachment; filename=\""+stem+".png\"");
}

// HandleImport: does NOT call IMG_Load. Queues job for main thread.
static void HandleImport(int fd,const Request& req){
    auto fields=ParseMultipart(req.body,req.contentType);
    std::string stem;std::vector<uint8_t> fileData;std::string fileExt=".png";
    std::string encoderStr,bc1ModeStr,fitModeStr,matteStr,shapeStr;
    for(auto& f:fields){
        if(f.name=="stem")stem=f.data;
        else if(f.name=="encoder")encoderStr=f.data;
        else if(f.name=="bc1Mode")bc1ModeStr=f.data;
        else if(f.name=="fitMode")fitModeStr=f.data;
        else if(f.name=="matte")matteStr=f.data;
        else if(f.name=="shape")shapeStr=f.data;
        else if(f.name=="file"){fileData.assign(f.data.begin(),f.data.end());if(!f.filename.empty()){size_t dot=f.filename.rfind('.');if(dot!=std::string::npos)fileExt=f.filename.substr(dot);}}
    }
    if(stem.empty()||fileData.empty()){SrvLog("Import: missing stem or file",true);Send500(fd,"Missing stem or file");return;}
    SrvLog("Import: received '"+stem+"' ("+std::to_string(fileData.size())+" bytes, ext="+fileExt+")");

    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){SrvLog("Import: entry not found: "+stem,true);Send500(fd,"Entry not found");return;}

    MkdirP("/switch/TomoToolNX");
    std::string tmpPath="/switch/TomoToolNX/.import_tmp"+fileExt;
    SrvLog("Import: writing temp file "+tmpPath);
    {FILE* f=fopen(tmpPath.c_str(),"wb");if(!f){SrvLog("Import: cannot write temp file",true);Send500(fd,"Cannot write temp file");return;}fwrite(fileData.data(),1,fileData.size(),f);fclose(f);}
    SrvLog("Import: temp file written OK ("+std::to_string(fileData.size())+" bytes)");

    TextureProcessor::ImportOptions opts;
    opts.pngPath=tmpPath;opts.destStem=found->directory()+"/"+found->stem;
    opts.writeCanvas=found->hasCanvas();opts.writeThumb=found->hasThumb();opts.thumbPath=found->thumbPath;opts.noSrgb=false;opts.originalUgctexPath=found->ugctexPath;opts.originalCanvasPath=found->canvasPath;
    if(encoderStr=="pca") opts.encoder=TextureProcessor::Bc1Encoder::PCA;
    if(bc1ModeStr=="fourColor") opts.bc1Mode=TextureProcessor::Bc1Mode::FourColor;
    else if(bc1ModeStr=="threeColor") opts.bc1Mode=TextureProcessor::Bc1Mode::ThreeColor;
    if(fitModeStr=="contain") opts.fitMode=TextureProcessor::FitMode::Contain;
    else if(fitModeStr=="fill") opts.fitMode=TextureProcessor::FitMode::Fill;
    else opts.fitMode=TextureProcessor::FitMode::Cover;
    // Canvas shape — picks output dimensions when the slot isn't square. The
    // WebUI exposes a Square/Book/TV dropdown next to the existing encoder
    // controls; omitting the field keeps the upstream square-default.
    if(shapeStr=="book")    opts.canvasShape=TextureProcessor::CanvasShape::Book;
    else if(shapeStr=="tv") opts.canvasShape=TextureProcessor::CanvasShape::Tv;
    else                    opts.canvasShape=TextureProcessor::CanvasShape::Square;
    // Matte color comes in as '#RRGGBB' (or empty = transparent). Only used
    // when fit=contain; otherwise the resize ignores it.
    if(!matteStr.empty() && matteStr.size()==7 && matteStr[0]=='#'){
        auto h=[&](int o){int v=0;for(int i=0;i<2;i++){char c=matteStr[o+i];v<<=4;if(c>='0'&&c<='9')v|=c-'0';else if(c>='a'&&c<='f')v|=c-'a'+10;else if(c>='A'&&c<='F')v|=c-'A'+10;}return (uint8_t)v;};
        opts.matte={h(1),h(3),h(5),255};
    }

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

    SrvLog("Import: SUCCESS "+stem+(found->hasThumb()?" (+thumb)":""));
    mutexLock(&s_mutex);s_pendingCommit=true;mutexUnlock(&s_mutex);
    Send200(fd,"application/json","{\"ok\":true}");
}

// POST /api/removebg?stem=X — queues BgRemoveJob for main thread, long-polls for result
static void HandleBgRemove(int fd,const Request& req){
    std::string stem=GetQueryParam(req.query,"stem");
    if(stem.empty()){SrvLog("BgRemove: missing stem",true);Send500(fd,"Missing stem");return;}
    auto entries=UgcScanner::Scan(s_ugcPath);const UgcTextureEntry* found=nullptr;
    for(auto& e:entries)if(e.stem==stem){found=&e;break;}
    if(!found){SrvLog("BgRemove: entry not found: "+stem,true);Send500(fd,"Entry not found");return;}
    TextureProcessor::ImportOptions opts;
    opts.destStem=found->directory()+"/"+found->stem;
    opts.writeCanvas=found->hasCanvas();opts.writeThumb=found->hasThumb();opts.thumbPath=found->thumbPath;opts.noSrgb=false;opts.originalUgctexPath=found->ugctexPath;opts.originalCanvasPath=found->canvasPath;
    {mutexLock(&s_bgMutex);s_bgJob={found->ugctexPath,opts};s_bgState=ImportState::Queued;s_bgResult="";mutexUnlock(&s_bgMutex);}
    SrvLog("BgRemove: job queued for main thread");
    bool timedOut=true;
    for(int i=0;i<120000;i++){
        svcSleepThread(1000000ULL);
        mutexLock(&s_bgMutex);bool done=(s_bgState==ImportState::Done);mutexUnlock(&s_bgMutex);
        if(done){timedOut=false;break;}
        if(i%10000==9999)SrvLog("BgRemove: waiting for main thread ("+std::to_string((i+1)/1000)+"s)...");
    }
    if(timedOut){SrvLog("BgRemove: TIMED OUT",true);mutexLock(&s_bgMutex);s_bgState=ImportState::Idle;mutexUnlock(&s_bgMutex);Send500(fd,"BgRemove timed out");return;}
    std::string err;{mutexLock(&s_bgMutex);err=s_bgResult;s_bgState=ImportState::Idle;mutexUnlock(&s_bgMutex);}
    if(!err.empty()){SrvLog("BgRemove: FAILED - "+err,true);Send500(fd,err);return;}
    SrvLog("BgRemove: SUCCESS "+stem);
    mutexLock(&s_mutex);s_pendingCommit=true;mutexUnlock(&s_mutex);
    Send200(fd,"application/json","{\"ok\":true}");
}

// GET /api/mii/list
// GET /api/habits — returns list of all habits with category and English label.
static void HandleHabitsList(int fd){
    std::string j = "[";
    for (int i = 0; i < HABIT_COUNT; i++) {
        if (i) j += ",";
        std::string label;
        for (const char* p = HABITS[i].label; *p; p++) {
            char c = *p;
            if (c == '"') label += "\\\"";
            else if (c == '\\') label += "\\\\";
            else label += c;
        }
        j += "{\"n\":\""; j += HABITS[i].name;
        j += "\",\"c\":"; j += std::to_string(HABITS[i].category);
        j += ",\"l\":\""; j += label;
        j += "\"}";
    }
    j += "]";
    Send200(fd, "application/json", j);
}

// GET /api/wishes/list — return [{h:hash, n:englishName, c:catIdx}] + categories
static void HandleWishesList(int fd){
    auto jsonEsc = [](const char* p) {
        std::string out;
        for (; *p; p++) {
            char c = *p;
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "";
            else out += c;
        }
        return out;
    };
    std::string j = "{\"categories\":[";
    for (int i = 0; i < WishesData::CATEGORY_COUNT; i++) {
        if (i) j += ",";
        j += "\""; j += jsonEsc(WishesData::CATEGORIES[i]); j += "\"";
    }
    j += "],\"wishes\":[";
    for (int i = 0; i < WishesData::WISH_COUNT; i++) {
        if (i) j += ",";
        const auto& w = WishesData::WISHES[i];
        j += "{\"h\":";
        j += std::to_string((unsigned long)w.hash);
        j += ",\"n\":\"";
        j += jsonEsc(w.name);
        j += "\",\"c\":";
        j += std::to_string((int)w.categoryIdx);
        j += "}";
    }
    j += "]}";
    Send200(fd, "application/json", j);
}

static void HandleMiiList(int fd){
    auto miis = MiiManager::ListMiis();
    SrvLog("WebUI: mii list ("+std::to_string(miis.size())+" miis)");
    std::string json="{\"miis\":[";
    for(size_t i=0;i<miis.size();i++){
        if(i)json+=",";
        // Escape name for JSON
        std::string name;
        for(char c:miis[i].name){
            if(c=='"')name+="\\\"";
            else if(c=='\\')name+="\\\\";
            else name+=c;
        }
        json+="{\"slot\":"+std::to_string(miis[i].slot)+
              ",\"name\":\""+name+"\""+
              ",\"hasFacepaint\":"+(miis[i].hasFacepaint?"true":"false")+"}";
    }
    json+="]}";
    Send200(fd,"application/json",json);
}

// GET /api/mii/export?slot=N
static void HandleMiiExport(int fd,const std::string& query){
    std::string slotStr=GetQueryParam(query,"slot");
    if(slotStr.empty()){Send500(fd,"Missing slot");return;}
    int slot=atoi(slotStr.c_str());
    SrvLog("WebUI: mii export slot "+std::to_string(slot));

    // Export to a temp path then serve the bytes
    std::string tmpPath="/switch/TomoToolNX/.mii_export_tmp.ltd";
    std::string err=MiiManager::ExportMii(slot,tmpPath);
    // ExportMii appends .ltd and may uniquify — find the actual file
    if(!err.empty()){SrvLog("Mii export failed: "+err,true);Send500(fd,err);return;}

    // Find the actual written file (may have been uniquified)
    std::string actualPath=tmpPath;
    if(!FileExists(actualPath)){
        // Try with .ltd appended
        actualPath=tmpPath+".ltd";
    }

    std::vector<uint8_t> data;
    FILE* f=fopen(actualPath.c_str(),"rb");
    if(!f){Send500(fd,"Cannot read exported file");return;}
    fseek(f,0,SEEK_END);size_t sz=(size_t)ftell(f);fseek(f,0,SEEK_SET);
    data.resize(sz);fread(data.data(),1,sz,f);fclose(f);
    remove(actualPath.c_str());

    // Suggest filename from slot
    std::string disp="attachment; filename=\"Mii_slot"+std::to_string(slot)+".ltd\"";
    Send200Bin(fd,"application/octet-stream",data,disp);
    SrvLog("Mii export slot "+std::to_string(slot)+" OK");
}

// POST /api/mii/import — multipart: file + slot
static void HandleMiiImport(int fd,const Request& req){
    auto fields=ParseMultipart(req.body,req.contentType);
    std::string slotStr;std::vector<uint8_t> fileData;
    for(auto& f:fields){
        if(f.name=="slot")slotStr=f.data;
        else if(f.name=="file")fileData.assign(f.data.begin(),f.data.end());
    }
    if(slotStr.empty()||fileData.empty()){Send500(fd,"Missing slot or file");return;}
    int slot=atoi(slotStr.c_str());
    SrvLog("Mii import: slot "+std::to_string(slot)+" ("+std::to_string(fileData.size())+" bytes)");

    // Write temp file
    MkdirP("/switch/TomoToolNX");
    std::string tmpPath="/switch/TomoToolNX/.mii_import_tmp.ltd";
    {FILE* f=fopen(tmpPath.c_str(),"wb");
    if(!f){Send500(fd,"Cannot write temp file");return;}
    fwrite(fileData.data(),1,fileData.size(),f);fclose(f);}

    std::string err=MiiManager::ImportMii(slot,tmpPath);
    remove(tmpPath.c_str());

    if(!err.empty()){SrvLog("Mii import failed: "+err,true);Send500(fd,err);return;}

    SrvLog("Mii import slot "+std::to_string(slot)+" OK");
    mutexLock(&s_mutex);s_pendingCommit=true;s_pendingMiiRefresh=true;mutexUnlock(&s_mutex);
    Send200(fd,"application/json","{\"ok\":true}");
}

// GET /api/ugc/itemexport?stem=UgcFood005
static void HandleUgcItemExport(int fd,const std::string& query){
    std::string stem=GetQueryParam(query,"stem");
    if(stem.empty()){Send500(fd,"Missing stem");return;}
    static const char* kPfx[]={"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
    static const char* kExt[]={".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl"};
    int kind=-1;int slot0=0;
    if(stem.size()>3&&stem.substr(0,3)=="Ugc"){
        std::string rest=stem.substr(3);
        for(int k=0;k<7;k++){size_t pl=strlen(kPfx[k]);if(rest.size()>pl&&rest.substr(0,pl)==kPfx[k]){kind=k;slot0=atoi(rest.substr(pl).c_str());break;}}
    }
    if(kind<0){Send500(fd,"Cannot parse UGC stem: "+stem);return;}
    std::string tmpPath="/switch/TomoToolNX/.ugc_export_tmp";
    std::string err=MiiManager::ExportUgc(kind,slot0+1,tmpPath);
    if(!err.empty()){SrvLog("UGC item export failed: "+err,true);Send500(fd,err);return;}
    std::string actualPath=tmpPath+kExt[kind];
    std::vector<uint8_t> data;
    FILE* f=fopen(actualPath.c_str(),"rb");
    if(!f){Send500(fd,"Cannot read exported file");return;}
    fseek(f,0,SEEK_END);size_t sz=(size_t)ftell(f);fseek(f,0,SEEK_SET);
    data.resize(sz);fread(data.data(),1,sz,f);fclose(f);remove(actualPath.c_str());
    Send200Bin(fd,"application/octet-stream",data,"attachment; filename=\""+stem+kExt[kind]+"\"");
    SrvLog("UGC item export: "+stem+" OK");
}

// POST /api/ugc/itemimport — multipart: stem + file [+ isAdding]
static void HandleUgcItemImport(int fd,const Request& req){
    auto fields=ParseMultipart(req.body,req.contentType);
    std::string stemStr,isAddingStr;std::vector<uint8_t> fileData;
    for(auto& f:fields){
        if(f.name=="stem")stemStr=f.data;
        else if(f.name=="isAdding")isAddingStr=f.data;
        else if(f.name=="file")fileData.assign(f.data.begin(),f.data.end());
    }
    if(stemStr.empty()||fileData.empty()){Send500(fd,"Missing stem or file");return;}
    static const char* kPfx[]={"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
    int kind=-1;int slot0=0;
    if(stemStr.size()>3&&stemStr.substr(0,3)=="Ugc"){
        std::string rest=stemStr.substr(3);
        for(int k=0;k<7;k++){size_t pl=strlen(kPfx[k]);if(rest.size()>pl&&rest.substr(0,pl)==kPfx[k]){kind=k;slot0=atoi(rest.substr(pl).c_str());break;}}
    }
    if(kind<0){Send500(fd,"Cannot parse UGC stem: "+stemStr);return;}
    bool isAdding=(isAddingStr=="true"||isAddingStr=="1");
    SrvLog("UGC item import: "+stemStr+" kind="+std::to_string(kind)+" slot="+std::to_string(slot0+1)+(isAdding?" (adding)":""));
    MkdirP("/switch/TomoToolNX");
    std::string tmpPath="/switch/TomoToolNX/.ugc_import_tmp.ltdx";
    {FILE* f=fopen(tmpPath.c_str(),"wb");if(!f){Send500(fd,"Cannot write temp file");return;}
    fwrite(fileData.data(),1,fileData.size(),f);fclose(f);}
    std::string err=MiiManager::ImportUgc(kind,slot0+1,tmpPath,isAdding);
    remove(tmpPath.c_str());
    if(!err.empty()){SrvLog("UGC item import failed: "+err,true);Send500(fd,err);return;}
    SrvLog("UGC item import "+stemStr+" OK");
    mutexLock(&s_mutex);s_pendingCommit=true;mutexUnlock(&s_mutex);
    Send200(fd,"application/json","{\"ok\":true}");
}

// GET /api/mii/social?slot=N  — return relationship graph JSON for one mii slot
static void HandleMiiSocial(int fd, const std::string& query) {
    std::string slotStr = GetQueryParam(query, "slot");
    if (slotStr.empty()) { Send500(fd, "Missing slot"); return; }
    int slot = atoi(slotStr.c_str()) - 1; // convert 1-based slot to 0-based index

    SaveEditor::SavFile sav;
    std::string err;
    if (!SaveEditor::Load(SAVE_MII_SAV, sav, err)) { Send500(fd, "Cannot load Mii.sav: " + err); return; }

    static const uint32_t H_ID_A  = 0xf7420afbu;
    static const uint32_t H_ID_B  = 0x4071f71cu;
    static const uint32_t H_BASE  = 0x8b41897eu;
    static const uint32_t H_METER = 0x42c2fc2fu;
    static const uint32_t H_NAME  = 0x2499bfdau;

    auto relColor = [](uint32_t h) -> const char* {
        if (h==0xba939a42u) return "#64C878";
        if (h==0xc2d067a7u) return "#FF96B4";
        if (h==0xb7ce0c18u) return "#FF6464";
        if (h==0x354a0515u) return "#64AAFF";
        if (h==0x7783d4c3u) return "#A050A0";
        if (h==0xfe59f825u) return "#A03C3C";
        if (h==0xdcfc7603u||h==0xe193c5a2u) return "#DC9650";
        if (h==0x1918f808u||h==0x3b1d200au) return "#DCC850";
        if (h==0x2fd9785bu||h==0x7e3cd550u) return "#C8AA78";
        if (h==0x804172f3u) return "#A0A0A0";
        return "#646464";
    };
    auto relName = [](uint32_t h) -> const char* {
        if (h==0x0784a8dcu) return "Unknown";
        if (h==0xba939a42u) return "Friend";
        if (h==0xc2d067a7u) return "Couple";
        if (h==0xb7ce0c18u) return "Lovers";
        if (h==0x354a0515u) return "Knows";
        if (h==0x7783d4c3u) return "Ex";
        if (h==0xfe59f825u) return "Divorced";
        if (h==0xdcfc7603u) return "Parent";
        if (h==0xe193c5a2u) return "Child";
        if (h==0x1918f808u) return "Sibling";
        if (h==0x3b1d200au) return "Sibling";
        if (h==0x2fd9785bu) return "Grandparent";
        if (h==0x7e3cd550u) return "Grandchild";
        if (h==0x804172f3u) return "Relative";
        return "?";
    };
    auto jsonStr = [](const std::string& s) -> std::string {
        std::string r; r.reserve(s.size()+2); r+='"';
        for (char c : s) { if(c=='"')r+="\\\""; else if(c=='\\')r+="\\\\"; else r+=c; }
        r+='"'; return r;
    };

    std::string centerName = SaveEditor::GetWStr32At(sav, H_NAME, slot);
    int pairCount = SaveEditor::ArraySize(sav, H_ID_A);

    std::string json = "{\"center\":{\"slot\":"+std::to_string(slot+1)+",\"name\":"+jsonStr(centerName)+"},\"edges\":[";
    bool first = true;
    for (int i = 0; i < pairCount; i++) {
        int a = SaveEditor::GetIntAt(sav, H_ID_A, i);
        int b = SaveEditor::GetIntAt(sav, H_ID_B, i);
        if (a < 0 || b < 0) continue;
        if (a != slot && b != slot) continue;
        bool selfA = (a == slot);
        int other = selfA ? b : a;
        uint32_t outT = SaveEditor::GetEnumAt(sav, H_BASE, selfA ? i*2 : i*2+1);
        int32_t  outM = SaveEditor::GetIntAt(sav, H_METER, selfA ? i*2 : i*2+1);
        uint32_t inT  = SaveEditor::GetEnumAt(sav, H_BASE, selfA ? i*2+1 : i*2);
        int32_t  inM  = SaveEditor::GetIntAt(sav, H_METER, selfA ? i*2+1 : i*2);
        std::string oName = SaveEditor::GetWStr32At(sav, H_NAME, other);
        if (!first) json += ",";
        first = false;
        json += "{\"slot\":"+std::to_string(other+1)+
                ",\"name\":"+jsonStr(oName)+
                ",\"outType\":\""+relName(outT)+"\""+
                ",\"outMeter\":"+std::to_string(outM)+
                ",\"inType\":\""+relName(inT)+"\""+
                ",\"inMeter\":"+std::to_string(inM)+
                ",\"color\":\""+relColor(outT)+"\"}";
    }
    json += "]}";
    Send200(fd, "application/json", json);
}

// GET /api/save/download?file=player|mii  — serve raw .sav bytes
static void HandleSaveDownload(int fd, const std::string& query) {
    std::string which = GetQueryParam(query, "file");
    const char* path = nullptr;
    const char* fname = nullptr;
    if      (which=="player") { path=SAVE_PLAYER_SAV; fname="Player.sav"; }
    else if (which=="mii")    { path=SAVE_MII_SAV;    fname="Mii.sav";    }
    else if (which=="map")    { path=SAVE_MAP_SAV;    fname="Map.sav";    }
    else { Send404(fd); return; }

    std::vector<uint8_t> data;
    FILE* f = fopen(path, "rb");
    if (!f) { Send500(fd, std::string("Cannot open ")+path); return; }
    fseek(f,0,SEEK_END); size_t sz=(size_t)ftell(f); fseek(f,0,SEEK_SET);
    data.resize(sz); fread(data.data(),1,sz,f); fclose(f);

    std::string disp = std::string("attachment; filename=\"") + fname + "\"";
    Send200Bin(fd,"application/octet-stream",data,disp);
    SrvLog("Save download: "+which+" ("+std::to_string(sz)+" bytes)");
}

// POST /api/save/upload?file=player|mii|map  — receive modified .sav and commit
static void HandleSaveUpload(int fd, const Request& req) {
    std::string which = GetQueryParam(req.query, "file");
    const char* path = nullptr;
    if      (which=="player") path=SAVE_PLAYER_SAV;
    else if (which=="mii")    path=SAVE_MII_SAV;
    else if (which=="map")    path=SAVE_MAP_SAV;
    else { Send500(fd,"Invalid file"); return; }

    if (req.body.size() < 0x20) { Send500(fd,"File too small"); return; }
    if ((uint8_t)req.body[0]!=0x04||(uint8_t)req.body[1]!=0x03||
        (uint8_t)req.body[2]!=0x02||(uint8_t)req.body[3]!=0x01) {
        Send500(fd,"Bad magic: not a .sav file"); return;
    }

    FILE* f = fopen(path, "wb");
    if (!f) { Send500(fd,"Cannot write save"); return; }
    fwrite(req.body.data(),1,req.body.size(),f); fclose(f);

    mutexLock(&s_mutex);
    s_pendingCommit = true;
    if      (which == "player") s_pendingPlayerSavReload = true;
    else if (which == "mii")  { s_pendingMiiSavReload = true; s_pendingMiiRefresh = true; }
    else if (which == "map")    s_pendingMapSavReload = true;
    mutexUnlock(&s_mutex);
    SrvLog("Save upload OK: "+which+" ("+std::to_string(req.body.size())+" bytes)");
    Send200(fd,"application/json","{\"ok\":true}");
}

static void HandleConfig(int fd) {
    mutexLock(&s_mutex);
    bool acked = s_saveWarnAcked;
    mutexUnlock(&s_mutex);
    Send200(fd, "application/json", std::string("{\"saveWarnAcked\":") + (acked ? "true" : "false") + "}");
}

// ── TomodachiShare proxy ─────────────────────────────────────────────────────
// The Switch app routes browser requests through here so that:
//  1. CORS doesn't block them (the browser only ever talks to localhost)
//  2. Rate limits / TLS quirks live in C++ where libcurl already handles them
static size_t ShareCurlAppend(void* ptr, size_t sz, size_t n, void* userp) {
    auto* buf = (std::vector<uint8_t>*)userp;
    size_t total = sz * n;
    buf->insert(buf->end(), (uint8_t*)ptr, (uint8_t*)ptr + total);
    return total;
}
static long ShareCurlFetch(const std::string& url, std::vector<uint8_t>& body, std::string& err) {
    CURL* c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return 0; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "TomoToolNX/share-proxy");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); // Switch has no cert store
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, ShareCurlAppend);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode res = curl_easy_perform(c);
    long status = 0;
    if (res != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(c);
    return status;
}
// Validate that a query value is safe to splice into a URL: only [A-Za-z0-9._-]
// Used for things like ids and tag names; longer free-text params go through curl_easy_escape.
static bool ShareIsSafeToken(const std::string& s, size_t maxLen) {
    if (s.empty() || s.size() > maxLen) return false;
    for (char c : s) {
        bool ok = (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')
                  || c=='_' || c=='-' || c=='.';
        if (!ok) return false;
    }
    return true;
}

// GET /api/share/list?<same params as tomodachishare>
// Forwards the query string verbatim (curl_easy_escape'd for the search term).
static void HandleShareList(int fd, const std::string& query) {
    // Re-encode the whole query string, but allow-list known keys.
    // Keys we forward as-is (after URL-escaping their values):
    //   q, sort, tags, exclude, platform, gender, makeup, allowCopying,
    //   isFromSaveFile, page, limit, timeRange
    static const char* kAllowed[] = {
        "q","sort","tags","exclude","platform","gender","makeup",
        "allowCopying","isFromSaveFile","page","limit","timeRange",
        nullptr
    };
    std::string out;
    CURL* esc = curl_easy_init();
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        pos = (amp == std::string::npos) ? query.size() : amp + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq);
        std::string v = kv.substr(eq + 1);
        bool allowed = false;
        for (int i = 0; kAllowed[i]; i++) if (k == kAllowed[i]) { allowed = true; break; }
        if (!allowed) continue;
        char* enc = curl_easy_escape(esc, v.c_str(), (int)v.size());
        if (!out.empty()) out += "&";
        out += k;
        out += "=";
        out += (enc ? enc : "");
        if (enc) curl_free(enc);
    }
    if (esc) curl_easy_cleanup(esc);

    std::string url = "https://api.tomodachishare.com/api/mii/list";
    if (!out.empty()) { url += "?"; url += out; }

    std::vector<uint8_t> body;
    std::string err;
    long status = ShareCurlFetch(url, body, err);
    if (status == 0) { Send500(fd, "Network error: " + err); return; }
    if (status != 200) {
        Send500(fd, "Upstream returned HTTP " + std::to_string(status));
        return;
    }
    Send200Bin(fd, "application/json", body);
}

// GET /api/share/info?id=N
static void HandleShareInfo(int fd, const std::string& query) {
    std::string id = GetQueryParam(query, "id");
    if (!ShareIsSafeToken(id, 16)) { Send500(fd, "Bad id"); return; }
    std::string url = "https://api.tomodachishare.com/api/mii/" + id + "/info";
    std::vector<uint8_t> body; std::string err;
    long status = ShareCurlFetch(url, body, err);
    if (status == 0) { Send500(fd, "Network error: " + err); return; }
    if (status != 200) { Send500(fd, "Upstream returned HTTP " + std::to_string(status)); return; }
    Send200Bin(fd, "application/json", body);
}

// GET /api/share/image?id=N&type=mii|qr-code|features|facepaint|image0|image1|image2
static void HandleShareImage(int fd, const std::string& query) {
    std::string id   = GetQueryParam(query, "id");
    std::string type = GetQueryParam(query, "type");
    if (type.empty()) type = "mii";
    if (!ShareIsSafeToken(id, 16))   { Send500(fd, "Bad id"); return; }
    if (!ShareIsSafeToken(type, 24)) { Send500(fd, "Bad type"); return; }
    std::string url = "https://api.tomodachishare.com/mii/" + id + "/image?type=" + type;
    std::vector<uint8_t> body; std::string err;
    long status = ShareCurlFetch(url, body, err);
    if (status == 0) { Send500(fd, "Network error: " + err); return; }
    if (status != 200) { Send500(fd, "Upstream returned HTTP " + std::to_string(status)); return; }
    // Images for a given (id,type) are immutable upstream, so cache long.
    std::string h = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: "
                    + std::to_string(body.size())
                    + "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: public, max-age=3600, immutable\r\nConnection: close\r\n\r\n";
    SendStr(fd, h);
    SendAll(fd, (const char*)body.data(), body.size());
}

// POST /api/share/import?id=N&slot=M  — download .ltd from tomodachishare and import to a slot
static void HandleShareImport(int fd, const std::string& query) {
    std::string id   = GetQueryParam(query, "id");
    std::string slotStr = GetQueryParam(query, "slot");
    if (!ShareIsSafeToken(id, 16)) { Send500(fd, "Bad id"); return; }
    if (slotStr.empty()) { Send500(fd, "Missing slot"); return; }
    int slot = atoi(slotStr.c_str());
    if (slot < 1 || slot > 70) { Send500(fd, "Bad slot"); return; }

    std::string url = "https://api.tomodachishare.com/mii/" + id + "/download";
    std::vector<uint8_t> body; std::string err;
    long status = ShareCurlFetch(url, body, err);
    if (status == 0) { Send500(fd, "Network error: " + err); return; }
    if (status == 404) { Send500(fd, "This Mii has no .ltd download available"); return; }
    if (status != 200) { Send500(fd, "Upstream returned HTTP " + std::to_string(status)); return; }
    if (body.empty())   { Send500(fd, "Empty download"); return; }

    MkdirP("/switch/TomoToolNX");
    std::string tmpPath = "/switch/TomoToolNX/.share_import_tmp.ltd";
    {
        FILE* f = fopen(tmpPath.c_str(), "wb");
        if (!f) { Send500(fd, "Cannot write temp file"); return; }
        fwrite(body.data(), 1, body.size(), f); fclose(f);
    }
    std::string importErr = MiiManager::ImportMii(slot, tmpPath);
    remove(tmpPath.c_str());
    if (!importErr.empty()) { SrvLog("Share import failed: " + importErr, true); Send500(fd, importErr); return; }
    SrvLog("Share import: mii " + id + " → slot " + std::to_string(slot));
    mutexLock(&s_mutex); s_pendingCommit = true; s_pendingMiiRefresh = true; mutexUnlock(&s_mutex);
    Send200(fd, "application/json", "{\"ok\":true}");
}

// GET /img/<asset>.png — 302-redirect the browser to the ltdimages repo on
// GitHub so it fetches the full-size image directly. Filename is validated to
// prevent path traversal (the redirect Location is built from it).
static bool IsSafeAssetName(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    if (s.find("..") != std::string::npos) return false;
    if (s.size() < 5 || s.substr(s.size() - 4) != ".png") return false;
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    }
    return true;
}
static void HandleItemImage(int fd, const std::string& path) {
    // path is like "/img/Cloth...Hat_00.png" — strip the "/img/" prefix.
    std::string name = path.substr(5);
    if (!IsSafeAssetName(name)) { Send404(fd); return; }
    Send302(fd, "https://raw.githubusercontent.com/ltdimages/images/main/" + name);
}

// ── Island Generator endpoints ──────────────────────────────────────────────
//
// All endpoints follow the same pattern:
//   1. Load the relevant .sav file from tomodata:/.
//   2. Call into IslandGen / SaveEditor helpers to mutate.
//   3. Save back to disk + SaveMount::Commit().
// Returns JSON {ok, ...stats} or {ok:false, error:...} on failure.

static std::string GenFirstRunMarkerPath() { return "tomodata:/.tomotool_first_run"; }

static bool GenFirstRunMarkerExists() {
    struct stat st;
    return stat(GenFirstRunMarkerPath().c_str(), &st) == 0;
}

static void HandleGenFirstRun(int fd) {
    std::string j = std::string("{\"ok\":true,\"first_run\":")
                  + (GenFirstRunMarkerExists() ? "true" : "false") + "}";
    Send200(fd, "application/json", j);
}

static void HandleGenFirstRunClear(int fd) {
    remove(GenFirstRunMarkerPath().c_str());
    Send200(fd, "application/json", "{\"ok\":true}");
}

static void HandleGenTemplates(int fd) {
    int sCount = 0, tCount = 0;
    const IslandGen::SurfaceTheme* themes    = IslandGen::AllSurfaceThemes(&sCount);
    const IslandGen::MapTemplate*  templates = IslandGen::AllMapTemplates(&tCount);

    std::string j = "{\"ok\":true,\"themes\":[";
    for (int i = 0; i < sCount; i++) {
        if (i) j += ",";
        j += "{\"id\":\""; j += themes[i].id; j += "\"}";
    }
    j += "],\"templates\":[";
    bool first = true;
    for (int i = 0; i < tCount; i++) {
        // Hide templates whose bin2s payload is the 4-byte "STUB" placeholder
        // (no real bytes shipped). Real templates are ~38 KB.
        if (templates[i].size < 1024) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"id\":\"";          j += templates[i].id;
        j += "\",\"name\":\"";      j += templates[i].displayName;
        j += "\",\"description\":\""; j += templates[i].description;
        j += "\"}";
    }
    j += "]}";
    Send200(fd, "application/json", j);
}

static void HandleGenMap(int fd, const std::string& query) {
    std::string mode = GetQueryParam(query, "mode");
    std::string id   = GetQueryParam(query, "id");
    if (mode.empty()) mode = "random";

    SaveEditor::SavFile mp; std::string err;
    if (!SaveEditor::Load("tomodata:/Map.sav", mp, err)) { Send500(fd, "load Map.sav: " + err); return; }

    if (mode == "template") {
        const IslandGen::MapTemplate* t = IslandGen::MapTemplateById(id.c_str());
        if (!t) { Send500(fd, "unknown template id"); return; }
        err = IslandGen::ApplyMapTemplate(mp, *t);
        if (!err.empty()) { Send500(fd, err); return; }
    } else {
        // Random: pick a theme. If query carries a specific theme id, honor
        // it; otherwise roll one uniformly.
        std::string themeId = GetQueryParam(query, "theme");
        const IslandGen::SurfaceTheme* th = themeId.empty()
            ? nullptr
            : IslandGen::SurfaceThemeById(themeId.c_str());
        int tc = 0;
        const IslandGen::SurfaceTheme* themes = IslandGen::AllSurfaceThemes(&tc);
        if (!th) th = &themes[(unsigned)armGetSystemTick() % (unsigned)tc];
        err = IslandGen::GenerateRandomMap(mp, *th);
        if (!err.empty()) { Send500(fd, err); return; }
    }
    int moved = IslandGen::SnapActorsToLand(mp);
    err = SaveEditor::Save("tomodata:/Map.sav", mp);
    if (!err.empty()) { Send500(fd, "save Map.sav: " + err); return; }
    SaveMount::Commit();

    std::string j = "{\"ok\":true,\"actors_snapped\":" + std::to_string(moved) + "}";
    Send200(fd, "application/json", j);
}

static void HandleGenHouses(int fd, const std::string& /*query*/) {
    SaveEditor::SavFile mp, mi; std::string err;
    if (!SaveEditor::Load("tomodata:/Map.sav", mp, err))  { Send500(fd, "load Map.sav: " + err); return; }
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))  { Send500(fd, "load Mii.sav: " + err); return; }

    int placed = IslandGen::AssignHousing(mi, mp);
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) { Send500(fd, "save Mii.sav: " + err); return; }
    SaveMount::Commit();

    std::string j = "{\"ok\":true,\"placed\":" + std::to_string(placed) + "}";
    Send200(fd, "application/json", j);
}

static void HandleGenRels(int fd, const std::string& query) {
    std::string mode = GetQueryParam(query, "mode");
    if (mode.empty()) mode = "dense";

    SaveEditor::SavFile mi; std::string err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))  { Send500(fd, "load Mii.sav: " + err); return; }

    int n = 0;
    if (mode == "none") {
        n = IslandGen::WipeRelationships(mi);
    } else {
        n = IslandGen::WriteDenseRelationships(mi);
    }
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) { Send500(fd, "save Mii.sav: " + err); return; }
    SaveMount::Commit();

    std::string j = "{\"ok\":true,\"written\":" + std::to_string(n) + ",\"mode\":\"" + mode + "\"}";
    Send200(fd, "application/json", j);
}

static void HandleGenWishes(int fd) {
    SaveEditor::SavFile pl; std::string err;
    if (!SaveEditor::Load("tomodata:/Player.sav", pl, err)) { Send500(fd, "load Player.sav: " + err); return; }

    int n = IslandGen::UnlockAllWishes(pl);
    err = SaveEditor::Save("tomodata:/Player.sav", pl);
    if (!err.empty()) { Send500(fd, "save Player.sav: " + err); return; }
    SaveMount::Commit();

    std::string j = "{\"ok\":true,\"unlocked\":" + std::to_string(n) + "}";
    Send200(fd, "application/json", j);
}

static void HandleGenLevels(int fd, const std::string& query) {
    std::string minS = GetQueryParam(query, "min");
    std::string maxS = GetQueryParam(query, "max");
    int minLv = !minS.empty() ? atoi(minS.c_str()) : 100;
    int maxLv = !maxS.empty() ? atoi(maxS.c_str()) : 150;
    // Clamp to a reasonable Tomodachi range. Game caps mii level around 999.
    if (minLv < 0)   minLv = 0;
    if (maxLv > 999) maxLv = 999;

    SaveEditor::SavFile mi; std::string err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))  { Send500(fd, "load Mii.sav: " + err); return; }

    int n = IslandGen::RandomizeMiiLevels(mi, minLv, maxLv);
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) { Send500(fd, "save Mii.sav: " + err); return; }
    SaveMount::Commit();

    std::string j = "{\"ok\":true,\"miis\":" + std::to_string(n)
                  + ",\"min\":" + std::to_string(minLv)
                  + ",\"max\":" + std::to_string(maxLv) + "}";
    Send200(fd, "application/json", j);
}

static void HandleConnection(int fd){
    mutexLock(&s_mutex); s_lastConnectTick = armGetSystemTick(); mutexUnlock(&s_mutex);
    Request req;if(!ReadRequest(fd,req)){close(fd);return;}
    if(req.method=="GET"&&req.path=="/"){SrvLog("WebUI: page loaded");Send200Gzip(fd,"text/html; charset=utf-8",webui_gz_bin,webui_gz_bin_size);}
    else if(req.method=="GET"&&req.path=="/api/config")HandleConfig(fd);
    else if(req.method=="GET"&&req.path=="/api/list")HandleList(fd);
    else if(req.method=="GET"&&req.path=="/api/preview")HandlePreview(fd,req.query);
    else if(req.method=="GET"&&req.path=="/api/export")HandleExport(fd,req.query);
    else if(req.method=="POST"&&req.path=="/api/import")HandleImport(fd,req);
    else if(req.method=="POST"&&req.path=="/api/removebg")HandleBgRemove(fd,req);
    else if(req.method=="GET"  &&req.path=="/api/habits")           HandleHabitsList(fd);
    else if(req.method=="GET"  &&req.path=="/api/wishes/list")      HandleWishesList(fd);
    else if(req.method=="GET"  &&req.path=="/api/mii/list")         HandleMiiList(fd);
    else if(req.method=="GET"  &&req.path=="/api/mii/export")       HandleMiiExport(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/mii/import")       HandleMiiImport(fd,req);
    else if(req.method=="GET"  &&req.path=="/api/mii/social")       HandleMiiSocial(fd,req.query);
    else if(req.method=="GET"  &&req.path=="/api/ugc/itemexport")   HandleUgcItemExport(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/ugc/itemimport")   HandleUgcItemImport(fd,req);
    else if(req.method=="GET"  &&req.path=="/api/save/download")   HandleSaveDownload(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/save/upload")     HandleSaveUpload(fd,req);
    else if(req.method=="GET"  &&req.path=="/api/share/list")      HandleShareList(fd,req.query);
    else if(req.method=="GET"  &&req.path=="/api/share/info")      HandleShareInfo(fd,req.query);
    else if(req.method=="GET"  &&req.path=="/api/share/image")     HandleShareImage(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/share/import")    HandleShareImport(fd,req.query);
    else if(req.method=="GET"  &&req.path.compare(0,5,"/img/")==0)  HandleItemImage(fd,req.path);
    // ── Island Generator endpoints (Phase B) ────────────────────────────────
    else if(req.method=="GET"  &&req.path=="/api/genisland/first_run") HandleGenFirstRun(fd);
    else if(req.method=="POST" &&req.path=="/api/genisland/first_run") HandleGenFirstRunClear(fd);
    else if(req.method=="GET"  &&req.path=="/api/genisland/templates") HandleGenTemplates(fd);
    else if(req.method=="POST" &&req.path=="/api/genisland/map")       HandleGenMap(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/genisland/houses")    HandleGenHouses(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/genisland/rels")      HandleGenRels(fd,req.query);
    else if(req.method=="POST" &&req.path=="/api/genisland/wishes")    HandleGenWishes(fd);
    else if(req.method=="POST" &&req.path=="/api/genisland/levels")    HandleGenLevels(fd,req.query);
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
    if(s_running) return;
    s_port=port;s_ugcPath=ugcPath;s_running=true;s_pendingCommit=false;s_pendingPlayerSavReload=false;s_pendingMiiSavReload=false;s_pendingMapSavReload=false;
    mutexInit(&s_mutex);mutexInit(&s_logMutex);mutexInit(&s_importMutex);mutexInit(&s_bgMutex);
    s_logWrite=0;s_logRead=0;s_importState=ImportState::Idle;s_bgState=ImportState::Idle;
    MkdirP("/switch/TomoToolNX");
    threadCreate(&s_thread,ServerThreadFunc,nullptr,nullptr,128*1024,0x2C,-2);threadStart(&s_thread);
}
void Stop(){if(!s_running)return;s_running=false;threadWaitForExit(&s_thread);threadClose(&s_thread);}
bool IsRunning(){return s_running;}
bool HasPendingCommit(){mutexLock(&s_mutex);bool v=s_pendingCommit;mutexUnlock(&s_mutex);return v;}
void ClearPendingCommit(){mutexLock(&s_mutex);s_pendingCommit=false;mutexUnlock(&s_mutex);}
bool HasPendingMiiRefresh(){mutexLock(&s_mutex);bool v=s_pendingMiiRefresh;mutexUnlock(&s_mutex);return v;}
void ClearPendingMiiRefresh(){mutexLock(&s_mutex);s_pendingMiiRefresh=false;mutexUnlock(&s_mutex);}
bool HasPendingPlayerSavReload(){mutexLock(&s_mutex);bool v=s_pendingPlayerSavReload;mutexUnlock(&s_mutex);return v;}
void ClearPendingPlayerSavReload(){mutexLock(&s_mutex);s_pendingPlayerSavReload=false;mutexUnlock(&s_mutex);}
bool HasPendingMiiSavReload(){mutexLock(&s_mutex);bool v=s_pendingMiiSavReload;mutexUnlock(&s_mutex);return v;}
void ClearPendingMiiSavReload(){mutexLock(&s_mutex);s_pendingMiiSavReload=false;mutexUnlock(&s_mutex);}
bool HasPendingMapSavReload(){mutexLock(&s_mutex);bool v=s_pendingMapSavReload;mutexUnlock(&s_mutex);return v;}
void ClearPendingMapSavReload(){mutexLock(&s_mutex);s_pendingMapSavReload=false;mutexUnlock(&s_mutex);}
bool HasPendingImport(){mutexLock(&s_importMutex);bool v=(s_importState==ImportState::Queued);mutexUnlock(&s_importMutex);return v;}
ImportJob TakePendingImport(){mutexLock(&s_importMutex);ImportJob job=s_importJob;s_importState=ImportState::InProgress;mutexUnlock(&s_importMutex);return job;}
void FinishImport(const std::string& result){mutexLock(&s_importMutex);s_importResult=result;s_importState=ImportState::Done;mutexUnlock(&s_importMutex);}
bool HasPendingBgRemove(){mutexLock(&s_bgMutex);bool v=(s_bgState==ImportState::Queued);mutexUnlock(&s_bgMutex);return v;}
BgRemoveJob TakePendingBgRemove(){mutexLock(&s_bgMutex);BgRemoveJob j=s_bgJob;s_bgState=ImportState::InProgress;mutexUnlock(&s_bgMutex);return j;}
void FinishBgRemove(const std::string& result){mutexLock(&s_bgMutex);s_bgResult=result;s_bgState=ImportState::Done;mutexUnlock(&s_bgMutex);}
void DrainLog(std::vector<LogEntry>& out){mutexLock(&s_logMutex);while(s_logRead<s_logWrite)out.push_back(s_logRing[s_logRead++%LOG_RING_SIZE]);mutexUnlock(&s_logMutex);}
uint64_t LastConnectTick(){mutexLock(&s_mutex);u64 t=s_lastConnectTick;mutexUnlock(&s_mutex);return t;}
void SetSaveWarnAcked(bool v){mutexLock(&s_mutex);s_saveWarnAcked=v;mutexUnlock(&s_mutex);}
} // namespace HttpServer
