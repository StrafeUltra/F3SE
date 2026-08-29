#include "Kore.hpp"

/*
* Kore.cpp contains resolution for the internal Lua C API functions we depend on
* Along with the implementations of our own versions of some of the Lua C API functions and helpers to get GC header
*/


namespace Kore
{
	luaL_loadbufferFunc luaL_loadbuffer = nullptr;
	lua_newthreadFunc lua_newthread = nullptr;
	lua_resumeFunc lua_resume = nullptr;
	luaL_refFunc luaL_ref = nullptr;
	luaL_unrefFunc luaL_unref = nullptr;
	luaV_tolstringFunc luaV_tolstring = nullptr;
	luaL_errorFunc luaL_error = nullptr;
	SetLuaStateNameFunc lua_SetLuaStateName = nullptr;
	luaK_setfenvFunc luaK_setfenv = nullptr;
	lua_createtableFunc lua_createtable = nullptr;
	luaD_growstackFunc luaD_growstack = nullptr;
	lua_pcallFunc lua_pcall = nullptr;
	lua_callFunc lua_call = nullptr;
	luaS_newlstrFunc luaS_newlstr = nullptr;
	luaV_gettableFunc luaV_gettable = nullptr;
	luaF_newCclosureFunc luaF_newCclosure = nullptr;
	luaD_throwFunc luaD_throw = nullptr;
	luaV_settableFunc luaV_settable = nullptr;

	void InitializeFunctions(std::uintptr_t BaseVA)
	{
		luaL_loadbuffer = reinterpret_cast<luaL_loadbufferFunc>(BaseVA + luaL_loadbufferRVA);
		lua_newthread = reinterpret_cast<lua_newthreadFunc>(BaseVA + lua_newthreadRVA);
		lua_resume = reinterpret_cast<lua_resumeFunc>(BaseVA + lua_resumeRVA);
		luaL_ref = reinterpret_cast<luaL_refFunc>(BaseVA + luaL_refRVA);
		luaL_unref = reinterpret_cast<luaL_unrefFunc>(BaseVA + luaL_unrefRVA);
		luaV_tolstring = reinterpret_cast<luaV_tolstringFunc>(BaseVA + lua_tolstringRVA);
		luaL_error = reinterpret_cast<luaL_errorFunc>(BaseVA + luaL_errorRVA);
		lua_SetLuaStateName = reinterpret_cast<SetLuaStateNameFunc>(BaseVA + SetLuaStateNameRVA);
		luaK_setfenv = reinterpret_cast<luaK_setfenvFunc>(BaseVA + luaK_setfenvRVA);
		lua_createtable = reinterpret_cast<lua_createtableFunc>(BaseVA + lua_createtableRVA);
		luaD_growstack = reinterpret_cast<luaD_growstackFunc>(BaseVA + luaD_growstackRVA);
		lua_pcall = reinterpret_cast<lua_pcallFunc>(BaseVA + lua_pcallRVA);
		lua_call = reinterpret_cast<lua_callFunc>(BaseVA + lua_callRVA);
		luaS_newlstr = reinterpret_cast<luaS_newlstrFunc>(BaseVA + luaS_newlstrRVA);
		luaV_gettable = reinterpret_cast<luaV_gettableFunc>(BaseVA + luaV_gettableRVA);
		luaF_newCclosure = reinterpret_cast<luaF_newCclosureFunc>(BaseVA + luaF_newCclosureRVA);
		luaD_throw = reinterpret_cast<luaD_throwFunc>(BaseVA + luaD_throwRVA);
		luaV_settable = reinterpret_cast<luaV_settableFunc>(BaseVA + luaV_settableRVA);
	}

	GCHeader* GetGC8(void* obj)
	{
		return reinterpret_cast<GCHeader*>(static_cast<std::uint8_t*>(obj) - 0x8);
	}

	GCHeader* GetGC4(void* obj)
	{
		return reinterpret_cast<GCHeader*>(static_cast<std::uint8_t*>(obj) - 0x4);
	}

	std::int32_t lua_gettop(lua_State* L)
	{
		return static_cast<std::int32_t>(L->top - L->base);
	}

	void lua_settop(lua_State* L, std::int32_t idx)
	{
		if (idx >= 0)
		{
			StkId NewTop = L->base + idx;
			if (NewTop > L->stack_last) { return; }

			while (L->top < NewTop)
			{
				L->top->tt = LUA_TNIL;
				L->top->value.p = nullptr;
				L->top++;
			}

			while (L->top > NewTop)
			{
				L->top--;
				L->top->tt = LUA_TNIL;
				L->top->value.p = nullptr;
			}
		}
		else
		{
			StkId NewTop = L->top + (idx + 1);
			if (NewTop < L->base) { NewTop = L->base; }

			while (L->top > NewTop)
			{
				L->top--;
				L->top->tt = LUA_TNIL;
				L->top->value.p = nullptr;
			}
		}
	}

	void lua_pop(lua_State* L, std::int32_t n)
	{
		lua_settop(L, -(n)-1);
	}

	StkId index2adr(lua_State* L, std::int32_t idx)
	{
		if (idx == 0) return nullptr;

		if (idx > 0)
		{
			StkId v = L->base + (idx - 1);
			if (v < L->top) return v;
			return nullptr;
		}

		if (idx > LUA_REGISTRYINDEX)
		{
			StkId v = L->top + idx;
			if (v >= L->base) return v;
			return nullptr;
		}

		if (idx == LUA_REGISTRYINDEX)
			return &L->l_G->l_registry;

		if (idx == LUA_GLOBALSINDEX)
			return &L->l_gt;

		StkId FuncSlot = L->base - 1;
		if (!FuncSlot)
			return nullptr;

		if (idx == LUA_ENVIRONINDEX)
		{
			Closure* cl = reinterpret_cast<Closure*>(FuncSlot->value.gc);

			L->env.value.p = cl->env;
			L->env.tt = LUA_TTABLE;
			return &L->env;
		}

		if (FuncSlot && (FuncSlot->tt == LUA_TCFUNCTION))
		{
			Closure* cl = reinterpret_cast<Closure*>(FuncSlot->value.gc);
			const std::int32_t upvalueidx = LUA_GLOBALSINDEX - idx;

			if (upvalueidx <= cl->c.nupvalues)
				return &cl->c.upvalue[upvalueidx - 1];
		}

		return nullptr;
	}
	
	const char* lua_tolstring(lua_State* L, std::int32_t idx, std::size_t* len)
	{
		StkId o = index2adr(L, idx);
		if (!o)
		{
			if (len)
				*len = 0;
			return nullptr;
		}

		const char* s = luaV_tolstring(L, o, len);
		if (!s)
		{
			if (len)
				*len = 0;
			return nullptr;
		}

		return s;
	}

	const char* lua_tostring(lua_State* L, std::int32_t idx)
	{
		return lua_tolstring(L, idx, nullptr);
	}

	lua_Number lua_tonumber(lua_State* L, std::int32_t idx)
	{
		StkId o = index2adr(L, idx);
		if (!o || o->tt != LUA_TNUMBER)
			return 0.0f;
		return o->value.n;
	}

	void lua_sethook(lua_State* L, lua_Hook func, std::int32_t mask, std::int32_t count) // the interpreter loop DOES not respect this
	{
		if (!func || !mask)
		{
			L->hook = nullptr;
			L->hookmask = 0;
			L->hookcount = 0;
			L->basehookcount = 0;
		}
		else
		{
			L->hook = func;
			L->hookmask = mask;
			L->basehookcount = count;
			L->hookcount = count;
			L->hook_unk_64 = 1;
		}
	}

	std::string_view lua_GetLuaStateName(lua_State* L)
	{
		if (!L || !L->name)
			return {};

		TString* ts = reinterpret_cast<TString*>(L->name);
		return std::string_view(ts->data);
	}

	std::int32_t lua_type(lua_State* L, std::int32_t idx)
	{
		StkId o = index2adr(L, idx);
		if (!o)
			return LUA_TNONE;
		return o->tt;
	}

	void lua_pushvalue(lua_State* L, std::int32_t idx)
	{
		if (!lua_checkstack(L, 1))
			return;

		StkId o = index2adr(L, idx);
		if (!o)
		{
			L->top->tt = LUA_TNIL;
			L->top->value.p = nullptr;
		}
		else
		{
			L->top->value = o->value;
			L->top->tt = o->tt;
		}
		L->top++;
	}

	bool lua_isnil(lua_State* L, std::int32_t idx)
	{
		return lua_type(L, idx) == LUA_TNIL;
	}

	std::int32_t lua_setfenv(lua_State* L, std::int32_t idx)
	{
		StkId o = index2adr(L, idx);
		if (!o || L->top <= L->base)
			return 0;
		StkId tbl = L->top - 1;
		if (tbl->tt != LUA_TTABLE)
			return 0;

		std::int32_t res = luaK_setfenv(L, o, tbl); // kore's internal luaK_setfenv already pops the table off the stack

		if (res)
			L->top--;

		return res;
	}

	Table* lua_newtable(lua_State* L)
	{
		return lua_createtable(L, 0, 0);
	}

	void lua_remove(lua_State* L, std::int32_t idx)
	{
		StkId p = index2adr(L, idx);
		if (!p || p >= L->top)
			return;
		for (StkId q = p; q + 1 < L->top; ++q)
			*q = *(q + 1);
		--L->top;
	}

	void lua_xmove(lua_State* from, lua_State* to, std::int32_t n)
	{
		if (n <= 0)
			return;

		if (from->l_G != to->l_G)
			return;

		if (!lua_checkstack(to, n))
			return;

		StkId src = from->top - n;
		StkId dst = to->top;

		for (std::int32_t i = 0; i < n; ++i)
		{
			dst[i].value = src[i].value;
			dst[i].tt = src[i].tt;
		}

		from->top -= n;
		to->top += n;
	}

	void lua_pushnil(lua_State* L)
	{
		if (!lua_checkstack(L, 1))
			return;

		L->top->tt = LUA_TNIL;
		L->top->value.p = nullptr;
		L->top++;
	}

	std::int32_t lua_checkstack(lua_State* L, std::int32_t extra)
	{
		if (extra <= 0)
			return 1;

		if (L->top + extra <= L->stack_last)
			return 1;

		luaD_growstack(&L->ci_base, L, extra);

		return (L->top + extra <= L->stack_last) ? 1 : 0;
	}

	void lua_replace(lua_State* L, std::int32_t idx)
	{
		StkId o = index2adr(L, idx);
		if (!o || L->top <= L->base)
			return;

		*o = *(L->top - 1);
		L->top--;
	}

	bool lua_toboolean(lua_State* L, std::int32_t idx)
	{
		StkId o = index2adr(L, idx);
		if (!o)
			return false;

		if (o->tt == LUA_TNIL || o->tt == LUA_TNONE)
			return false;

		if (o->tt == LUA_TBOOLEAN)
			return o->value.b != 0;

		return true;
	}

	std::int32_t lua_upvalueindex(std::int32_t i)
	{
		return LUA_GLOBALSINDEX - i;
	}

	lua_State* lua_tothread(lua_State* L, std::int32_t idx)
	{
		StkId o = index2adr(L, idx);
		if (!o || o->tt != LUA_TTHREAD)
			return nullptr;

		return reinterpret_cast<lua_State*>(o->value.gc);
	}

	void lua_insert(lua_State* L, std::int32_t idx)
	{
		StkId p = index2adr(L, idx);
		if (!p || L->top <= L->base)
			return;

		if (p < L->base || p >= L->top)
			return;

		TValue topval = *(L->top - 1);

		for (StkId q = L->top - 1; q > p; --q)
			*q = *(q - 1);

		*p = topval;
	}

	void lua_getfield(lua_State* L, std::int32_t idx, const char* k)
	{
		StkId t = index2adr(L, idx);
		if (!t)
		{
			lua_pushnil(L);
			return;
		}

		const TValue table = *t;

		if (k)
			luaS_newlstr(L, k, std::strlen(k));
		else
			lua_pushnil(L);

		StkId key = L->top - 1;
		*key = luaV_gettable(L, table.value.p, table.tt, key->value, key->tt);
	}

	void lua_gettable(lua_State* L, std::uint32_t idx)
	{
		StkId t = index2adr(L, idx);
		if (!t || L->top <= L->base)
			return;

		const TValue table = *t;
		StkId key = L->top - 1;

		*key = luaV_gettable(L, table.value.p, table.tt, key->value, key->tt);
	}

	StkId lua_pushcclosure(lua_State* L, lua_CFunction fn, std::int32_t n)
	{
		return luaF_newCclosure(L, fn, n, 0);
	}

	StkId lua_pushcfunction(lua_State* L, lua_CFunction fn)
	{
		return lua_pushcclosure(L, fn, 0);
	}

	void lua_getglobal(lua_State* L, const char* k)
	{
		lua_getfield(L, LUA_GLOBALSINDEX, k);
	}

	void lua_setglobal(lua_State* L, const char* k)
	{
		lua_setfield(L, LUA_GLOBALSINDEX, k);
	}

	void lua_pushlstring(lua_State* L, const char* s, std::size_t len)
	{
		if (!lua_checkstack(L, 1))
			return;

		if (!s)
		{
			lua_pushnil(L);
			return;
		}

		luaS_newlstr(L, s, len);

	}

	void lua_pushstring(lua_State* L, const char* s)
	{
		lua_pushlstring(L, s, std::strlen(s));
	}

	std::int32_t lua_error(lua_State* L)
	{
		return luaD_throw(L, LUA_ERROR);
	}

	std::int32_t abs_index(lua_State* L, std::int32_t idx)
	{
		if (idx > 0 || idx <= Kore::LUA_REGISTRYINDEX)
			return idx;
		return static_cast<std::int32_t>(L->top - L->base) + idx + 1;
	}

	void lua_settable(lua_State* L, std::int32_t idx)
	{
		if (L->top - L->base < 2)
			return;

		StkId t = index2adr(L, idx);
		if (!t)
			return;

		StkId key = L->top - 2;
		StkId val = L->top - 1;

		luaV_settable(L, t, key, val);

		L->top -= 2;
	}

	void lua_setfield(lua_State* L, std::int32_t idx, const char* k)
	{
		if (L->top <= L->base)
			return;

		if (!lua_checkstack(L, 1))
			return;

		const std::int32_t tidx = abs_index(L, idx);

		if (k)
			luaS_newlstr(L, k, std::strlen(k));
		else
			lua_pushnil(L);

		StkId t = index2adr(L, tidx);
		if (!t)
		{
			L->top -= 1;
			return;
		}

		StkId val = L->top - 2;
		StkId key = L->top - 1;

		luaV_settable(L, t, key, val);

		L->top -= 2;
	}

	std::int32_t lua_yield(lua_State* L, std::int32_t nresults)
	{
		if (L->status == LUA_YIELD)
			return luaL_error(L, "attempt to yield a yielded coroutine");

		if (L->errorJmp && L->errorJmp->previous != nullptr)
			return luaL_error(L, "attempt to yield across metamethod/C-call boundary");

		if (L->l_G && L->l_G->mainthread == L)
			return luaL_error(L, "You cannot yield the main state");


		if (nresults < 0)
			nresults = 0;

		if (nresults > 0)
		{
			StkId src = L->top - nresults;
			StkId dst = L->base;
			for (std::int32_t i = 0; i < nresults; ++i)
				dst[i] = src[i];
		}


		L->top = L->base + nresults;
		L->status = LUA_YIELD;
		return LUA_YIELD;
	}

	void lua_pushnumber(lua_State* L, lua_Number n)
	{
		if (!lua_checkstack(L, 1))
			return;

		L->top->value.n = n;
		L->top->tt = LUA_TNUMBER;
		++L->top;
	}

	void lua_pushboolean(lua_State* L, std::int32_t b)
	{
		if (!lua_checkstack(L, 1))
			return;

		L->top->value.b = (b != 0);
		L->top->tt = LUA_TBOOLEAN;
		++L->top;
	}
}