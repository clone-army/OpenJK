// Shared wallets reserve centrally before game effects; commits and stats use a
// durable local outbox. Losing a reply cancels the reservation, never repeats it.
#include "shared_ledger.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <string>
#include <ctime>
#include <cerrno>

static std::string StatePath(const char *file) {
    const char *dir=getenv("MBIIEZ_STATE_DIR");
    return std::string(dir && *dir ? dir : "/var/lib/mbiiez")+"/"+file;
}
static cJSON *Config() {
    std::ifstream stream(StatePath("shared_engine.json"));
    std::string text((std::istreambuf_iterator<char>(stream)),{});
    return text.size()<8192 ? cJSON_Parse(text.c_str()) : nullptr;
}
qboolean SV_SharedEnabled() {
    cJSON *cfg=Config();bool on=cfg && cJSON_IsTrue(cJSON_GetObjectItem(cfg,"enabled"));
    cJSON_Delete(cfg);static bool announced=false;if(on && !announced){Com_Printf("MBIIEZ_SHARED_LEDGER_V2: shared data enabled\n");announced=true;}return on ? qtrue : qfalse;
}
static std::string Identifier() {
    byte bytes[16];if(!Sys_RandomBytes(bytes,sizeof(bytes)))return {};
    char out[33];for(int i=0;i<16;++i)snprintf(out+i*2,3,"%02x",bytes[i]);return out;
}
static void AddId(cJSON *item) {std::string id=Identifier();cJSON_AddStringToObject(item,"id",id.c_str());}
qboolean SV_SharedEvent(cJSON *event) {
    if(!cJSON_HasObjectItem(event,"id"))AddId(event);
    cJSON *identifier=cJSON_GetObjectItem(event,"id");
    if(!cJSON_IsString(identifier)||strlen(identifier->valuestring)!=32)return qfalse;
    char *text=cJSON_PrintUnformatted(event);if(!text)return qfalse;
    std::string line=std::string(text)+"\n";cJSON_free(text);
    if(line.size()>16384)return qfalse;
    int fd=open(StatePath("shared-events.jsonl").c_str(),O_CREAT|O_APPEND|O_WRONLY|O_NOFOLLOW,0600);
    if(fd<0)return qfalse;
    if(flock(fd,LOCK_EX)!=0){close(fd);return qfalse;}
    bool ok=true;
    off_t before=lseek(fd,0,SEEK_END);
    if(ok)ok=write(fd,line.data(),line.size())==(ssize_t)line.size() && fsync(fd)==0;
    if(!ok && before>=0 && ftruncate(fd,before)!=0)Com_Printf("Shared journal rollback failed; check storage before shared operations.\n");
    flock(fd,LOCK_UN);close(fd);return ok ? qtrue : qfalse;
}
static cJSON *Request(cJSON *item) {
    cJSON *cfg=Config();if(!cfg)return nullptr;
    cJSON *key=cJSON_GetObjectItem(cfg,"key"),*port=cJSON_GetObjectItem(cfg,"port");
    if(!cJSON_IsString(key)||!cJSON_IsNumber(port)){cJSON_Delete(cfg);return nullptr;}
    char *body=cJSON_PrintUnformatted(item);if(!body){cJSON_Delete(cfg);return nullptr;}
    std::string payload=body;cJSON_free(body);
    std::ostringstream output;
    output<<"POST /api/v1/shared/engine HTTP/1.0\r\nHost: localhost\r\nAuthorization: Bearer "<<key->valuestring
          <<"\r\nContent-Type: application/json\r\nContent-Length: "<<payload.size()<<"\r\nConnection: close\r\n\r\n"<<payload;
    int apiPort=port->valueint;cJSON_Delete(cfg);
    if(apiPort<1||apiPort>65535)return nullptr;
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return nullptr;
    timeval deadline{1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&deadline,sizeof(deadline));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(apiPort);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    bool ok=connect(fd,(sockaddr*)&addr,sizeof(addr))==0;
    std::string request=output.str();size_t sent=0;
    while(ok && sent<request.size()) {ssize_t n=send(fd,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);if(n<=0)ok=false;else sent+=n;}
    std::string reply;char buffer[4096];
    while(ok) {ssize_t n=recv(fd,buffer,sizeof(buffer),0);if(n==0)break;if(n<0){ok=false;break;}reply.append(buffer,n);if(reply.size()>1024*1024){ok=false;break;}}
    close(fd);auto separator=reply.find("\r\n\r\n");
    if(!ok || reply.find(" 200 ")>16 || separator==std::string::npos)return nullptr;
    return cJSON_Parse(reply.c_str()+separator+4);
}
static std::string StartTicks() {
    std::ifstream f("/proc/self/stat");std::string s;std::getline(f,s);auto at=s.rfind(')');
    if(at==std::string::npos)return {};
    std::istringstream fields(s.substr(at+1));std::string field;
    for(int i=0;i<=19;++i)if(!(fields>>field))return {};
    return field;
}
static bool Reserve(cJSON *changes,cJSON **reply) {
    std::string reserve=Identifier();if(reserve.empty())return false;
    cJSON *intent=cJSON_CreateObject();cJSON_AddStringToObject(intent,"kind","intent");cJSON_AddStringToObject(intent,"id",reserve.c_str());
    cJSON_AddItemToObject(intent,"changes",cJSON_Duplicate(changes,1));
    cJSON_AddNumberToObject(intent,"pid",getpid());cJSON_AddStringToObject(intent,"started",StartTicks().c_str());
    bool durable=SV_SharedEvent(intent);cJSON_Delete(intent);if(!durable)return false;
    cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"kind","reserve");cJSON_AddStringToObject(item,"id",reserve.c_str());
    cJSON_AddItemToObject(item,"changes",cJSON_Duplicate(changes,1));
    *reply=Request(item);cJSON_Delete(item);
    bool accepted=*reply && cJSON_IsObject(cJSON_GetObjectItem(*reply,"balances"));
    cJSON *finish=cJSON_CreateObject();cJSON_AddStringToObject(finish,"kind",accepted ? "commit" : "cancel");
    cJSON_AddStringToObject(finish,"reservation",reserve.c_str());
    if(!SV_SharedEvent(finish)){accepted=false;cJSON_ReplaceItemInObject(finish,"kind",cJSON_CreateString("cancel"));SV_SharedEvent(finish);}
    cJSON_Delete(finish);return accepted;
}
qboolean SV_SharedSpend(client_t *cl,int amount,const char *recipient) {
    if(amount<=0)return amount==0 ? qtrue : qfalse;
    if(!SV_SharedEnabled()) return SV_EconomyStandaloneDebit(cl,amount);
    cJSON *changes=cJSON_CreateArray(),*change=cJSON_CreateObject();
    cJSON_AddStringToObject(change,"session",cl->economySharedSession);cJSON_AddStringToObject(change,"handle",cl->economyHandle);cJSON_AddNumberToObject(change,"amount",amount);
    if(recipient && *recipient)cJSON_AddStringToObject(change,"recipient",recipient);
    cJSON_AddItemToArray(changes,change);cJSON *reply=nullptr;
    bool ok=Reserve(changes,&reply);cJSON_Delete(changes);
    if(ok){cJSON *balances=cJSON_GetObjectItem(reply,"balances");char handle[24];Q_strncpyz(handle,cl->economyHandle,sizeof(handle));Q_strlwr(handle);
        cJSON *value=cJSON_GetObjectItem(balances,handle);ok=cJSON_IsNumber(value);
        if(ok)cl->economyCredits=cl->economyCreditsSynced=value->valueint;}
    if(reply && cJSON_IsTrue(cJSON_GetObjectItem(reply,"relogin"))){cl->economyHandle[0]=cl->economySharedSession[0]=0;cl->economyCredits=cl->economyCreditsSynced=0;}
    cJSON_Delete(reply);
    if(!ok)SV_SendServerCommand(cl,"chat \"^1[Credits]^7 Purchase not completed: insufficient credits or shared service unavailable. Try again shortly.\"\n");
    return ok ? qtrue : qfalse;
}
qboolean SV_SharedSpendPair(client_t *a,client_t *b,int amount) {
    if(amount<=0)return amount==0 ? qtrue : qfalse;
    if(!SV_SharedEnabled()) return SV_EconomyStandalonePair(a,b,amount);
    cJSON *changes=cJSON_CreateArray();for(client_t *cl:{a,b}){cJSON *row=cJSON_CreateObject();cJSON_AddStringToObject(row,"session",cl->economySharedSession);cJSON_AddStringToObject(row,"handle",cl->economyHandle);cJSON_AddNumberToObject(row,"amount",amount);cJSON_AddItemToArray(changes,row);}
    cJSON *reply=nullptr;bool ok=Reserve(changes,&reply);cJSON_Delete(changes);
    if(ok)for(client_t *cl:{a,b}){char handle[24];Q_strncpyz(handle,cl->economyHandle,sizeof(handle));Q_strlwr(handle);cJSON *value=cJSON_GetObjectItem(cJSON_GetObjectItem(reply,"balances"),handle);if(!cJSON_IsNumber(value)){ok=false;break;}cl->economyCredits=cl->economyCreditsSynced=value->valueint;}
    cJSON_Delete(reply);return ok ? qtrue : qfalse;
}
qboolean SV_SharedCredits(const char *handle,int amount) {
    if(amount<0)return qfalse;
    cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"kind","credit");cJSON_AddStringToObject(event,"handle",handle);cJSON_AddNumberToObject(event,"amount",amount);
    qboolean ok=SV_SharedEvent(event);cJSON_Delete(event);return ok;
}
qboolean SV_SharedBalance(const char *handle,int *balance) {
    cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"kind","balance");cJSON_AddStringToObject(item,"handle",handle);
    cJSON *reply=Request(item);cJSON_Delete(item);cJSON *value=reply ? cJSON_GetObjectItem(reply,"credits") : nullptr;
    bool ok=cJSON_IsNumber(value);if(ok)*balance=value->valueint;cJSON_Delete(reply);return ok ? qtrue : qfalse;
}
qboolean SV_SharedLogin(client_t *cl,const char *kind,const char *handle,const char *pin,int bonus) {
    cJSON *item=cJSON_CreateObject();AddId(item);cJSON_AddStringToObject(item,"kind",kind);cJSON_AddStringToObject(item,"handle",handle);cJSON_AddStringToObject(item,"pin",pin);cJSON_AddNumberToObject(item,"bonus",bonus);
    cJSON *reply=Request(item);cJSON_Delete(item);
    cJSON *name=reply ? cJSON_GetObjectItem(reply,"handle") : nullptr,*credits=reply ? cJSON_GetObjectItem(reply,"credits") : nullptr;
    bool ok=cJSON_IsString(name)&&cJSON_IsNumber(credits);
    if(ok){cJSON *session=cJSON_GetObjectItem(reply,"session");if(!cJSON_IsString(session)){cJSON_Delete(reply);return qfalse;}Q_strncpyz(cl->economySharedSession,session->valuestring,sizeof(cl->economySharedSession));Q_strncpyz(cl->economyHandle,name->valuestring,sizeof(cl->economyHandle));cl->economyCredits=cl->economyCreditsSynced=credits->valueint;
        SV_SendServerCommand(cl,"chat \"^2[Accounts]^7 Logged in to shared account '%s'. Balance: %d credits.\"\n",cl->economyHandle,cl->economyCredits);}
    else SV_SendServerCommand(cl,"chat \"^1[Accounts]^7 Login/registration failed. Check your handle and PIN, or retry when the shared service is available.\"\n");
    cJSON_Delete(reply);return ok ? qtrue : qfalse;
}
qboolean SV_SharedStats(const char *key,int kills,int deaths,int suicides,int seconds) {
    cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"kind","stats");cJSON_AddStringToObject(event,"key",key);
    int values[]={kills,deaths,suicides,seconds};cJSON_AddItemToObject(event,"delta",cJSON_CreateIntArray(values,4));
    qboolean ok=SV_SharedEvent(event);cJSON_Delete(event);return ok;
}
qboolean SV_SharedKill(const char *killer,const char *victim) {
    cJSON *event=cJSON_CreateObject(),*rows=cJSON_CreateArray();
    cJSON_AddStringToObject(event,"kind","stats_batch");
    const char *keys[]={killer,victim};
    for(int i=0;i<2;++i){cJSON *row=cJSON_CreateObject();cJSON_AddStringToObject(row,"key",keys[i]);int delta[]={i==0 ? 1:0,i==1 ? 1:0,0,0};cJSON_AddItemToObject(row,"delta",cJSON_CreateIntArray(delta,4));cJSON_AddItemToArray(rows,row);}
    cJSON_AddItemToObject(event,"rows",rows);qboolean ok=SV_SharedEvent(event);cJSON_Delete(event);return ok;
}
qboolean SV_SharedBan(const char *kind,const char *key,cJSON *row,int revision) {
    cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"kind",kind);cJSON_AddNumberToObject(event,"base_revision",revision);cJSON_AddStringToObject(event,"dataset","guid_bans");cJSON_AddStringToObject(event,"key",key);
    if(row)cJSON_AddItemToObject(event,"row",cJSON_Duplicate(row,1));qboolean ok=SV_SharedEvent(event);cJSON_Delete(event);return ok;
}
// Embedded marker used by the agent to reject legacy game processes safely.
static const char *const sharedVersion="MBIIEZ_SHARED_LEDGER_V2";
