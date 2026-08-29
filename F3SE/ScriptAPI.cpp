#include "ScriptAPI.hpp"
#include "Overlay.hpp"

#include <unordered_map>
#include <chrono>

/*
* ScriptAPI.cpp contains the primary logic for getting scripts to actually run
* Along with the custom CFunctions/CClosures we register to the environment
* and all of the sandboxing logic for threads, the main function, coroutines, etc
* 
* The logic for handling yields that we manage is also in here too
* 
* Along with the luaL_ref/luaL_unref RAII logic
* because we want our Lua threads to be popped off the main Lua state and put into the Lua registry while they're in use
* but we don't want a thread that has finished execution to stay stuck in the registry
* so our RAII should remove the ref to it in the registry when we're no longer using or needing the thread
* allowing the Lua garbage collector to destroy it
*/

namespace ScriptAPI
{
	static std::atomic<std::uint32_t> NextScriptId{ 1 };

	static std::string MakeUniqueScriptName()
	{
		std::uint32_t id = NextScriptId.fetch_add(1, std::memory_order_relaxed);
		char buf[32];
		std::snprintf(buf, sizeof(buf), "F3SE_script%u", id);
		return buf;
	}

	struct RegistryRef
	{
		Kore::lua_State* L = nullptr;
		int ref = Kore::LUA_REFNIL;

		RegistryRef() = default;
		RegistryRef(Kore::lua_State* l, int r) noexcept : L(l), ref(r) {}

		~RegistryRef() { reset(); }

		RegistryRef(const RegistryRef&) = delete;
		RegistryRef& operator=(const RegistryRef&) = delete;

		RegistryRef(RegistryRef&& o) noexcept : L(o.L), ref(o.ref) {
			o.L = nullptr;
			o.ref = Kore::LUA_REFNIL;
		}

		RegistryRef& operator=(RegistryRef&& o) noexcept {
			if (this != &o) {
				reset();
				L = o.L;
				ref = o.ref;
				o.L = nullptr;
				o.ref = Kore::LUA_REFNIL;
			}
			return *this;
		}

		void reset() noexcept {
			if (L && ref != Kore::LUA_REFNIL)
				Kore::luaL_unref(L, Kore::LUA_REGISTRYINDEX, ref);
			L = nullptr;
			ref = Kore::LUA_REFNIL;
		}

		/** Stop managing; caller owns the ref index now. */
		[[nodiscard]] int release() noexcept {
			const int r = ref;
			ref = Kore::LUA_REFNIL;
			L = nullptr;
			return r;
		}

		[[nodiscard]] bool valid() const noexcept {
			return ref != Kore::LUA_REFNIL;
		}
	};

	enum class YieldWakeType { None, Timer, Signal };

	struct YieldJob
	{
		RegistryRef anchor;
		Kore::lua_State* T = nullptr;
		YieldWakeType wake = YieldWakeType::None;
		std::string Signal;
		double resumeat = 0.0;
	};

	std::unordered_map<Kore::lua_State*, YieldJob> Yields;

	inline double GetTimeSeconds()
	{
		using clock = std::chrono::steady_clock;
		static const auto t0 = clock::now();
		return std::chrono::duration<double>(clock::now() - t0).count();
	}

	const char* LuaStatusString(const Kore::lua_State* L)
	{
		if (!L) return "invalid";
		if (L->status == Kore::LUA_YIELD) return "yield";
		if (L->status == Kore::LUA_OK) return "ok";
		if (L->status == Kore::LUA_ERROR) return "error";
		return "other";
	}

	// wait
	std::int32_t Wait(Kore::lua_State* L)
	{
		double sec = static_cast<double>(Kore::lua_tonumber(L, 1));

		const double now = GetTimeSeconds();

		auto& j = Yields[L];
		j.T = L;
		j.wake = YieldWakeType::Timer;
		j.resumeat = now + sec;
		j.Signal.clear();

		return Kore::lua_yield(L, 0);
	}

	// print
	std::int32_t Print(Kore::lua_State* L)
	{
		const std::int32_t n = lua_gettop(L);
		std::string line{};
		line.reserve(128);

		for (std::int32_t i = 1; i <= n; ++i)
		{
			if (i > 1)
				line.push_back('\t');

			const std::int32_t t = lua_type(L, i);
			switch (t)
			{
			case Kore::LUA_TNONE:
			{
				line += "none";
				break;
			}
			case Kore::LUA_TNIL:
			{
				line += "nil";
				break;
			}
			case Kore::LUA_TBOOLEAN:
			{
				line += Kore::lua_toboolean(L, i) ? "true" : "false";
				break;
			}
			case Kore::LUA_TNUMBER:
			{
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(Kore::lua_tonumber(L, i)));
				line += buf;
				break;
			}
			case Kore::LUA_TSTRING:
			{
				std::size_t len = 0;
				const char* str = Kore::lua_tolstring(L, i, &len);
				if (str)
					line.append(str, len);
				else
					line += "?";
				break;
			}
			case Kore::LUA_TUSERDATA:
			{
				line += "userdata";
				break;
			}
			case Kore::LUA_TLIGHTUSERDATA:
			{
				line += "light userdata";
				break;
			}
			case Kore::LUA_TTABLE:
			{
				line += "table";
				break;
			}
			case Kore::LUA_TTHREAD:
			{
				line += "thread";
				break;
			}
			case Kore::LUA_TLFUNCTION:
			{
				line += "lua function";
				break;
			}
			case Kore::LUA_TCFUNCTION:
			{
				line += "c function";
				break;
			}
			case Kore::LUA_TUI64:
			{
				line += "kore uint64";
				break;
			}
			case Kore::LUA_TSTRUCT:
			{
				line += "kore struct";
				break;
			}
			case Kore::LUA_TFUNCTION: // just incase?
			{
				line += "unknown function";
				break;
			}
			default: break;
			}
		}

		Overlay::AppendLog(line, Overlay::LogLine::Level::Info);

		return 0;
	}

	// coroutine.create
	std::int32_t SandboxedCoroutineCreate(Kore::lua_State* L)
	{
		if (Kore::lua_type(L, 1) != Kore::LUA_TLFUNCTION)
			return Kore::luaL_error(L, "Lua function expected");

		Kore::lua_pushvalue(L, Kore::LUA_GLOBALSINDEX);
		Kore::lua_setfenv(L, 1); // make sure the passed function respects the parent thread's _G / env

		Kore::lua_State* child = Kore::lua_newthread(L);

		Kore::lua_pushvalue(L, 1);
		Kore::lua_xmove(L, child, 1);

		static std::atomic<std::uint32_t> id{ 1 };
		char name[64];
		std::snprintf(name, sizeof(name), "F3SE_co%u", id.fetch_add(1));
		Kore::lua_SetLuaStateName(child, name);
		child->hookcount = 50000; // make sure the thread respects our limits

		Kore::lua_pushvalue(L, Kore::LUA_GLOBALSINDEX);
		Kore::lua_xmove(L, child, 1);
		Kore::lua_replace(child, Kore::LUA_GLOBALSINDEX); // make sure the new child thread respects the parent thread's _G / env

		return 1;
	}

	// coroutine.resume (for wrap)
	std::int32_t SandboxCoroutineWrapResume(Kore::lua_State* L)
	{
		Kore::lua_State* co = Kore::lua_tothread(L, Kore::lua_upvalueindex(1));
		if (!co)
			return Kore::luaL_error(L, "coroutine.wrap: no thread");

		const std::int32_t nargs = Kore::lua_gettop(L);

		if (nargs > 0)
			Kore::lua_xmove(L, co, nargs);

		const std::int32_t status = Kore::lua_resume(co, nargs);

		if (status != 0 && status != Kore::LUA_YIELD)
		{
			if (Kore::lua_gettop(co) >= 1)
				Kore::lua_xmove(co, L, 1);
			else
				Kore::lua_pushstring(L, "cannot resume dead coroutine");

			return Kore::lua_error(L);
		}


		const std::int32_t nres = Kore::lua_gettop(co);
		if (nres > 0)
			Kore::lua_xmove(co, L, nres);

		return nres;
	}

	// coroutine.wrap
	std::int32_t SandboxedCoroutineWrap(Kore::lua_State* L)
	{
		if (Kore::lua_type(L, 1) != Kore::LUA_TLFUNCTION)
			return Kore::luaL_error(L, "Lua function expected");

		SandboxedCoroutineCreate(L);

		if (Kore::lua_type(L, -1) != Kore::LUA_TTHREAD)
			return Kore::luaL_error(L, "coroutine.wrap: create failed");


		Kore::luaF_newCclosure(L, SandboxCoroutineWrapResume, 1, 1);

		return 1;
	}

	// F3SE.list_threads
	std::int32_t ListThreads(Kore::lua_State* L)
	{
		Kore::lua_State* MainState = L->l_G->main_L;

		Kore::lua_newtable(L);
		std::int32_t i = 0;
		for (Kore::lua_State* t = MainState; t; t = t->next_thread)
		{
			Kore::lua_newtable(L);

			std::string_view name = Kore::lua_GetLuaStateName(t);
			Kore::lua_pushstring(L, name.empty() ? "(unnamed)" : name.data());
			Kore::lua_setfield(L, -2, "name");

			Kore::lua_pushstring(L, LuaStatusString(t));
			Kore::lua_setfield(L, -2, "status");

			Kore::lua_pushboolean(L, name.starts_with("F3SE_"));
			Kore::lua_setfield(L, -2, "ours");

			++i;
			Kore::lua_pushnumber(L, static_cast<Kore::lua_Number>(i));
			Kore::lua_insert(L, -2);
			Kore::lua_settable(L, -3);
		}

		return 1;
	}

	// F3SE.force_complete_mistpeakdemondoor
	/*
	* Walks all Lua threads to find the primary thread for the Mistpeak Valley demon door
	* after that we walk the stack and ci to find the self table where we set self.ParentQuest.DemonDoorComplete = 1
	* then we call self:SetState("OUTRO") to force the real quest script into its outro sequence
	* (we wanna do it this way so that its actually opened properly and the quest thread doesn't leak you also get the end dialogue from the door)
	* 
	* I would've exposed a generalized function to manipulate quests this way but right now I deem that dangerous until I make up my mind about that....
	* but for now we expose a function that lets you force this door quest into its outro stage
	*/

	std::int32_t ForceCompleteMistpeakDemonDoor(Kore::lua_State* L)
	{
		Kore::lua_State* T = L->l_G->main_L;

		constexpr std::string_view wanted = "QD030_DemonDoor";

		for (std::int32_t seen = 0; T && seen < 4096; T = T->next_thread, ++seen)
		{
			if (Kore::lua_GetLuaStateName(T) != wanted)
				continue;

			if (T->status != Kore::LUA_YIELD && T->status != Kore::LUA_OK)
			{
				Kore::lua_pushstring(L, "bad status");
				return 1;
			}

			Kore::StkId self = nullptr;
			auto consider = [&](Kore::StkId o) {
				if (!o || o->tt != Kore::LUA_TTABLE || self)
					return;
				if (T->top >= T->stack_last)
					return;
				*T->top++ = *o;
				Kore::lua_getfield(T, -1, "SetState");
				Kore::lua_getfield(T, -2, "ParentQuest");
				const bool hit = !Kore::lua_isnil(T, -2) && !Kore::lua_isnil(T, -1);
				Kore::lua_pop(T, 3);
				if (hit)
					self = o;
				};

			if (T->ci && T->ci_base)
			{
				for (Kore::CallInfo* ci = T->ci; ci >= T->ci_base; --ci)
				{
					consider(ci->base);
					if (ci->base + 1 < T->top)
						consider(ci->base + 1);
				}
			}

			for (Kore::StkId o = T->base; !self && o < T->top; ++o)
				consider(o);

			if (!self)
			{
				Kore::lua_pushstring(L, "found thread, no self on stack");
				return 1;
			}

			const int save = Kore::lua_gettop(T);
			if (T->top >= T->stack_last)
			{
				Kore::lua_pushstring(L, "stack full");
				return 1;
			}

			*T->top++ = *self;

			Kore::lua_getfield(T, -1, "ParentQuest");
			if (!Kore::lua_isnil(T, -1))
			{
				Kore::lua_pushnumber(T, 1);
				Kore::lua_setfield(T, -2, "DemonDoorComplete");
			}
			Kore::lua_pop(T, 1);

			Kore::lua_getfield(T, -1, "SetState");
			if (Kore::lua_isnil(T, -1))
			{
				Kore::lua_settop(T, save);
				Kore::lua_pushstring(L, "no SetState");
				return 1;
			}

			Kore::lua_pushvalue(T, -2);
			Kore::lua_pushstring(T, "OUTRO");
			if (Kore::lua_pcall(T, 2, 0, 0) != 0)
			{
				Kore::lua_pop(T, 1);
				Kore::lua_settop(T, save);
				Kore::lua_pushstring(L, "SetState pcall failed");
				return 1;
			}

			Kore::lua_settop(T, save);
			Kore::lua_pushstring(L, "SetState(OUTRO) ok");
			return 1;
		}

		Kore::lua_pushstring(L, "core mistpeak demon door thread not available");

		return 1;
	}

	static bool YieldConditionMet(const YieldJob& job)
	{
		switch (job.wake)
		{
		case YieldWakeType::Timer:
			return GetTimeSeconds() >= job.resumeat;
		case YieldWakeType::Signal:
		{
			// todo
			return false;
		}
		case YieldWakeType::None:
		default:
			return false;
		}
	}

	void HandleYields()
	{
		if (Yields.empty())
			return;

		for (auto it = Yields.begin(); it != Yields.end();)
		{
			YieldJob& job = it->second;

			if (!job.anchor.valid())
			{
				++it;
				continue;
			}

			if (!YieldConditionMet(job))
			{
				++it;
				continue;
			}

			const std::int32_t status = Kore::lua_resume(job.T, 0);

			if (status == Kore::LUA_OK)
			{
				it = Yields.erase(it);
				continue;
			}

			if (status == Kore::LUA_YIELD)
			{
				++it;
				continue;
			}

			const char* err = Kore::lua_tostring(job.T, -1);
			Overlay::AppendLog(err, Overlay::LogLine::Level::Error);
            lua_pop(job.T, 1);
			//std::cout << "Runtime: " << (err ? err : "?") << std::endl;
			it = Yields.erase(it);
		}
	}

	static void CopyGlobal(Kore::lua_State* from, Kore::lua_State* to, const char* name)
	{
		Kore::lua_getglobal(from, name);

		if (Kore::lua_isnil(from, -1))
		{
			Kore::lua_pop(from, 1);
			return;
		}

		Kore::lua_xmove(from, to, 1);
		Kore::lua_setfield(to, -2, name);
	}

    static void InstallGlobals(Kore::lua_State* L, Kore::lua_State* T)
    {
        for (const auto& name : Overlay::kAllGlobals)
            CopyGlobal(L, T, name.data());
    }

	void InstallSandboxedCoroutineFuncs(Kore::lua_State* L, Kore::lua_State* T)
	{
		Kore::lua_newtable(T);

		constexpr std::string_view CorKeep[] = { // exclude "setname" so we don't accidentally provide a function allowing L->name to be changed
			"resume", "yield", "status", "running", "getname"
		};

		for (const auto name : CorKeep)
		{
			Kore::lua_getglobal(L, "coroutine");
			if (Kore::lua_type(L, -1) != Kore::LUA_TTABLE)
			{
				Kore::lua_pop(L, 1);
				continue;
			}

			Kore::lua_getfield(L, -1, name.data());
			Kore::lua_remove(L, -2);

			if (Kore::lua_isnil(L, -1))
			{
				Kore::lua_pop(L, 1);
				continue;
			}

			Kore::lua_xmove(L, T, 1);
			Kore::lua_setfield(T, -2, name.data());
		}

		Kore::lua_pushcfunction(T, SandboxedCoroutineCreate);
		Kore::lua_setfield(T, -2, "create");

		Kore::lua_pushcfunction(T, SandboxedCoroutineWrap);
		Kore::lua_setfield(T, -2, "wrap");

		Kore::lua_setglobal(T, "coroutine");
	}

	bool LooksLikeBytecode(std::string_view src) // make sure we're not trying to run bytecode (probably better ways to guard from this but it just werks)
	{
		if (src.empty())
			return false;

		if (static_cast<unsigned char>(src[0]) == 0x1B)
			return true;

		if (src.size() >= 4 && src[0] == '\x1b' && src[1] == 'L' && src[2] == 'u' && src[3] == 'a')
			return true;
		return false;
	}

	void ExecuteScript(Kore::lua_State* L, std::string_view Script)
	{
		Kore::lua_State* T = Kore::lua_newthread(L);
		if (!T)
			return;

		RegistryRef anchor{ L, Kore::luaL_ref(L, Kore::LUA_REGISTRYINDEX) };

		std::string ScriptName = MakeUniqueScriptName();

		Kore::lua_SetLuaStateName(T, ScriptName.c_str());

		T->hookcount = 50000; // make sure the thread respects our limits

		Kore::lua_newtable(T); // create our new _G / env table
		Kore::lua_pushvalue(T, -1);
		Kore::lua_setfield(T, -2, "_G");

		InstallGlobals(L, T); // copy over all of the allowed globals from the main Lua state into our _G table

        Kore::lua_replace(T, Kore::LUA_GLOBALSINDEX); // make sure our thread switches its environment to our table


        Kore::lua_pushcfunction(T, Wait);
        Kore::lua_setglobal(T, "wait");

		Kore::lua_pushcfunction(T, Print);
		Kore::lua_setglobal(T, "print");

		Kore::lua_newtable(T);

		Kore::lua_pushcfunction(T, ListThreads);
		Kore::lua_setfield(T, -2, "list_threads");

		Kore::lua_pushcfunction(T, ForceCompleteMistpeakDemonDoor);
		Kore::lua_setfield(T, -2, "force_complete_mistpeakdemondoor");

		Kore::lua_setglobal(T, "F3SE");

		InstallSandboxedCoroutineFuncs(L, T);

		if (LooksLikeBytecode(Script))
		{
			Overlay::AppendLog("rejected: bytecode is not allowed", Overlay::LogLine::Level::Error);
			return;
		}
		else
		{
			if (Kore::luaL_loadbuffer(T, Script.data(), Script.size(), ScriptName.c_str()) != 0)
			{
				const char* err = Kore::lua_tostring(T, -1);
				Overlay::AppendLog(err, Overlay::LogLine::Level::Error);
				lua_pop(T, 1);
				//std::cout << "Compile: " << (err ? err : "?") << std::endl;
				return;
			}
		}


		Kore::lua_pushvalue(T, Kore::LUA_GLOBALSINDEX);
		Kore::lua_setfenv(T, -2); // make sure the function respects our new _G / env table

		const std::int32_t status = Kore::lua_resume(T, 0);

		switch (status)
		{
		case Kore::LUA_OK:
			break;
		case Kore::LUA_YIELD:
		{
			auto it = Yields.find(T);
			if (it != Yields.end() && it->second.wake != YieldWakeType::None)
				it->second.anchor = std::move(anchor);
			break;
		}
		case Kore::LUA_ERROR:
		default:
		{
			const char* err = Kore::lua_tostring(T, -1);
			Overlay::AppendLog(err, Overlay::LogLine::Level::Error);
            lua_pop(T, 1);
			break;
		}
		}
	}
}