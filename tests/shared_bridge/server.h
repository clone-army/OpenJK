#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sys/random.h>
using byte=unsigned char;
enum qboolean {qfalse,qtrue};
struct client_t {char economyHandle[24]{};char economySharedSession[33]{};int economyCredits=0;int economyCreditsSynced=0;};
inline bool Sys_RandomBytes(byte *p,int n){return getrandom(p,n,0)==n;}
inline void Q_strncpyz(char *out,const char *in,int n){snprintf(out,n,"%s",in);}
inline void Q_strlwr(char *p){for(;*p;++p)*p=tolower(*p);}
inline void Com_Printf(const char *,...){ }
inline void SV_SendServerCommand(client_t *,const char *,...){ }
