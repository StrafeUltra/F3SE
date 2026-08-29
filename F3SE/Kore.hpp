#pragma once
#include <cstdint>
#include <string_view>


/*
* Kore.hpp contains RVAs, structs, and types that belong to Fable III's Kore VM Lua implementation
* Along with function definitions for internal Lua C API that we need (and definitions for functions we implement ourselves)
*/

namespace Kore
{
	struct lua_State;

	constexpr std::uintptr_t luaL_loadbufferRVA = 0x204CF0;
	constexpr std::uintptr_t lua_resumeRVA = 0x1ECAD0;
	constexpr std::uintptr_t lua_newthreadRVA = 0x1DEAD0;
	constexpr std::uintptr_t luaF_newCclosureRVA = 0x1FA400;
	constexpr std::uintptr_t luaL_refRVA = 0x1E18A0;
	constexpr std::uintptr_t luaL_unrefRVA = 0x1E1960;
	constexpr std::uintptr_t lua_tolstringRVA = 0x1EF090;
	constexpr std::uintptr_t luaL_errorRVA = 0x1E2580;
	constexpr std::uintptr_t SetLuaStateNameRVA = 0x1EF690;
	constexpr std::uintptr_t luaK_setfenvRVA = 0x1F9AE0;
	constexpr std::uintptr_t lua_createtableRVA = 0x1DEC20;
	constexpr std::uintptr_t luaD_growstackRVA = 0x1DD730;
	constexpr std::uintptr_t lua_pcallRVA = 0x1EC960;
	constexpr std::uintptr_t lua_callRVA = 0x5EC940;
	constexpr std::uintptr_t luaS_newlstrRVA = 0x7070;
	constexpr std::uintptr_t luaV_gettableRVA = 0x1E4EE0;
	constexpr std::uintptr_t luaD_throwRVA = 0x1F1C30;
	constexpr std::uintptr_t luaV_settableRVA = 0x1E9150;


	constexpr std::int32_t LUA_GLOBALSINDEX = -10002;
	constexpr std::int32_t LUA_REGISTRYINDEX = -10000;
	constexpr std::int32_t LUA_ENVIRONINDEX = -10001;

	constexpr std::int32_t LUA_MULTRET = -1;
	constexpr std::int32_t LUA_OK = 0;
	constexpr std::int32_t LUA_YIELD = -2;
	constexpr std::int32_t LUA_ERROR = -100;
	constexpr std::int32_t LUA_REFNIL = -1;

	using lua_Number = float; // Kore uses float rather then double
	using lua_Integer = std::int32_t;

	union Value
	{
		void* gc;
		void* p;
		lua_Number n;
		bool b;
	};

	struct TValue
	{
		Value value;
		int tt;
	};

	using StkId = TValue*;

	struct TString
	{
		std::uint32_t LenAndFlags; // 0x00
		std::uint32_t HashOrExtra; // 0x04
		char data[1]; // 0x08;
	};

	enum LuaTypes : std::int32_t
	{
		LUA_TNONE = -1,
		LUA_TNIL,
		LUA_TBOOLEAN,
		LUA_TLIGHTUSERDATA,
		LUA_TNUMBER,
		LUA_TSTRING,
		LUA_TTABLE,
		LUA_TFUNCTION, // nothing is actually set to this its just a return type for certain functions to generalize
		LUA_TUSERDATA,
		LUA_TTHREAD,
		LUA_TLFUNCTION,
		LUA_TCFUNCTION,
		LUA_TUI64,
		LUA_TSTRUCT
	};

	enum LuaGCWhat : std::int32_t
	{
		LUA_GCSTOP = 0,
		LUA_GCRESTART,
		LUA_GCCOLLECT,
		LUA_GCCOUNT,
		LUA_GCCOUNTB,
		LUA_GCSTEP,
		LUA_GCSETPAUSE,
		LUA_GCSETSTEPMUL
	};

	using lua_Hook = void(__cdecl*)(lua_State* L, void* ar);

	using lua_CFunction = std::int32_t(__cdecl*)(lua_State* L);

	/*
	* GCHeader is behind the actual Lua object pointer
	* It is at object - 0x8 for these things:
	* Lua State/Threads, Tables, Structs, Closures
	* 
	* It is at object - 0x4 for these things:
	* Strings, Userdata
	*/

	struct GCHeader
	{
		std::uint32_t bits;
		std::uint32_t color;
	};


	struct lua_longjmp
	{

		alignas(4) std::uint8_t jmp[0x40];
		lua_longjmp* previous; // 0x40
	};

	struct stringtable
	{
		TString** hash; // 0x00
		std::int32_t nuse; // 0x04
		std::int32_t mask; // 0x08
	};

	struct Table
	{
		Table* metatable; // 0x00
		std::uint32_t nodemask; // 0x04
		void* node; // 0x08
		StkId array; // 0x0C
		std::int32_t sizearray; // 0x10
		std::int32_t nhash; // 0x14
		std::uint32_t stamp; // 0x18
	};

	struct CallInfo
	{
		StkId base; // 0x00
		std::int32_t savedpc; // 0x04
		std::int16_t nlevels; // 0x08
		std::uint16_t nvarargs; // 0x0A
		std::int32_t nresults; // 0x0C
	};

	struct LocVar
	{
		TString* varname; // 0x00
		std::int32_t startpc; // 0x04
		std::int32_t endpc; // 0x08
	};

	struct Proto
	{
		std::uint8_t nups; // 0x00
		std::uint8_t numparams; // 0x01
		std::uint8_t is_vararg; // 0x02
		std::uint8_t maxstacksize; // 0x03
		std::uint32_t sizecode; // 0x04
		std::uint32_t* code; // 0x08
		std::uint32_t sizek; // 0x0C
		StkId k; // 0x10
		std::uint32_t sizep; // 0x14
		Proto** p; // 0x18
		std::int32_t linedefined; // 0x1C
		std::int32_t lastlinedefined; // 0x20
		std::uint32_t sizelineinfo; // 0x24
		std::int32_t* lineinfo; // 0x28
		std::uint32_t sizeupvalues; // 0x2C
		TString** upvalues; // 0x30
		TString* source; // 0x34
		TString* source2; // 0x38
		std::uint32_t sizelocvars; // 0x3C
		LocVar* locvars; // 0x40

	};

	struct Closure
	{
		union // 0x00
		{
			lua_CFunction f;
			Proto* p;
		};

		void* env; // 0x04

		union // starting here is where members differ between LClosures and CClosures
		{
			struct // CClosure path
			{
				std::uint16_t nupvalues; // 0x08
				std::uint16_t flags; // 0x0A
				TValue upvalue[]; // 0x0C
			} c;

			struct // LClosure path
			{
				std::uint8_t reserved; // 0x08
				std::uint8_t maxstacksize; // 0x09
				std::uint8_t is_vararg; // 0x0A
				std::uint8_t numparams; // 0x0B
				StkId k; // 0x0C
				std::uint32_t* code; // 0x10
				void* upvals[];
			} l;
		};
	};

	struct global_State
	{
		std::uint8_t unk_00[4];
		std::uint32_t totalbytes;     // 0x04  lua_gc count; create stores 1032 first
		std::uint32_t mem_limit;      // 0x08  stats: max KB = this >> 10
		void* frealloc;       // 0x0C
		void* ud;             // 0x10
		std::uint8_t unk_14[0x1C];   // 0x14 .. 0x2F
		lua_State* main_L;         // 0x30  (G+48) set at create
		std::uint8_t unk_34[8];      // 0x34 .. 0x3B
		std::int32_t gcstat;         // 0x3C  (G+60)
		std::uint8_t unk_40[0x48];   // 0x40 .. 0x87
		std::int32_t gcpause;        // 0x88  (G+136)
		std::int32_t gcstepmul;      // 0x8C  (G+140)
		std::uint8_t gcstopped;      // 0x90  (G+144)
		std::uint8_t pad_91[3];
		std::int32_t(*gc_break)(void*); // 0x94 (G+148)
		std::int32_t gc_copy_98;     // 0x98  (G+152) stepmul copy
		std::int32_t gcstepmul2;     // 0x9C  (G+156)
		std::uint8_t unk_A0[4];
		std::int32_t unk_A4;         // 0xA4  create writes 317
		std::uint8_t unk_A8[0x14];
		std::int32_t unk_B8;         // 0xB8  (G+184)
		stringtable strt;           // 0xBC  hash, nuse, mask
		std::int32_t strt_spare;     // 0xC8  (G+200) create = 0
		TValue l_registry;     // 0xCC  value @204, tt @208
		void* struct_types;   // 0xD4  (G+212)
		std::uint16_t struct_n;       // 0xD8  create = 0
		std::uint16_t struct_cap;     // 0xDA  create = 16
		void* extra_table;    // 0xDC  (G+220)
		std::uint8_t unk_E0[4];
		lua_State* mainthread;     // 0xE4  (G+228)

		std::uint8_t unk_E8[0x1C0];  // 0xE8 .. 0x2A7

		std::uint32_t gc_time_lo;     // 0x2A8  (G+680)
		std::uint32_t gc_time_hi;     // 0x2AC  (G+684)
		std::uint8_t gc_flag_2B0;    // 0x2B0  (G+688)
		std::uint8_t pad_2B1[3];
		void (*log)(lua_State*, const char*, ...); // 0x2B4 (G+692)

		std::uint8_t tail[1032 - 0x2B8];
	};

	struct lua_State
	{
		global_State* l_G; // 0x00 (global_State*)
		CallInfo* ci_base; // 0x04
		CallInfo* ci_end; // 0x08
		CallInfo* ci; // 0x0C
		std::uint8_t unk_10[8]; // 0x10
		std::int32_t errfunc; // 0x18 (-2 at create)
		StkId top; // 0x1C
		StkId base; // 0x20
		StkId stack_last; // 0x24
		StkId stack; // 0x28
		void* openupval; // 0x2C
		TValue l_gt; // 0x30
		TValue env; // 0x38
		lua_longjmp* errorJmp; // 0x40
		std::int32_t nCcalls; // 0x44
		TString* name; // 0x48
		lua_State* next_thread; // 0x4C
		std::uint8_t unk_50[4]; // 0x50
		std::int32_t status; // 0x54
		lua_Hook hook; // 0x58
		std::int32_t hookmask; // 0x5C (1 = c, 2 = r, 4 = l, 8 = count)
		std::int32_t hookcount; // 0x60
		std::int32_t hook_unk_64; // 0x64 (always set to 0?)
		std::uint32_t unk_68; // 0x68 todo findout what is here
		std::int32_t basehookcount; // 0x6C
	};

	using luaL_loadbufferFunc = std::int32_t(__cdecl*)(lua_State* L, const char* buff, std::size_t sz, const char* name);
	extern luaL_loadbufferFunc luaL_loadbuffer;

	using lua_newthreadFunc = lua_State * (__cdecl*)(lua_State* L);
	extern lua_newthreadFunc lua_newthread;

	using lua_resumeFunc = std::int32_t(__cdecl*)(lua_State* L, std::int32_t narg);
	extern lua_resumeFunc lua_resume;

	using luaL_refFunc = std::int32_t(__cdecl*)(lua_State* L, std::int32_t t);
	extern luaL_refFunc luaL_ref;

	using luaL_unrefFunc = void(__cdecl*)(lua_State* L, std::int32_t t, std::int32_t ref);
	extern luaL_unrefFunc luaL_unref;

	using luaV_tolstringFunc = const char* (__cdecl*)(lua_State* L, StkId obj, std::size_t* len);
	extern luaV_tolstringFunc luaV_tolstring;

	using luaL_errorFunc = std::int32_t(__cdecl*)(lua_State* L, const char* fmt, ...);
	extern luaL_errorFunc luaL_error;

	using SetLuaStateNameFunc = std::int32_t* (__cdecl*)(lua_State* L, const char* name);
	extern SetLuaStateNameFunc lua_SetLuaStateName;

	using luaK_setfenvFunc = std::int32_t(__cdecl*)(lua_State* L, StkId o, StkId table);
	extern luaK_setfenvFunc luaK_setfenv;

	using lua_createtableFunc = Table * (__cdecl*)(lua_State* L, std::int32_t narr, std::int32_t nrec);
	extern lua_createtableFunc lua_createtable;

	using luaD_growstackFunc = std::int32_t(__cdecl*)(CallInfo** cibase, lua_State* L, std::int32_t extra);
	extern luaD_growstackFunc luaD_growstack;

	using lua_pcallFunc = std::int32_t(__cdecl*)(lua_State* L, std::int32_t nargs, std::int32_t nresults, std::int32_t errfunc);
	extern lua_pcallFunc lua_pcall;

	using lua_callFunc = void(__cdecl*)(lua_State* L, std::int32_t nargs, std::int32_t nresults);
	extern lua_callFunc lua_call;

	using luaS_newlstrFunc = TString * (__cdecl*)(lua_State* L, const char* str, size_t l);
	extern luaS_newlstrFunc luaS_newlstr;

	using luaV_gettableFunc = TValue(__cdecl*)(lua_State* L, void* tableobj, std::uint32_t tablett, Value keyval, std::uint32_t keytt);
	extern luaV_gettableFunc luaV_gettable;

	using luaF_newCclosureFunc = StkId(__cdecl*)(lua_State* L, lua_CFunction fn, std::int32_t n, std::int32_t flags);
	extern luaF_newCclosureFunc luaF_newCclosure;

	using luaD_throwFunc = std::int32_t(__cdecl*)(lua_State* L, std::int32_t err);
	extern luaD_throwFunc luaD_throw;

	using luaV_settableFunc = std::int32_t(__cdecl*)(lua_State* L, StkId table, StkId key, StkId val);
	extern luaV_settableFunc luaV_settable;

	void InitializeFunctions(std::uintptr_t BaseVA);

	GCHeader* GetGC8(void* obj);

	GCHeader* GetGC4(void* obj);

	std::int32_t lua_gettop(lua_State* L);

	void lua_settop(lua_State* L, std::int32_t idx);

	void lua_pop(lua_State* L, std::int32_t n);

	TValue* index2adr(lua_State* L, std::int32_t idx);

	const char* lua_tolstring(lua_State* L, std::int32_t idx, std::size_t* len);

	const char* lua_tostring(lua_State* L, std::int32_t idx);

	lua_Number lua_tonumber(lua_State* L, std::int32_t idx);

	void lua_sethook(lua_State* L, lua_Hook func, std::int32_t mask, std::int32_t count);

	std::string_view lua_GetLuaStateName(lua_State* L);

	std::int32_t lua_type(lua_State* L, std::int32_t idx);

	void lua_pushvalue(lua_State* L, std::int32_t idx);

	bool lua_isnil(lua_State* L, std::int32_t idx);

	std::int32_t lua_setfenv(lua_State* L, std::int32_t idx);

	Table* lua_newtable(lua_State* L);

	void lua_remove(lua_State* L, std::int32_t idx);

	void lua_xmove(lua_State* from, lua_State* to, std::int32_t n);

	void lua_pushnil(lua_State* L);

	std::int32_t lua_checkstack(lua_State* L, std::int32_t extra);

	void lua_replace(lua_State* L, std::int32_t idx);

	bool lua_toboolean(lua_State* L, std::int32_t idx);

	std::int32_t lua_upvalueindex(std::int32_t i);

	lua_State* lua_tothread(lua_State* L, std::int32_t idx);

	void lua_insert(lua_State* L, std::int32_t idx);

	void lua_getfield(lua_State* L, std::int32_t idx, const char* k);

	void lua_gettable(lua_State* L, std::uint32_t idx);

	StkId lua_pushcclosure(lua_State* L, lua_CFunction fn, std::int32_t n);

	StkId lua_pushcfunction(lua_State* L, lua_CFunction fn);

	void lua_getglobal(lua_State* L, const char* k);

	void lua_setglobal(lua_State* L, const char* k);

	void lua_pushlstring(lua_State* L, const char* s, std::size_t len);

	void lua_pushstring(lua_State* L, const char* s);

	std::int32_t lua_error(lua_State* L);

	std::int32_t abs_index(lua_State* L, std::int32_t idx);

	void lua_settable(lua_State* L, std::int32_t idx);

	void lua_setfield(lua_State* L, std::int32_t idx, const char* k);

	std::int32_t lua_yield(lua_State* L, std::int32_t nresults);

	void lua_pushnumber(lua_State* L, lua_Number n);

	void lua_pushboolean(lua_State* L, std::int32_t b);
}