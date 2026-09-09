#include "F3SE_Launcher.hpp"

/*
* F3SE_Launcher.cpp contains the main logic for launching the game and then injecting our F3SE DLL into the correct child process
* 
* Because of SecuROM PA 8 being included with the game its not as simple as just launching Fable3.exe
* so we have to use the correct launch path to keep the shitty DRM happy
* 
* The launch chain is basically F3SE_Launcher.exe -> FableLauncher.exe -> F3Secu.exe -> Fable3.exe
* After we launch FableLauncher.exe we wait until the main Fable3.exe child process is available
* at which point we inject our DLL into it using standard injection
* 
* Now checks to see if my SecuROM PA emulator is present during launch if so we launch that instead of the usual chain
*/

std::filesystem::path GetExeDir()
{
	char buf[MAX_PATH]{};
	const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return {};
	return std::filesystem::path(buf).parent_path();
}

bool NameEq(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;

	for (size_t i = 0; i < a.size(); ++i)
	{
		const auto ca = static_cast<unsigned char>(a[i]);
		const auto cb = static_cast<unsigned char>(b[i]);
		if (std::tolower(ca) != std::tolower(cb))
			return false;
	}
	return true;
}

std::uint32_t FindPid(std::string_view exe)
{
	HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE)
		return {};

	PROCESSENTRY32 ProcEnt{};
	ProcEnt.dwSize = sizeof(PROCESSENTRY32);

	std::uint32_t pid{};

	if (::Process32First(hSnap, &ProcEnt))
	{
		do
		{
			if (NameEq(ProcEnt.szExeFile, exe))
			{
				pid = ProcEnt.th32ProcessID;
				break;
			}
		} while (::Process32Next(hSnap, &ProcEnt));
	}

	::CloseHandle(hSnap);
	return pid;
}

bool Inject(std::uint32_t pid, const std::filesystem::path& dll)
{
	const std::string path = dll.string();

	HANDLE hProc = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);

	if (!hProc)
		return false;

	const size_t bytes = path.size() + 1;

	void* pathstr = ::VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!pathstr)
	{
		::CloseHandle(hProc);
		return false;
	}

	bool ok = false;

	if (::WriteProcessMemory(hProc, pathstr, path.c_str(), bytes, nullptr))
	{
		auto* load = reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
		if (load)
		{
			HANDLE th = ::CreateRemoteThread(hProc, nullptr, 0, load, pathstr, 0, nullptr);
			if (th)
			{
				::WaitForSingleObject(th, 15000);
				DWORD code = 0;
				::GetExitCodeThread(th, &code);
				::CloseHandle(th);
				ok = (code != 0);
			}
		}
	}

	::VirtualFreeEx(hProc, pathstr, 0, MEM_RELEASE);
	::CloseHandle(hProc);

	return ok;
}

bool StartLauncher(const std::filesystem::path& root, const std::filesystem::path& launcher)
{
	STARTUPINFOA StartInfo{};
	StartInfo.cb = sizeof(STARTUPINFOA);
	PROCESS_INFORMATION ProcInfo{};

	const std::string path = launcher.string();
	const std::string cwd = root.string();


	const BOOL ok = ::CreateProcessA(path.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &StartInfo, &ProcInfo);

	if (!ok)
		return false;

	::CloseHandle(ProcInfo.hThread);
	::CloseHandle(ProcInfo.hProcess);

	return true;
}

std::uint32_t LaunchAndInject()
{
	const std::filesystem::path root = GetExeDir();
	if (root.empty())
		return {};

	const std::filesystem::path launcher = root / LauncherExe;
	const std::filesystem::path dll = root / DLLName;
	const std::filesystem::path spaemu = root / SecuROMIPCEmulator;

	if (!std::filesystem::is_regular_file(dll))
		return {};

	if (auto existing = FindPid(TargetExe))
	{
		if (Inject(existing, dll))
			return existing;
		return {};
	}


	if (!std::filesystem::is_regular_file(spaemu))
	{
		if (!StartLauncher(root, launcher))
			return {};
	}
	else
	{
		if (!StartLauncher(root, spaemu))
			return {};
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMS);

	while (std::chrono::steady_clock::now() < deadline)
	{
		if (auto pid = FindPid(TargetExe))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			if (Inject(pid, dll))
				return pid;
			return {};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(PollMS));
	}

	return {};
}

BOOL WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR cmd, int argc)
{
	const auto pid = LaunchAndInject();

	if (!pid)
		return 1;

	return 0;
}