#include "luascript.h"
#include "gens.h"
#include "save.h"
#include "g_main.h"
#include "guidraw.h"
#include "movie.h"
#include "vdp_io.h"
#include "drawutil.h"
#include <assert.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

// the emulator must provide these so that we can implement
// the various functions the user can call from their lua script
// (this interface with the emulator needs cleanup, I know)
extern int (*Update_Frame)();
extern int (*Update_Frame_Fast)();
extern "C" unsigned int ReadValueAtHardwareAddress(unsigned int address, unsigned int size);
extern "C" void WriteValueAtHardwareAdress(unsigned int address, unsigned int value, unsigned int size);
extern "C" int disableSound2, disableRamSearchUpdate;
extern "C" int Clear_Sound_Buffer(void);
extern long long GetCurrentInputCondensed();
extern void SetNextInputCondensed(long long input, long long mask);
extern int Set_Current_State(int Num);
extern int Update_Emulation_One(HWND hWnd);
extern void Update_Emulation_One_Before(HWND hWnd);
extern void Update_Emulation_After_Fast(HWND hWnd);
extern void Update_Emulation_One_Before_Minimal();
extern int Update_Frame_Adjusted();
extern int Update_Frame_Hook();
extern int Update_Frame_Fast_Hook();
extern void Update_Emulation_After_Controlled(HWND hWnd, bool flip);
extern void UpdateLagCount();
extern bool BackgroundInput;
extern bool Step_Gens_MainLoop(bool allowSleep, bool allowEmulate);
extern bool frameadvSkipLagForceDisable;
extern "C" void Put_Info(char *Message, int Duration);
extern int Show_Genesis_Screen();

extern "C" {
	#include "lua/src/lua.h"
	#include "lua/src/lauxlib.h"
	#include "lua/src/lualib.h"
	#include "lua/src/lstate.h"
};

enum SpeedMode
{
	SPEEDMODE_NORMAL,
	SPEEDMODE_NOTHROTTLE,
	SPEEDMODE_TURBO,
	SPEEDMODE_MAXIMUM,
};

struct LuaContextInfo {
	lua_State* L; // the Lua state
	bool started; // script has been started and hasn't yet been terminated, although it may not be currently running
	bool running; // script is currently running code (either the main call to the script or the callbacks it registered)
	bool returned; // main call to the script has returned (but it may still be active if it registered callbacks)
	bool crashed; // true if script has errored out
	bool restart; // if true, tells the script-running code to restart the script when the script stops
	bool restartLater; // set to true when a still-running script is stopped so that RestartAllLuaScripts can know which scripts to restart
	int worryCount; // counts up as the script executes, gets reset when the application is able to process messages, triggers a warning prompt if it gets too high
	bool panic; // if set to true, tells the script to terminate as soon as it can do so safely (used because directly calling lua_close() or luaL_error() is unsafe in some contexts)
	bool ranExit; // used to prevent a registered exit callback from ever getting called more than once
	bool guiFuncsNeedDeferring; // true whenever GUI drawing would be cleared by the next emulation update before it would be visible, and thus needs to be deferred until after the next emulation update
	bool ranFrameAdvance; // false if gens.frameadvance() hasn't been called yet
	SpeedMode speedMode; // determines how gens.frameadvance() acts
	char panicMessage [64]; // a message to print if the script terminates due to panic being set
	std::string lastFilename; // path to where the script last ran from so that restart can work (note: storing the script in memory instead would not be useful because we always want the most up-to-date script from file)
	// callbacks into the lua window... these don't need to exist per context the way I'm using them, but whatever
	void(*print)(int uid, const char* str);
	void(*onstart)(int uid);
	void(*onstop)(int uid);
};
std::map<int, LuaContextInfo*> luaContextInfo;
std::map<lua_State*, LuaContextInfo*> luaStateToContextMap; // because callbacks from lua will only give us a Lua_State to work with
std::map<lua_State*, int> luaStateToUIDMap;
int g_numScriptsStarted = 0;

static const char* luaCallIDStrings [] =
{
	"CALL_BEFOREEMULATION",
	"CALL_AFTEREMULATION",
	"CALL_AFTEREMULATIONGUI",
	"CALL_BEFOREEXIT",
	"CALL_BEFORESAVE",
	"CALL_AFTERLOAD",
};

static const int _makeSureWeHaveTheRightNumberOfStrings [sizeof(luaCallIDStrings)/sizeof(*luaCallIDStrings) == LUACALL_COUNT ? 1 : 0];

void StopScriptIfFinished(int uid, bool justReturned = false);

int add_memory_proc (int *list, int addr, int &numprocs)
{
	if (numprocs >= 16) return 1;
	int i = 0;
	while ((i < numprocs) && (list[i] < addr))
		i++;
	for (int j = numprocs; j >= i; j--)
		list[j] = list[j-i];
	list[i] = addr;
	numprocs++;
	return 0;
}
int del_memory_proc (int *list, int addr, int &numprocs)
{
	if (numprocs <= 0) return 1;
	int i = 0;
//	bool found = false;
	while ((i < numprocs) && (list[i] != addr))
		i++;
	if (list[i] == addr)
	{
		while (i < numprocs)
		{
			list[i] = list[i+1];
			i++;
		}
		list[i]=0;
		return 0;
	}
	else return 1;
}
#define memreg(proc)\
int proc##_writelist[16];\
int proc##_readlist[16];\
int proc##_execlist[16];\
int proc##_numwritefuncs = 0;\
int proc##_numreadfuncs = 0;\
int proc##_numexecfuncs = 0;\
int memory_register##proc##write (lua_State* L)\
{\
	unsigned int addr = luaL_checkinteger(L, 1);\
	char Name[16];\
	sprintf(Name,#proc"_W%08X",addr);\
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)\
		luaL_error(L, "function or nil expected in arg 2 to memory.register" #proc "write");\
	lua_setfield(L, LUA_REGISTRYINDEX, Name);\
	if (lua_type(L,2) == LUA_TNIL) \
		del_memory_proc(proc##_writelist, addr, proc##_numwritefuncs);\
	else \
		if (add_memory_proc(proc##_writelist, addr, proc##_numwritefuncs)) \
			luaL_error(L, #proc "write hook registry is full.");\
	return 0;\
}\
int memory_register##proc##read (lua_State* L)\
{\
	unsigned int addr = luaL_checkinteger(L, 1);\
	char Name[16];\
	sprintf(Name,#proc"_R%08X",addr);\
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)\
		luaL_error(L, "function or nil expected in arg 2 to memory.register" #proc "write");\
	lua_setfield(L, LUA_REGISTRYINDEX, Name);\
	if (lua_type(L,2) == LUA_TNIL) \
		del_memory_proc(proc##_readlist, addr, proc##_numreadfuncs);\
	else \
		if (add_memory_proc(proc##_readlist, addr, proc##_numreadfuncs)) \
			luaL_error(L, #proc "read hook registry is full.");\
	return 0;\
}\
int memory_register##proc##exec (lua_State* L)\
{\
	unsigned int addr = luaL_checkinteger(L, 1);\
	char Name[16];\
	sprintf(Name,#proc"_E%08X",addr);\
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)\
		luaL_error(L, "function or nil expected in arg 2 to memory.register" #proc "write");\
	lua_setfield(L, LUA_REGISTRYINDEX, Name);\
	if (lua_type(L,2) == LUA_TNIL) \
		del_memory_proc(proc##_execlist, addr, proc##_numexecfuncs);\
	else \
		if (add_memory_proc(proc##_execlist, addr, proc##_numexecfuncs)) \
			luaL_error(L, #proc "exec hook registry is full.");\
	return 0;\
}
memreg(M68K)
memreg(S68K)
//memreg(SH2)
//memreg(Z80)
int gens_registerbefore(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int gens_registerafter(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int gens_registerexit(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int gui_register(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATIONGUI]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int state_registersave(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESAVE]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int state_registerload(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERLOAD]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}


static const char* deferredGUIIDString = "lazygui";

// store the most recent C function call from Lua (and all its arguments)
// for later evaluation
void DeferFunctionCall(lua_State* L, const char* idstring)
{
	// there's probably a cleaner way of doing this using lua_pushcclosure or lua_getfenv

	int num = lua_gettop(L);

	// get the C function pointer
	int funcStackIndex = -(num+1);
	lua_CFunction cf = lua_tocfunction(L, funcStackIndex);
	assert(cf); // the above is kind of a hack so it might need to be replaced with the following also-hack if this assert fails:
	//lua_CFunction cf = &(L->ci->func)->value.gc->cl.c.f;
	lua_pushcfunction(L,cf);

	// make a list of the function and its arguments (and also pop those arguments from the stack)
	lua_createtable(L, num+1, 0);
	lua_insert(L, 1);
	for(int n = num+1; n > 0; n--)
		lua_rawseti(L, 1, n);

	// put the list into a global array
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	lua_insert(L, 1);
	int curSize = luaL_getn(L, 1);
	lua_rawseti(L, 1, curSize+1);

	// clean the stack
	lua_settop(L, 0);
}
void CallDeferredFunctions(lua_State* L, const char* idstring)
{
	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	int numCalls = luaL_getn(L, 1);
	for(int i = 1; i <= numCalls; i++)
	{
        lua_rawgeti(L, 1, i);  // get the function+arguments list
		int listSize = luaL_getn(L, 2);

		// push the arguments and the function
		for(int j = 1; j <= listSize; j++)
			lua_rawgeti(L, 2, j);

		// get and pop the function
		lua_CFunction cf = lua_tocfunction(L, -1);
		lua_pop(L, 1);

		// swap the arguments on the stack with the list we're iterating through
		// before calling the function, because C functions assume argument 1 is at 1 on the stack
		lua_pushvalue(L, 1);
		lua_remove(L, 2);
		lua_remove(L, 1);

		// call the function
		cf(L);
 
		// put the list back where it was
		lua_replace(L, 1);
		lua_settop(L, 1);
	}

	// clear the list of deferred functions
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, idstring);

	// clean the stack
	lua_settop(L, 0);
}

bool DeferGUIFuncIfNeeded(lua_State* L)
{
	LuaContextInfo& info = *luaStateToContextMap[L];
	if(info.speedMode == SPEEDMODE_MAXIMUM)
	{
		// if the mode is "maximum" then discard all GUI function calls
		// and pretend it was because we deferred them
		return true;
	}
	if(info.guiFuncsNeedDeferring)
	{
		// defer whatever function called this one until later
		DeferFunctionCall(L, deferredGUIIDString);
		return true;
	}

	// ok to run the function right now
	return false;
}

void worry(lua_State* L, int intensity)
{
	LuaContextInfo& info = *luaStateToContextMap[L];
	if(info.worryCount >= 0)
		info.worryCount += intensity;
}

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

static const char* toCString(lua_State* L)
{
	static const int maxLen = 1024;
	static char str[maxLen];
	str[0] = 0;
	char* ptr = str;

	int n=lua_gettop(L);
	int i;
	for (i=1; i<=n; i++)
	{
		//if (i>1)
		//	ptr += sprintf(ptr, "\t");
		if (lua_isstring(L,i))
			ptr += snprintf(ptr, maxLen, "%s",lua_tostring(L,i));
		else if (lua_isnil(L,i))
			ptr += snprintf(ptr, maxLen, "%s","nil");
		else if (lua_isboolean(L,i))
			ptr += snprintf(ptr, maxLen, "%s",lua_toboolean(L,i) ? "true" : "false");
		else
			ptr += snprintf(ptr, maxLen, "%s:%p",luaL_typename(L,i),lua_topointer(L,i)); // TODO: replace this with something more useful (even though this is the usual method, it doesn't show anything in the table)
	}
	ptr += snprintf(ptr, maxLen, "\r\n");
	return str;
}

static int gens_message(lua_State* L)
{
	const char* str = toCString(L);
	Put_Info((char*)str, 500);
	return 0;
}

static int print(lua_State* L)
{
	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = *luaContextInfo[uid];

	const char* str = toCString(L);

	if(info.print)
		info.print(uid, str);
	else
		puts(str);

	worry(L, 10);
	return 0;
}

static int bitand(lua_State *L)
{
	int rv = ~0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv &= luaL_checkinteger(L,i);
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}
static int bitor(lua_State *L)
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv |= luaL_checkinteger(L,i);
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}
static int bitxor(lua_State *L)
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv ^= luaL_checkinteger(L,i);
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}
static int bitshift(lua_State *L)
{
	int num = luaL_checkinteger(L,1);
	int shift = luaL_checkinteger(L,2);
	if(shift < 0)
		num <<= -shift;
	else
		num >>= shift;
	lua_settop(L,0);
	lua_pushinteger(L,num);
	return 1;
}


#define HOOKCOUNT 4096
#define MAX_WORRY_COUNT 600
void LuaRescueHook(lua_State* L, lua_Debug *dbg)
{
	LuaContextInfo& info = *luaStateToContextMap[L];

	if(info.worryCount < 0 && !info.panic)
		return;

	info.worryCount++;

	if(info.worryCount > MAX_WORRY_COUNT || info.panic)
	{
		info.worryCount = 0;

		int answer;
		if(info.panic)
		{
			answer = IDYES;
		}
		else
		{
			DialogsOpen++;
			answer = MessageBox(HWnd, "A Lua script has been running for quite a while. Maybe it is in an infinite loop.\n\nWould you like to stop the script?\n\n(Yes to stop it now,\n No to keep running and not ask again,\n Cancel to keep running but ask again later)", "Lua Alert", MB_YESNOCANCEL | MB_DEFBUTTON3 | MB_ICONASTERISK);
			DialogsOpen--;
		}

		if(answer == IDNO)
			info.worryCount = -1; // don't remove the hook because we need it still running for RequestAbortLuaScript to work

		if(answer == IDYES)
		{
			//lua_sethook(L, NULL, 0, 0);
			assert(L->errfunc || L->errorJmp);
			luaL_error(L, info.panic ? info.panicMessage : "terminated by user");
		}

		info.panic = false;
	}
}

int gens_emulateframe(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	Update_Emulation_One(HWnd);

	worry(L,30);
	return 0;
}

int gens_emulateframefastnoskipping(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	Update_Emulation_One_Before(HWnd);
	Update_Frame_Hook();
	Update_Emulation_After_Controlled(HWnd, true);

	worry(L,20);
	return 0;
}

int gens_emulateframefast(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	disableVideoLatencyCompensationCount = VideoLatencyCompensation + 1;

	Update_Emulation_One_Before(HWnd);

	if(FrameCount%16 == 0)
	{
		Update_Frame_Hook();
		Update_Emulation_After_Controlled(HWnd, true);
	}
	else
	{
		Update_Frame_Fast_Hook();
		Update_Emulation_After_Controlled(HWnd, false);
	}

	worry(L,15);
	return 0;
}

int gens_emulateframeinvisible(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	int oldDisableSound2 = disableSound2;
	int oldDisableRamSearchUpdate = disableRamSearchUpdate;
	disableSound2 = true;
	disableRamSearchUpdate = true;

	Update_Emulation_One_Before_Minimal();
	Update_Frame_Fast();
	UpdateLagCount();

	disableSound2 = oldDisableSound2;
	disableRamSearchUpdate = oldDisableRamSearchUpdate;

	// disable video latency compensation for a few frames
	// because it can get pretty slow if that's doing prediction updates every frame
	// when the lua script is also doing prediction updates
	disableVideoLatencyCompensationCount = VideoLatencyCompensation + 1;

	worry(L,10);
	return 0;
}

int gens_speedmode(lua_State* L)
{
	SpeedMode newSpeedMode = SPEEDMODE_NORMAL;
	if(lua_isnumber(L,1))
		newSpeedMode = (SpeedMode)luaL_checkinteger(L,1);
	else
	{
		const char* str = luaL_checkstring(L,1);
		if(!stricmp(str, "normal"))
			newSpeedMode = SPEEDMODE_NORMAL;
		else if(!stricmp(str, "nothrottle"))
			newSpeedMode = SPEEDMODE_NOTHROTTLE;
		else if(!stricmp(str, "turbo"))
			newSpeedMode = SPEEDMODE_TURBO;
		else if(!stricmp(str, "maximum"))
			newSpeedMode = SPEEDMODE_MAXIMUM;
	}

	LuaContextInfo& info = *luaStateToContextMap[L];
	info.speedMode = newSpeedMode;
	return 0;
}

int gens_wait(lua_State* L)
{
	LuaContextInfo& info = *luaStateToContextMap[L];

	switch(info.speedMode)
	{
		default:
		case SPEEDMODE_NORMAL:
			while(!Step_Gens_MainLoop(true, false) && !info.panic);
			break;
		case SPEEDMODE_NOTHROTTLE:
		case SPEEDMODE_TURBO:
		case SPEEDMODE_MAXIMUM:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			break;
	}
	return 0;
}

int gens_frameadvance(lua_State* L)
{
	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = *luaStateToContextMap[L];

	if(!info.ranFrameAdvance)
	{
		// otherwise we'll never see the first frame of GUI drawing
		if(info.speedMode != SPEEDMODE_MAXIMUM)
			Show_Genesis_Screen();
		info.ranFrameAdvance = true;
	}

	switch(info.speedMode)
	{
		default:
		case SPEEDMODE_NORMAL:
			while(!Step_Gens_MainLoop(true, true) && !info.panic);
			break;
		case SPEEDMODE_NOTHROTTLE:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			if(!(FastForwardKeyDown && (GetActiveWindow()==HWnd || BackgroundInput)))
				gens_emulateframefastnoskipping(L);
			else
				gens_emulateframefast(L);
			break;
		case SPEEDMODE_TURBO:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			gens_emulateframefast(L);
			break;
		case SPEEDMODE_MAXIMUM:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			gens_emulateframeinvisible(L);
			break;
	}
	return 0;
}

int gens_pause(lua_State* L)
{
	Paused = 1;
	gens_frameadvance(L);

	// allow the user to not have to manually unpause
	// after restarting a script that used gens.pause()
	LuaContextInfo& info = *luaStateToContextMap[L];
	if(info.panic)
		Paused = 0;

	return 0;
}





int memory_readbyte(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned char value = (unsigned char)(ReadValueAtHardwareAddress(address, 1) & 0xFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1; // we return the number of return values
}
int memory_readbytesigned(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	signed char value = (signed char)(ReadValueAtHardwareAddress(address, 1) & 0xFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned short value = (unsigned short)(ReadValueAtHardwareAddress(address, 2) & 0xFFFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readwordsigned(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	signed short value = (signed short)(ReadValueAtHardwareAddress(address, 2) & 0xFFFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readdword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned long value = (unsigned long)(ReadValueAtHardwareAddress(address, 4));
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readdwordsigned(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	signed long value = (signed long)(ReadValueAtHardwareAddress(address, 4));
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}

int memory_writebyte(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned char value = (unsigned char)(luaL_checkinteger(L,2) & 0xFF);
	WriteValueAtHardwareAdress(address, value, 1);
	return 0;
}
int memory_writeword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned short value = (unsigned short)(luaL_checkinteger(L,2) & 0xFFFF);
	WriteValueAtHardwareAdress(address, value, 2);
	return 0;
}
int memory_writedword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned long value = (unsigned long)(luaL_checkinteger(L,2));
	WriteValueAtHardwareAdress(address, value, 4);
	return 0;
}

int state_create(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
	{
		lua_pushnil(L);
		return 1;
	}

	int len = GENESIS_STATE_LENGTH;
	if (SegaCD_Started) len += SEGACD_LENGTH_EX;
	if (_32X_Started) len += G32X_LENGTH_EX;

	void* stateBuffer = lua_newuserdata(L, len);
	memset(stateBuffer, 0, len);

	return 1;
}
int state_get(lua_State* L)
{
	void* stateBuffer = lua_touserdata(L,1);

	if(stateBuffer)
		Save_State_To_Buffer((unsigned char*)stateBuffer);

	return 1;
}
int state_set(lua_State* L)
{
	void* stateBuffer = lua_touserdata(L,1);

	if(stateBuffer)
		Load_State_From_Buffer((unsigned char*)stateBuffer);

	return 0;
}
int state_save(lua_State* L)
{
	int stateNumber = luaL_checkinteger(L,1);
	Set_Current_State(stateNumber);
	Str_Tmp[0] = 0;
	Get_State_File_Name(Str_Tmp);
	Save_State(Str_Tmp);
	return 0;
}
int state_load(lua_State* L)
{
	int stateNumber = luaL_checkinteger(L,1);
	Set_Current_State(stateNumber);
	Str_Tmp[0] = 0;
	Get_State_File_Name(Str_Tmp);
	Load_State(Str_Tmp);
	return 0;
}

static const struct ButtonDesc
{
	unsigned short controllerNum;
	unsigned short bit;
	const char* name;
}
s_buttonDescs [] =
{
	{1, 0, "Up"},
	{1, 1, "Down"},
	{1, 2, "Left"},
	{1, 3, "Right"},
	{1, 4, "A"},
	{1, 5, "B"},
	{1, 6, "C"},
	{1, 7, "Start"},
	{1, 32, "X"},
	{1, 33, "Y"},
	{1, 34, "Z"},
	{1, 35, "Mode"},
	{2, 24, "Up"},
	{2, 25, "Down"},
	{2, 26, "Left"},
	{2, 27, "Right"},
	{2, 28, "A"},
	{2, 29, "B"},
	{2, 30, "C"},
	{2, 31, "Start"},
	{2, 36, "X"},
	{2, 37, "Y"},
	{2, 38, "Z"},
	{2, 39, "Mode"},
};


int joy_set(lua_State* L)
{
	int controllerNumber = luaL_checkinteger(L,1);

	luaL_checktype(L, 2, LUA_TTABLE);

	int input = ~0;
	int mask = 0;

	for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
	{
		const ButtonDesc& bd = s_buttonDescs[i];
		if(bd.controllerNum == controllerNumber)
		{
			lua_getfield(L, 2, bd.name);
			if (!lua_isnil(L,-1))
			{
				bool pressed = lua_toboolean(L,-1) != 0;
				int bitmask = ((long long)1 << bd.bit);
				if(pressed)
					input &= ~bitmask;
				else
					input |= bitmask;
				mask |= bitmask;
			}
			lua_pop(L,1);
		}
	}

	SetNextInputCondensed(input, mask);

	return 0;
}
int joy_get(lua_State* L)
{
	int controllerNumber = luaL_checkinteger(L,1);
	lua_newtable(L);

	long long input = GetCurrentInputCondensed();

	for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
	{
		const ButtonDesc& bd = s_buttonDescs[i];
		if(bd.controllerNum == controllerNumber)
		{
			bool pressed = (input & ((long long)1<<bd.bit)) == 0;
			lua_pushboolean(L, pressed);
			lua_setfield(L, -2, bd.name);
		}
	}

	return 1;
}

static const struct ColorMapping
{
	const char* name;
	int value;
}
s_colorMapping [] =
{
	{"white",     0xFFFFFFFF},
	{"black",     0x000000FF},
	{"clear",     0x00000000},
	{"gray",      0x7F7F7FFF},
	{"grey",      0x7F7F7FFF},
	{"red",       0xFF0000FF},
	{"orange",    0xFF7F00FF},
	{"yellow",    0xFFFF00FF},
	{"chartreuse",0x7FFF00FF},
	{"green",     0x00FF00FF},
	{"teal",      0x00FF7FFF},
	{"cyan" ,     0x00FFFFFF},
	{"blue",      0x0000FFFF},
	{"purple",    0x7F00FFFF},
	{"magenta",   0xFF00FFFF},
};

int getcolor(lua_State *L, int idx, int defaultColor)
{
	int type = lua_type(L,idx);
	switch(type)
	{
		case LUA_TSTRING:
		{
			const char* str = lua_tostring(L,idx);
			if(*str == '#')
			{
				int color;
				sscanf(str+1, "%X", &color);
				int len = strlen(str+1);
				int missing = max(0, 8-len);
				color <<= missing << 2;
				if(missing >= 2) color |= 0xFF;
				return color;
			}
			else for(int i = 0; i<sizeof(s_colorMapping)/sizeof(*s_colorMapping); i++)
			{
				if(!stricmp(str,s_colorMapping[i].name))
					return s_colorMapping[i].value;
			}
			if(!strnicmp(str, "rand", 4))
				return ((rand()*255/RAND_MAX) << 8) | ((rand()*255/RAND_MAX) << 16) | ((rand()*255/RAND_MAX) << 24) | 0xFF;
		}	break;
		case LUA_TNUMBER:
		{
			return lua_tointeger(L,idx);
		}	break;
	}
	return defaultColor;
}

int gui_text(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0; // we have to wait until later to call this function because gens hasn't emulated the next frame yet
		          // (the only way to avoid this deferring is to be in a gui.register or gens.registerafter callback)

	int x = luaL_checkinteger(L,1) & 0xFFFF;
	int y = luaL_checkinteger(L,2) & 0xFFFF;
	const char* str = luaL_checkstring(L,3);
	if(str && *str)
	{
		int foreColor = getcolor(L,4,0xFFFFFFFF);
		int backColor = getcolor(L,5,0x000000FF);
		PutText2(str, x, y, foreColor, backColor);
	}

	return 0;
}
int gui_box(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int x1 = luaL_checkinteger(L,1) & 0xFFFF;
	int y1 = luaL_checkinteger(L,2) & 0xFFFF;
	int x2 = luaL_checkinteger(L,3) & 0xFFFF;
	int y2 = luaL_checkinteger(L,4) & 0xFFFF;
	int fillcolor = getcolor(L,5,0xFFFFFF3F);
	int outlinecolor = getcolor(L,6,0xFFFFFFFF);

	DrawBoxPP2 (x1, y1, x2, y2, fillcolor, outlinecolor);

	return 0;
}
int gui_pixel(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int x = luaL_checkinteger(L,1) & 0xFFFF;
	int y = luaL_checkinteger(L,2) & 0xFFFF;
	int color = getcolor(L,3,0xFFFFFFFF);
	int color32 = color>>8;
	int color16 = DrawUtil::Pix32To16(color32);
	int Opac = color & 0xFF;

	Pixel(x, y, color32, color16, 0, Opac);

	return 0;
}
int gui_line(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int x1 = luaL_checkinteger(L,1) & 0xFFFF;
	int y1 = luaL_checkinteger(L,2) & 0xFFFF;
	int x2 = luaL_checkinteger(L,3) & 0xFFFF;
	int y2 = luaL_checkinteger(L,4) & 0xFFFF;
	int color = getcolor(L,5,0xFFFFFFFF);
	int color32 = color>>8;
	int color16 = DrawUtil::Pix32To16(color32);
	int Opac = color & 0xFF;

	DrawLine(x1, y1, x2, y2, color32, color16, 0, Opac);

	return 0;
}

int gens_getframecount(lua_State* L)
{
	lua_pushinteger(L, FrameCount);
	return 1;
}
int gens_getlagcount(lua_State* L)
{
	lua_pushinteger(L, LagCountPersistent);
	return 1;
}
int movie_getlength(lua_State* L)
{
	lua_pushinteger(L, MainMovie.LastFrame);
	return 1;
}
int movie_isactive(lua_State* L)
{
	lua_pushboolean(L, MainMovie.File != NULL);
	return 1;
}
int movie_rerecordcount(lua_State* L)
{
	lua_pushinteger(L, MainMovie.NbRerecords);
	return 1;
}
int movie_getreadonly(lua_State* L)
{
	lua_pushboolean(L, MainMovie.ReadOnly);
	return 1;
}
int movie_setreadonly(lua_State* L)
{
	bool readonly = lua_toboolean(L,1) != 0;
	MainMovie.ReadOnly = readonly;
	return 0;
}
int movie_isrecording(lua_State* L)
{
	lua_pushboolean(L, MainMovie.Status == MOVIE_RECORDING);
	return 1;
}
int movie_isplaying(lua_State* L)
{
	lua_pushboolean(L, MainMovie.Status == MOVIE_PLAYING);
	return 1;
}
int movie_getname(lua_State* L)
{
	lua_pushstring(L, MainMovie.FileName);
	return 1;
}

int sound_clear(lua_State* L)
{
	Clear_Sound_Buffer();
	return 0;
}

// resets our "worry" counter of the Lua state
int dontworry(lua_State* L)
{
	if(!L)
		return 0;
	LuaContextInfo& info = *luaStateToContextMap[L];
	info.worryCount = min(info.worryCount, 0);
	return 0;
}

static const struct luaL_reg genslib [] =
{
	{"frameadvance", gens_frameadvance},
	{"speedmode", gens_speedmode},
	{"wait", gens_wait},
	{"pause", gens_pause},
	{"emulateframe", gens_emulateframe},
	{"emulateframefastnoskipping", gens_emulateframefastnoskipping},
	{"emulateframefast", gens_emulateframefast},
	{"emulateframeinvisible", gens_emulateframeinvisible},
	{"framecount", gens_getframecount},
	{"lagcount", gens_getlagcount},
	{"registerbefore", gens_registerbefore},
	{"registerafter", gens_registerafter},
	{"registerexit", gens_registerexit},
	{"message", gens_message},
	{NULL, NULL}
};
static const struct luaL_reg guilib [] =
{
	{"register", gui_register},
	{"text", gui_text},
	{"box", gui_box},
	{"pixel", gui_pixel},
	{"line", gui_line},
	// alternative names
	{"drawtext", gui_text},
	{"drawbox", gui_box},
	{"drawpixel", gui_pixel},
	{"drawline", gui_line},
	{NULL, NULL}
};
static const struct luaL_reg statelib [] =
{
	{"create", state_create},
	{"set", state_set},
	{"get", state_get},
	{"save", state_save},
	{"load", state_load},
	{"registersave", state_registersave},
	{"registerload", state_registerload},
	{NULL, NULL}
};
static const struct luaL_reg memorylib [] =
{
	{"readbyte", memory_readbyte},
	{"readbyteunsigned", memory_readbyte},
	{"readbytesigned", memory_readbytesigned},
	{"readword", memory_readword},
	{"readwordunsigned", memory_readword},
	{"readwordsigned", memory_readwordsigned},
	{"readdword", memory_readdword},
	{"readdwordunsigned", memory_readdword},
	{"readdwordsigned", memory_readdwordsigned},
	{"writebyte", memory_writebyte},
	{"writeword", memory_writeword},
	{"writedword", memory_writedword},
	// alternate naming scheme for word and double-word
	{"readshort", memory_readword},
	{"readshortunsigned", memory_readword},
	{"readshortsigned", memory_readwordsigned},
	{"readlong", memory_readdword},
	{"readlongunsigned", memory_readdword},
	{"readlongsigned", memory_readdwordsigned},
	{"writeshort", memory_writeword},
	{"writelong", memory_writedword},

	//main 68000 memory hooks
	{"register", memory_registerM68Kwrite},
	{"registerwrite", memory_registerM68Kwrite},
	{"registerread", memory_registerM68Kread},
	{"registerexec", memory_registerM68Kexec},

	//full names for main 68000 memory hooks
	{"registerM68K", memory_registerM68Kwrite},
	{"registerM68Kwrite", memory_registerM68Kwrite},
	{"registerM68Kread", memory_registerM68Kread},
	{"registerM68Kexec", memory_registerM68Kexec},

	//alternate names for main 68000 memory hooks
	{"registergen", memory_registerM68Kwrite},
	{"registergenwrite", memory_registerM68Kwrite},
	{"registergenread", memory_registerM68Kread},
	{"registergenexec", memory_registerM68Kexec},

	//sub 68000 (segaCD) memory hooks
	{"registerS68K", memory_registerS68Kwrite},
	{"registerS68Kwrite", memory_registerS68Kwrite},
	{"registerS68Kread", memory_registerS68Kread},
	{"registerS68Kexec", memory_registerS68Kexec},

	//alternate names for sub 68000 (segaCD) memory hooks
	{"registerCD", memory_registerS68Kwrite},
	{"registerCDwrite", memory_registerS68Kwrite},
	{"registerCDread", memory_registerS68Kread},
	{"registerCDexec", memory_registerS68Kexec},

//	//Super-H 2 (32X) memory hooks
//	{"registerSH2", memory_registerSH2write},
//	{"registerSH2write", memory_registerSH2write},
//	{"registerSH2read", memory_registerSH2read},
//	{"registerSH2exec", memory_registerSH2PC},
//	{"registerSH2PC", memory_registerSH2PC},

//	//alternate names for Super-H 2 (32X) memory hooks
//	{"register32X", memory_registerSH2write},
//	{"register32Xwrite", memory_registerSH2write},
//	{"register32Xread", memory_registerSH2read},
//	{"register32Xexec", memory_registerSH2PC},
//	{"register32XPC", memory_registerSH2PC},

//	//Z80 (sound controller) memory hooks
//	{"registerZ80", memory_registerZ80write},
//	{"registerZ80write", memory_registerZ80write},
//	{"registerZ80read", memory_registerZ80read},
//	{"registerZ80PC", memory_registerZ80PC},

	{NULL, NULL}
};
static const struct luaL_reg joylib [] =
{
	{"get", joy_get},
	{"set", joy_set},
	{"read", joy_get},
	{"write", joy_set},
	{NULL, NULL}
};
static const struct luaL_reg movielib [] =
{
	{"length", movie_getlength},
	{"active", movie_isactive},
	{"recording", movie_isrecording},
	{"playing", movie_isplaying},
	{"name", movie_getname},
	{"getname", movie_getname},
	{"rerecordcount", movie_rerecordcount},
	{"readonly", movie_getreadonly},
	{"getreadonly", movie_getreadonly},
	{"setreadonly", movie_setreadonly},
	{NULL, NULL}
};
static const struct luaL_reg soundlib [] =
{
	{"clear", sound_clear},
	{NULL, NULL}
};

void OpenLuaContext(int uid, void(*print)(int uid, const char* str), void(*onstart)(int uid), void(*onstop)(int uid))
{
	LuaContextInfo* newInfo = new LuaContextInfo();
	newInfo->started = false;
	newInfo->running = false;
	newInfo->returned = false;
	newInfo->crashed = false;
	newInfo->restart = false;
	newInfo->restartLater = false;
	newInfo->worryCount = 0;
	newInfo->panic = false;
	newInfo->ranExit = false;
	newInfo->guiFuncsNeedDeferring = false;
	newInfo->ranFrameAdvance = false;
	newInfo->speedMode = SPEEDMODE_NORMAL;
	newInfo->print = print;
	newInfo->onstart = onstart;
	newInfo->onstop = onstop;
	newInfo->L = NULL;
	luaContextInfo[uid] = newInfo;
}

void RefreshScriptStartedStatus();

void RunLuaScriptFile(int uid, const char* filenameCStr)
{
	StopLuaScript(uid);

	LuaContextInfo& info = *luaContextInfo[uid];

	if(info.running)
	{
		// it's a little complicated, but... the call to luaL_dofile below
		// could call a C function that calls this very function again
		// additionally, if that happened then the above call to StopLuaScript
		// probably couldn't stop the script yet, so instead of continuing,
		// we'll set a flag that tells the first call of this function to loop again
		// when the script is able to stop safely
		info.restart = true;
		return;
	}

	std::string filename = filenameCStr;

	do
	{
		lua_State* L = lua_open();
		luaStateToContextMap[L] = &info;
		luaStateToUIDMap[L] = uid;
		info.L = L;
		info.worryCount = 0;
		info.panic = false;
		info.ranExit = false;
		info.crashed = false;
		info.guiFuncsNeedDeferring = true;
		info.ranFrameAdvance = false;
		info.restart = false;
		info.restartLater = false;
		info.speedMode = SPEEDMODE_NORMAL;
		info.lastFilename = filename;

		luaL_openlibs(L);
		luaL_register(L, "gens", genslib);
		luaL_register(L, "gui", guilib);
		luaL_register(L, "savestate", statelib);
		luaL_register(L, "memory", memorylib);
		luaL_register(L, "joypad", joylib);
		luaL_register(L, "movie", movielib);
		luaL_register(L, "sound", soundlib);
		lua_register(L, "print", print);
		lua_register(L, "AND", bitand);
		lua_register(L, "OR", bitor);
		lua_register(L, "XOR", bitxor);
		lua_register(L, "SHIFT", bitshift);

		// register a function to periodically check for inactivity
		lua_sethook(L, LuaRescueHook, LUA_MASKCOUNT, HOOKCOUNT);

		// deferred evaluation table
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, deferredGUIIDString);

		info.started = true;
		RefreshScriptStartedStatus();
		if(info.onstart)
			info.onstart(uid);
		info.running = true;
		info.returned = false;
		int errorcode = luaL_dofile(L,filename.c_str());
		info.running = false;
		info.returned = true;

		if (errorcode)
		{
			info.crashed = true;
			if(info.print)
			{
				info.print(uid, lua_tostring(L,-1));
				info.print(uid, "\r\n");
			}
			else
			{
				fprintf(stderr, "%s\n", lua_tostring(L,-1));
			}
			StopLuaScript(uid);
		}
		else
		{
			Show_Genesis_Screen();
			StopScriptIfFinished(uid, true);
		}
	} while(info.restart);
}

void StopScriptIfFinished(int uid, bool justReturned)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	if(!info.returned)
		return;

	// the script has returned, but it is not necessarily done running
	// because it may have registered a function that it expects to keep getting called
	// so check if it has any registered functions and stop the script only if it doesn't

	bool keepAlive = false;
	for(int calltype = 0; calltype < LUACALL_COUNT; calltype++)
	{
		if(calltype == LUACALL_BEFOREEXIT) // this is probably the only one that shouldn't keep the script alive
			continue;

		lua_State* L = info.L;
		if(L)
		{
			const char* idstring = luaCallIDStrings[calltype];
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			bool isFunction = lua_isfunction(L, -1);
			lua_pop(L, 1);

			if(isFunction)
			{
				keepAlive = true;
				break;
			}
		}
	}

	if(keepAlive)
	{
		if(justReturned)
		{
			if(info.print)
				info.print(uid, "script returned but is still running registered functions\r\n");
			else
				fprintf(stderr, "%s\n", "script returned but is still running registered functions");
		}
	}
	else
	{
		if(info.print)
			info.print(uid, "script finished running\r\n");
		else
			fprintf(stderr, "%s\n", "script finished running");

		StopLuaScript(uid);
	}
}

void RequestAbortLuaScript(int uid, const char* message)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(L)
	{
		// this probably isn't the right way to do it
		// but calling luaL_error here is positively unsafe
		// (it seemingly works fine but sometimes corrupts the emulation state in colorful ways)
		// and this works pretty well and is definitely safe, so screw it
		info.L->hookcount = 1; // run hook function as soon as possible
		info.panic = true; // and call luaL_error once we're inside the hook function
		if(message)
		{
			strncpy(info.panicMessage, message, sizeof(info.panicMessage));
			info.panicMessage[sizeof(info.panicMessage)-1] = 0;
		}
		else
		{
			strcpy(info.panicMessage, "script terminated");
		}
	}
}

void CallExitFunction(int uid)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;

	if(!L)
		return;

	dontworry(L);

	// first call the registered exit function if there is one
	if(!info.ranExit)
	{
		info.ranExit = true;

		lua_settop(L, 0);
		lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
		
		if (lua_isfunction(L, -1))
		{
			bool wasRunning = info.running;
			info.running = true;
			int errorcode = lua_pcall(L, 0, 0, 0);
			info.running = wasRunning;
			if (errorcode)
			{
				info.crashed = true;
				if(L->errfunc || L->errorJmp)
					luaL_error(L, lua_tostring(L,-1));
				else if(info.print)
				{
					info.print(uid, lua_tostring(L,-1));
					info.print(uid, "\r\n");
				}
				else
				{
					fprintf(stderr, "%s\n", lua_tostring(L,-1));
				}
			}
		}
	}
}

void StopLuaScript(int uid)
{
	LuaContextInfo& info = *luaContextInfo[uid];

	if(info.running)
	{
		// if it's currently running then we can't stop it now without crashing
		// so the best we can do is politely request for it to go kill itself
		RequestAbortLuaScript(uid);
		return;
	}

	lua_State* L = info.L;
	if(L)
	{
		CallExitFunction(uid);

		if(info.onstop && !info.crashed)
			info.onstop(uid); // must happen before closing L and after the exit function, otherwise the final GUI state of the script won't be shown properly or at all

		if(info.started) // this check is necessary
		{
			lua_close(L);
			luaStateToContextMap.erase(L);
			luaStateToUIDMap.erase(L);
			info.L = NULL;
			info.started = false;
		}
		RefreshScriptStartedStatus();
	}
}

void CloseLuaContext(int uid)
{
	StopLuaScript(uid);
	delete luaContextInfo[uid];
	luaContextInfo.erase(uid);
}


void CallRegisteredLuaFunctions(LuaCallID calltype)
{
	const char* idstring = luaCallIDStrings[calltype];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L && (!info.panic || calltype == LUACALL_BEFOREEXIT))
		{
			// handle deferred GUI function calls and disabling deferring when unnecessary
			if(calltype == LUACALL_AFTEREMULATIONGUI || calltype == LUACALL_AFTEREMULATION)
				info.guiFuncsNeedDeferring = false;
			if(calltype == LUACALL_AFTEREMULATIONGUI)
				CallDeferredFunctions(L, deferredGUIIDString);

			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				int errorcode = lua_pcall(L, 0, 0, 0);
				info.running = wasRunning;
				if (errorcode)
				{
					info.crashed = true;
					if(L->errfunc || L->errorJmp)
						luaL_error(L, lua_tostring(L,-1));
					else
					{
						if(info.print)
						{
							info.print(uid, lua_tostring(L,-1));
							info.print(uid, "\r\n");
						}
						else
						{
							fprintf(stderr, "%s\n", lua_tostring(L,-1));
						}
						StopLuaScript(uid);
					}
				}
			}
			else
			{
				lua_pop(L, 1);
			}

			info.guiFuncsNeedDeferring = true;
		}

		++iter;
	}
}

void CallRegisteredLuaFunctionsWithArg(LuaCallID calltype, int arg)
{
	const char* idstring = luaCallIDStrings[calltype];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L)
		{
			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				lua_pushinteger(L, arg);
				int errorcode = lua_pcall(L, 1, 0, 0);
				info.running = wasRunning;
				if (errorcode)
				{
					info.crashed = true;
					if(L->errfunc || L->errorJmp)
						luaL_error(L, lua_tostring(L,-1));
					else
					{
						if(info.print)
						{
							info.print(uid, lua_tostring(L,-1));
							info.print(uid, "\r\n");
						}
						else
						{
							fprintf(stderr, "%s\n", lua_tostring(L,-1));
						}
						StopLuaScript(uid);
					}
				}
			}
			else
			{
				lua_pop(L, 1);
			}
		}

		++iter;
	}
}

void DontWorryLua() // everything's going to be OK
{
	std::map<lua_State*, int>::const_iterator iter = luaStateToUIDMap.begin();
	std::map<lua_State*, int>::const_iterator end = luaStateToUIDMap.end();
	while(iter != end)
	{
		lua_State* L = iter->first;
		dontworry(L);
		++iter;
	}
}

void StopAllLuaScripts()
{
	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		bool wasStarted = info.started;
		StopLuaScript(uid);
		info.restartLater = wasStarted;
		++iter;
	}
}

void RestartAllLuaScripts()
{
	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		if(info.restartLater || info.started)
		{
			info.restartLater = false;
			RunLuaScriptFile(uid, info.lastFilename.c_str());
		}
		++iter;
	}
}

// sets anything that needs to depend on the total number of scripts running
void RefreshScriptStartedStatus()
{
	int numScriptsStarted = 0;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.started)
			numScriptsStarted++;
		++iter;
	}

	frameadvSkipLagForceDisable = (numScriptsStarted != 0); // disable while scripts are running because currently lag skipping makes lua callbacks get called twice per frame advance
	g_numScriptsStarted = numScriptsStarted;
}

