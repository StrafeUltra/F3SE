#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <queue>
#include <mutex>
#include <string_view>
#include <filesystem>
#include <fstream>

#include "F3SE.hpp"
#include "Kore.hpp"
#include "Overlay.hpp"
#include "ScriptAPI.hpp"


/*
* F3SE.cpp contains the main initialization logic and things such as...
* Logic for getting the game's primary game Lua State (not the GUI related one)
* 
* A hook on the LF tick branch in the main game thread for proper script scheduling (HF tick branch might be fine but Kore seems to be called from LF)
* There are some weird race conditions (lua related) running outside of the main game's thread... and creating a new Lua State with all of the goodies is not simple
* Also because of that game thread having the main havok allocator in its TEB->TLS
* (i managed to get my own thread to have its own havok allocator previously and it worked but more weird (havok related) race conditions)
* 
* 
* A hook at the beginning of the main Lua VM interpreter loop in luaV_execute with the goal of bringing back the lua_sethook maskcount logic
* We identify that its one of our lua threads with L->name and then thankfully they have leftover members in the Lua State structure so we use L->hookcount
* to moitor how many instructions the lua thread has executed so far and if it gets past our limit we stop it
* (this hook is to mainly guard against various types of infinite loop stalls in scripts)
* 
* The Autorun logic is also here
* 
* We also increase the LF tick rate from 15 Hz up to 30 Hz, and we also increase the HF tick rate from 30 hz to 60 hz
* So the sim tick system retains the 2:1 ratio but we've doubled it and this provides some noticable improvements to get rid of some of the latency
* Check Overlay.cpp on how we fix FPS with the game's VSync enabled
*/


namespace F3SE
{
	constexpr std::uintptr_t FableAppRVA = 0x19BD9DC;
	constexpr std::uintptr_t LFHookRVA = 0x279B71; // 5
	constexpr std::uintptr_t VMHookRVA = 0x1E9AF6; // 7
	constexpr std::uintptr_t LFTickRateRVA = 0x1886BF8;
	constexpr std::uintptr_t HFTickRateRVA = 0x1886C00;
	constexpr std::uintptr_t SubstepsPatchRVA = 0x138ADC5;

	std::uintptr_t ModuleBase;
	std::uintptr_t LFHookVA;
	std::uintptr_t LFHookJMPBack;

	std::uintptr_t VMHookVA;
	std::uintptr_t VMHookJMPBack;

	std::uintptr_t LFTickRateVA;
	std::uintptr_t HFTickRateVA;
	std::uintptr_t SubstepsPatchVA;

	std::mutex ScriptMutex;
	std::queue<std::string> ScriptQueue;

	[[nodiscard]] static Kore::lua_State* GetLuaState() noexcept
	{
		const std::uintptr_t FableApp = *reinterpret_cast<std::uintptr_t*>(ModuleBase + FableAppRVA);
		if (!FableApp)
			return nullptr;

		return *reinterpret_cast<Kore::lua_State**>(FableApp + 0xBC);
	}

	extern "C" void LFTickHookDrain()
	{
		static Kore::lua_State* L = nullptr;
		static bool AreFuncsAvail = false;

		if (!L)
		{
			L = GetLuaState();
			return;
		}

		if (!AreFuncsAvail)
		{
			Kore::lua_getglobal(L, "IsLevelLoaded"); // hack to make sure the later globals are available before we try running anything todo something better than this
			AreFuncsAvail = !Kore::lua_isnil(L, -1);
			Kore::lua_pop(L, 1);
			return;
		}

		ScriptAPI::HandleYields();

		std::string CurrentScript;
		{
			std::lock_guard<std::mutex> lock(ScriptMutex);
			if (ScriptQueue.empty())
				return;

			CurrentScript = std::move(ScriptQueue.front());
			ScriptQueue.pop();
		}

		if (!CurrentScript.empty())
			ScriptAPI::ExecuteScript(L, CurrentScript);
	}

	static __declspec(naked) void LFTickHook()
	{
		__asm
		{
			lea edx, [esp + 0x64]

			pushad
			pushfd

			call LFTickHookDrain

			popfd
			popad

			push edx

			jmp dword ptr [LFHookJMPBack]
		}
	}

	extern "C" void VMHookDrain(Kore::lua_State* L)
	{
		if (!L)
			return;

		std::string_view name = Kore::lua_GetLuaStateName(L);

		if (!name.starts_with("F3SE_"))
			return;

		if (L->hookcount == 0)
			Kore::luaL_error(L, "script timeout: instruction limit exceeded (nested)");

		if (--L->hookcount != 0)
			return;


		Kore::luaL_error(L, "script timeout: instruction limit exceeded");
	}

	static __declspec(naked) void VMHook()
	{
		__asm
		{
			pushad
			pushfd

			push edi
			call VMHookDrain
			add esp, 0x4

			popfd
			popad

			add ecx, 0x4
			mov [esp+0x10], ecx

			jmp dword ptr [VMHookJMPBack]
		}
	}

	void QueueScript(const std::string& Script)
	{
		if (Script.empty()) return;
			std::lock_guard lock(ScriptMutex);
			ScriptQueue.push(Script);
	}

	std::filesystem::path GetGameDirectory()
	{
		char buf[MAX_PATH]{};
		const unsigned long n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return {};
		return std::filesystem::path(buf).parent_path();
	}

	bool ReadWholeFile(const std::filesystem::path& path, std::string& out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;

		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

		return !in.bad();
	}

	void QueueAutorunScripts()
	{
		const std::filesystem::path dir = GetGameDirectory() / "F3SE";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		std::vector<std::filesystem::path> files;
		for (const auto& ent : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec) break;
			if (!ent.is_regular_file(ec)) continue;
			if (ent.path().extension() != ".lua") continue;
			files.push_back(ent.path());
		}

		std::sort(files.begin(), files.end());

		for (const auto& path : files)
		{
			std::string Script;
			if (!ReadWholeFile(path, Script)) continue;

			QueueScript(Script);
		}
	}

	static void Initialize()
	{
		DWORD OP{};

		ModuleBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
		LFHookVA = ModuleBase + LFHookRVA;
		LFHookJMPBack = LFHookVA + 0x5;

		VMHookVA = ModuleBase + VMHookRVA;
		VMHookJMPBack = VMHookVA + 0x7;

		LFTickRateVA = ModuleBase + LFTickRateRVA;
		HFTickRateVA = ModuleBase + HFTickRateRVA;

		SubstepsPatchVA = ModuleBase + SubstepsPatchRVA;

		Kore::InitializeFunctions(ModuleBase);

		/*
		* 30 LF and 60 HF seem to be the most reasonable....
		* we wanna maintain the 2:1 ratio with them
		*/

		*reinterpret_cast<double*>(LFTickRateVA) = 30.0; // originally 15 Hz
		*reinterpret_cast<double*>(HFTickRateVA) = 60.0; // originally 30 Hz

		/*
		* Just below we fix the major consequence of setting LF higher
		* We enforce that only 1 substep needs to occur in hkpCharacterProxy::integrate so that the engine doesn't get annoying and stick to stairs/ledges
		*/

		::VirtualProtect(reinterpret_cast<void*>(SubstepsPatchVA), 6, PAGE_EXECUTE_READWRITE, &OP);
		*reinterpret_cast<std::uint8_t*>(SubstepsPatchVA) = 0x83;
		*reinterpret_cast<std::uint8_t*>(SubstepsPatchVA + 0x1) = 0xF9;
		*reinterpret_cast<std::uint8_t*>(SubstepsPatchVA + 0x2) = 0x1;
		*reinterpret_cast<std::uint8_t*>(SubstepsPatchVA + 0x3) = 0x90;
		*reinterpret_cast<std::uint8_t*>(SubstepsPatchVA + 0x4) = 0x90;
		*reinterpret_cast<std::uint8_t*>(SubstepsPatchVA + 0x5) = 0x90;
		::VirtualProtect(reinterpret_cast<void*>(SubstepsPatchVA), 6, OP, &OP);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(SubstepsPatchVA), 6);


		::VirtualProtect(reinterpret_cast<void*>(LFHookVA), 5, PAGE_EXECUTE_READWRITE, &OP);
		*reinterpret_cast<std::uint8_t*>(LFHookVA) = 0xE9;
		*reinterpret_cast<std::int32_t*>(LFHookVA + 0x1) = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&LFTickHook) - LFHookVA - 5);
		::VirtualProtect(reinterpret_cast<void*>(LFHookVA), 5, OP, &OP);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(LFHookVA), 5);

		::VirtualProtect(reinterpret_cast<void*>(VMHookVA), 7, PAGE_EXECUTE_READWRITE, &OP);
		*reinterpret_cast<std::uint8_t*>(VMHookVA) = 0xE9;
		*reinterpret_cast<std::int32_t*>(VMHookVA + 0x1) = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&VMHook) - VMHookVA - 5);
		*reinterpret_cast<std::uint8_t*>(VMHookVA + 0x5) = 0x90;
		*reinterpret_cast<std::uint8_t*>(VMHookVA + 0x6) = 0x90;
		::VirtualProtect(reinterpret_cast<void*>(VMHookVA), 7, OP, &OP);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(VMHookVA), 7);

		Overlay::InstallD3DHook([](std::string_view src)
			{
				QueueScript(std::string(src));
			}
		);

		QueueAutorunScripts();
	}
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, [[maybe_unused]] LPVOID lpReserved)
{
	if (ulReason == DLL_PROCESS_ATTACH)
	{
		::DisableThreadLibraryCalls(hModule);
		std::thread{ F3SE::Initialize }.detach();
	}

	return TRUE;
}