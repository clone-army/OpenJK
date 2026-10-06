// Simple NXRP server script: responds to !hello chat command
#include "server.h"

// Return qtrue if handled
qboolean SV_Nxrp_HandleChat( client_t *cl, const char *commandName, const char *chatCursor ) {
	if ( !commandName ) {
		return qfalse;
	}

	// If player typed !hello, respond in chat
	if ( !Q_stricmp( commandName, "hello" ) ) {
		SV_SendServerCommand( cl, "chat \"Hello there\"\n" );
		return qtrue;
	}

	return qfalse;
}
