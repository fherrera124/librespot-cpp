/*
 * esdk_server.c — Spotify Connect ZeroConf receiver driven by the real Spotify
 * eSDK (libspotify_embedded_shared.so), API v4 (eSDK 1.20.0).
 *
 * Advertises a Connect device over mDNS (_spotify-connect._tcp, run by run.sh),
 * serves the ZeroConf getInfo/addUser HTTP endpoints by hand, and hands the
 * decrypted credentials blob to SpConnectionLoginZeroConf(). On a granted
 * account the eSDK connects to ap.spotify.com, fetches+decrypts+decodes the
 * stream internally and emits PCM through onAudioData().
 *
 * Runs natively on ARM Linux (Raspberry Pi, armhf) or under qemu-arm on x86.
 *
 * Usage: ./esdk_server [so] [appkey] [apiver=4] [port=8000] [name]
 * Env:   ESDK_PCM_STDOUT=1  -> write raw interleaved S16 PCM to stdout
 *                             (pipe to: | aplay -f cd   for 44100/16/stereo)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ---- eSDK types (API v4, 32-bit packed; derived from disassembling SpInit) ---- */
typedef struct {
    uint32_t version; void* wmem; uint32_t wmem_size;
    const uint8_t* app_key; uint32_t app_key_len;
    const char* uniqueid; const char* displayname;
    const char* brand; const char* model; const char* clientid;
    const char* osversion; int devicetype;
    void (*on_error)(int, void*); void* on_error_context;
} sp_init_config_t;

typedef struct {
    char token[150], uniqueid_hash[65], username[65], displayname[65];
    char accounttype[16], devicetype[16], version[31];
    uint32_t unknown; char unknown2[16];
} sp_zeroconfvars_t;

typedef struct {
    void (*onNotify)(int state, void* data);
    void (*onNotifyLoggedIn)(const char* blob, const char* username, void* data);
    void (*onMessage)(const char* msg, void* data);
} sp_connection_callbacks_t;

typedef struct { void (*print)(const char* line, void* data); } sp_debug_callbacks_t;
typedef int (*SpRegisterDebugCallbacks_t)(const sp_debug_callbacks_t*, void*);
static void cb_debug(const char* line, void* d){ (void)d; fprintf(stderr,"[ESDK] %s\n", line?line:""); }

/* Audio sample format handed to onAudioData */
typedef struct { int nchannels; int samplerate; } sp_sampleformat_t;

/* Playback callbacks: v4 .so copies 5 fn ptrs (SpRegisterPlaybackCallbacks internal @0x22eb0). */
typedef struct {
    int  (*onNotify)(int notify, void* data);
    unsigned long (*onAudioData)(const short* frames, unsigned long nframes,
                                 const sp_sampleformat_t* format, unsigned int arg4, void* data);
    void (*onSeek)(uint64_t position, void* data);
    void (*onApplyVolume)(unsigned short volume, void* data);
    void (*onUnavailableTrack)(const char* uri, void* data);
} sp_playback_callbacks_t;
typedef int (*SpRegisterPlaybackCallbacks_t)(const sp_playback_callbacks_t*, void*);

static int g_pcm_stdout = 0;
static unsigned long g_total_frames = 0; static int g_audio_seen = 0; static unsigned long g_next_log = 0;
static int cb_pb_notify(int n, void* d){ (void)d; fprintf(stderr,"[pb] onNotify %d\n", n); return 0; }
static unsigned long cb_audio(const short* f, unsigned long nf, const sp_sampleformat_t* fmt,
                              unsigned int a4, void* d){
    (void)a4;(void)d;
    int ch = fmt ? fmt->nchannels : 2, rate = fmt ? fmt->samplerate : 44100;
    if(!g_audio_seen){
        fprintf(stderr,"\n*** [pb] FIRST AUDIO DATA: nframes=%lu  %dch @%dHz  "
                       "<== KEY GRANTED + AUDIO DECRYPTED/DECODED\n\n", nf, ch, rate);
        g_audio_seen = 1;
    }
    if(g_pcm_stdout && f && nf){ fwrite(f, (size_t)ch*sizeof(short), nf, stdout); fflush(stdout); }
    g_total_frames += nf;
    if(g_total_frames >= g_next_log){
        fprintf(stderr,"[pb] audio flowing: total frames=%lu (%dch @%dHz)\n", g_total_frames, ch, rate);
        g_next_log = g_total_frames + (unsigned)rate; /* ~once per second */
    }
    return nf; /* consume all frames so playback keeps progressing */
}
static void cb_seek(uint64_t p, void* d){ (void)d; fprintf(stderr,"[pb] onSeek %llu\n",(unsigned long long)p); }
static void cb_vol(unsigned short v, void* d){ (void)d; fprintf(stderr,"[pb] onApplyVolume %u\n", v); }
static void cb_unavail(const char* uri, void* d){ (void)d; fprintf(stderr,"[pb] onUnavailableTrack '%s'\n", uri?uri:""); }

typedef int (*SpInit_t)(const sp_init_config_t*);
typedef int (*SpZeroConfGetVars_t)(sp_zeroconfvars_t*);
typedef int (*SpPumpEvents_t)(void);
typedef int (*SpRegisterConnectionCallbacks_t)(const sp_connection_callbacks_t*, void*);
typedef int (*SpConnectionLoginZeroConf_t)(const char*, const char*, const char*, const char*);
typedef const char* (*SpGetLibraryVersion_t)(void);

static SpZeroConfGetVars_t SpZeroConfGetVars;
static SpConnectionLoginZeroConf_t SpConnectionLoginZeroConf;

static void on_error_cb(int e, void* d){ (void)d; fprintf(stderr,"[on_error] %d\n", e); }
static void cb_notify(int s, void* d){ (void)d; fprintf(stderr,"[conn] onNotify state=%d\n", s); }
static void cb_login(const char* blob, const char* user, void* d){ (void)d;
    fprintf(stderr,"\n*** [conn] LOGGED IN username='%s'\n*** reusable blob=%s\n\n",
            user?user:"?", blob?blob:"(null)"); }
static void cb_msg(const char* m, void* d){ (void)d; fprintf(stderr,"[conn] onMessage: %s\n", m?m:""); }

/* url-decode in place. NB: do NOT map '+' to space — blob/clientKey are base64 with literal '+'. */
static void urldec(char* s){ char* o=s; for(;*s;s++){ if(*s=='%'&&s[1]&&s[2]){ int h; sscanf(s+1,"%2x",&h); *o++=(char)h; s+=2;} else *o++=*s; } *o=0; }
static int getfield(const char* body, const char* key, char* out, size_t n){
    char pat[64]; snprintf(pat,sizeof pat,"%s=",key);
    const char* p=strstr(body,pat); if(!p) return 0; p+=strlen(pat);
    const char* e=strchr(p,'&'); size_t len=e?(size_t)(e-p):strlen(p);
    if(len>=n) len=n-1; memcpy(out,p,len); out[len]=0; urldec(out); return 1;
}

static void send_json(int fd, const char* json){
    char hdr[256]; int L=strlen(json);
    int n=snprintf(hdr,sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n", L);
    write(fd,hdr,n); write(fd,json,L);
}

int main(int argc, char** argv){
    const char* so   = argc>1?argv[1]:"lib/esdk-1.20.0-armhf-v7.so";
    const char* keyf = argc>2?argv[2]:"lib/spotify_appkey.key";
    int api          = argc>3?atoi(argv[3]):4;
    int port         = argc>4?atoi(argv[4]):8000;
    const char* name = argc>5?argv[5]:"cspot-esdk-test";
    g_pcm_stdout = getenv("ESDK_PCM_STDOUT") != NULL;

    FILE* f=fopen(keyf,"rb"); if(!f){perror("appkey");return 1;}
    static uint8_t key[4096]; size_t klen=fread(key,1,sizeof key,f); fclose(f);

    void* h=dlopen(so, RTLD_NOW|RTLD_GLOBAL); if(!h) h=dlopen(so, RTLD_LAZY|RTLD_GLOBAL);
    if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    SpInit_t SpInit=(SpInit_t)dlsym(h,"SpInit");
    SpZeroConfGetVars=(SpZeroConfGetVars_t)dlsym(h,"SpZeroConfGetVars");
    SpPumpEvents_t SpPumpEvents=(SpPumpEvents_t)dlsym(h,"SpPumpEvents");
    SpRegisterConnectionCallbacks_t SpRegCB=(SpRegisterConnectionCallbacks_t)dlsym(h,"SpRegisterConnectionCallbacks");
    SpConnectionLoginZeroConf=(SpConnectionLoginZeroConf_t)dlsym(h,"SpConnectionLoginZeroConf");
    SpGetLibraryVersion_t SpVer=(SpGetLibraryVersion_t)dlsym(h,"SpGetLibraryVersion");
    fprintf(stderr,"lib=%s  LoginZeroConf=%p\n", SpVer?SpVer():"?", (void*)SpConnectionLoginZeroConf);

    void* wmem=calloc(1,0x1000000);
    sp_init_config_t cfg; memset(&cfg,0,sizeof cfg);
    cfg.version=api; cfg.wmem=wmem; cfg.wmem_size=0x1000000;
    cfg.app_key=key; cfg.app_key_len=klen;
    cfg.uniqueid="02:22:61:8E:82:C1"; cfg.displayname=name;  /* uniqueid: any stable id; see getInfo below */
    cfg.brand="Sangean"; cfg.model="WFR-28C"; cfg.osversion="1.0.0";
    cfg.devicetype=4; cfg.on_error=on_error_cb;
    int rc=SpInit(&cfg); fprintf(stderr,"SpInit -> %d\n",rc); if(rc){return rc;}

    SpRegisterDebugCallbacks_t SpRegDbg=(SpRegisterDebugCallbacks_t)dlsym(h,"SpRegisterDebugCallbacks");
    if(SpRegDbg){ static sp_debug_callbacks_t dcb; dcb.print=cb_debug;
        fprintf(stderr,"SpRegisterDebugCallbacks -> %d\n", SpRegDbg(&dcb,NULL)); }

    if(SpRegCB){ static sp_connection_callbacks_t cbs; cbs.onNotify=cb_notify;
        cbs.onNotifyLoggedIn=cb_login; cbs.onMessage=cb_msg; SpRegCB(&cbs,NULL); }

    SpRegisterPlaybackCallbacks_t SpRegPB=(SpRegisterPlaybackCallbacks_t)dlsym(h,"SpRegisterPlaybackCallbacks");
    if(SpRegPB){ static sp_playback_callbacks_t pcb;
        pcb.onNotify=cb_pb_notify; pcb.onAudioData=cb_audio; pcb.onSeek=cb_seek;
        pcb.onApplyVolume=cb_vol; pcb.onUnavailableTrack=cb_unavail;
        fprintf(stderr,"SpRegisterPlaybackCallbacks -> %d\n", SpRegPB(&pcb,NULL)); }

    int ls=socket(AF_INET,SOCK_STREAM,0); int one=1;
    setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET;
    a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
    if(bind(ls,(struct sockaddr*)&a,sizeof a)<0){perror("bind");return 1;}
    listen(ls,8);
    fprintf(stderr,"ZeroConf HTTP server listening on 0.0.0.0:%d\n",port);

    for(;;){
        if(SpPumpEvents) SpPumpEvents();
        struct pollfd pf={ls,POLLIN,0};
        if(poll(&pf,1,100)<=0) continue;
        int cs=accept(ls,NULL,NULL); if(cs<0) continue;
        static char buf[8192]; int tot=0,n;
        while((n=read(cs,buf+tot,sizeof(buf)-1-tot))>0){ tot+=n;
            buf[tot]=0; if(strstr(buf,"\r\n\r\n")){
                char* cl=strcasestr(buf,"content-length:");
                if(cl){ int len=atoi(cl+15); char* b=strstr(buf,"\r\n\r\n")+4;
                        int have=tot-(b-buf); while(have<len && (n=read(cs,buf+tot,sizeof(buf)-1-tot))>0){tot+=n;have+=n;} buf[tot]=0; }
                break; }
            if(tot>=(int)sizeof(buf)-1) break;
        }
        int isAddUser = strstr(buf,"addUser")!=NULL;

        if(isAddUser){
            char* body=strstr(buf,"\r\n\r\n"); body=body?body+4:buf;
            static char user[128],blob[4096],ckey[512];
            user[0]=blob[0]=ckey[0]=0;
            getfield(body,"userName",user,sizeof user);
            getfield(body,"blob",blob,sizeof blob);
            getfield(body,"clientKey",ckey,sizeof ckey);
            fprintf(stderr,"[addUser] user='%s' blob=%zuB clientKey=%zuB\n",user,strlen(blob),strlen(ckey));
            int lrc=-1;
            if(SpConnectionLoginZeroConf) lrc=SpConnectionLoginZeroConf(user,blob,ckey,"");
            fprintf(stderr,"[addUser] SpConnectionLoginZeroConf -> %d\n",lrc);
            for(int i=0;i<20;i++){ if(SpPumpEvents) SpPumpEvents(); usleep(100000); }
            send_json(cs,"{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}");
        } else { /* getInfo — cloned to the real Sangean's minimal schema */
            sp_zeroconfvars_t z; memset(&z,0,sizeof z); SpZeroConfGetVars(&z);
            static char j[2048];
            /* deviceID MUST be the eSDK's own uniqueid_hash: the inner credentials
               blob's AES key is SHA1(device_id), and the eSDK decrypts with
               SHA1(uniqueid_hash). Advertise anything else -> "Parsing ZeroConf
               blob failed" and login returns 1. */
            snprintf(j,sizeof j,
              "{\"status\": 101, \"statusString\": \"OK\", \"spotifyError\": 0, "
              "\"spotifyErrorString\": \"UNUSED\", \"version\": \"2.0.1\", "
              "\"deviceID\": \"%s\", \"deviceType\": \"%s\", "
              "\"remoteName\": \"%s\", \"activeUser\": \"%s\", \"publicKey\": \"%s\"}",
              z.uniqueid_hash, z.devicetype[0]?z.devicetype:"SPEAKER", z.displayname, z.username, z.token);
            send_json(cs,j);
        }
        close(cs);
    }
    return 0;
}
