#pragma once
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <TlHelp32.h>
#include <string_view>
#include <filesystem>
#include <vector>
#include <thread>

/*
* F3SE_Launcher.hpp contains....
* definitions of the functions in the main source file along with variables defining certain names and values for easy access
* (yeah i know we don't need functions defined here but i have OCD about header / source pair sometimes)
*/

constexpr std::string_view LauncherExe = "FableLauncher.exe";
constexpr std::string_view TargetExe = "Fable3.exe";
constexpr std::string_view DLLName = "F3SE.dll";
constexpr std::string_view SecuROMIPCEmulator = "F3SecuromPAless.exe";
constexpr std::uint32_t TimeoutMS = 120000;
constexpr std::uint32_t PollMS = 100;

std::filesystem::path GetExeDir();

bool NameEq(std::string_view a, std::string_view b);

std::uint32_t FindPid(std::string_view exe);

bool Inject(std::uint32_t pid, const std::filesystem::path& dll);

bool StartLauncher(const std::filesystem::path& root, const std::filesystem::path& launcher);

std::uint32_t LaunchAndInject();

int main();