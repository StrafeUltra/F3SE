#define WIN32_LEAN_AND_MEAN

#include "Overlay.hpp"

#include <Windows.h>
#include <d3d9.h>
#include <mutex>
#include <vector>
#include <charconv>

#include "MinHook/include/MinHook.h"

#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_dx9.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/TextEditor.h"

#pragma comment(lib, "d3d9.lib")

/*
* Overlay.cpp contains the primary logic for the ImGui executor GUI we render along with D3D9 hooks for it
* 
* We also do something important we fix Fable 3's broken VSync implementation now it respects your monitor's refresh rate when on
* Check the "FixPresentParams" function on how that's done (called in CreateDevice and Reset hooks)
*/

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay
{
	constexpr std::uintptr_t InputPollRVA = 0x148EFB0;

	using EndSceneFunc = HRESULT(__stdcall*)(IDirect3DDevice9*);
	EndSceneFunc OrigEndScene = nullptr;

	using ResetFunc = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
	ResetFunc OrigReset = nullptr;

	using CreateDeviceFunc = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
	CreateDeviceFunc OrigCreateDevice = nullptr;

	bool Installed = false;
	bool ImGuiReady = false;
	bool ExecutorOpen = false;
	bool WantShutdown = false;
	bool InputPollPatched = false;
	bool OutputScrollToBottom = false;

	static TextEditor LuaEditor;
	static bool LuaEditorSetup = false;

	ScriptSubmitFn ScriptSubmit;

	std::mutex LogMutex;
	std::vector<LogLine> LogLines;
	constexpr size_t MaxLogLines = 500;

	char ScriptBuf[64 * 1024] = {};

	struct CustomAPIInfo
	{
		std::string_view name;
		std::string_view desc;
	};

	static constexpr CustomAPIInfo F3SEEditorClosures[] = {
		{ "wait", "wait(seconds) - yield until timer" },
		{ "print", "print(...) - print to the executor output panel" },
		{ "coroutine", "coroutine - F3SE sandboxed version of the coroutine library" },
		{ "F3SE", "experimental F3SE functions" }
	};

	HWND hWnd = nullptr;
	WNDPROC PrevWndProc = nullptr;

	static LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (ExecutorOpen)
			ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);

		if (ExecutorOpen)
		{
			switch (msg)
			{
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_CHAR:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MOUSEWHEEL:
			case WM_INPUT:
				return 0;
			default:
				break;
			}
		}


		if (PrevWndProc)
			return ::CallWindowProcA(PrevWndProc, hwnd, msg, wp, lp);
		return ::DefWindowProcA(hwnd, msg, wp, lp);
	}

	static void EnsureWndProc(HWND hwnd)
	{
		if (!hwnd || hWnd == hwnd)
			return;
		hWnd = hwnd;
		PrevWndProc = reinterpret_cast<WNDPROC>(::SetWindowLongA(hwnd, GWL_WNDPROC, reinterpret_cast<LONG>(WndProcHook)));
	}

	static bool ResolveD3D9Funcs(void** ESFn, void** RFn, void** CDFn)
	{
		WNDCLASSA wc{};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = ::GetModuleHandleA(nullptr);
		wc.lpszClassName = "F3SEDummy";
		if (!::RegisterClassA(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return false;

		HWND hwnd = ::CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
		if (!hwnd)
			return false;

		IDirect3D9* d3d = ::Direct3DCreate9(D3D_SDK_VERSION);
		if (!d3d)
		{
			::DestroyWindow(hwnd);
			return false;
		}

		D3DPRESENT_PARAMETERS pp{};
		pp.Windowed = TRUE;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.hDeviceWindow = hwnd;
		pp.BackBufferFormat = D3DFMT_UNKNOWN;

		IDirect3DDevice9* dev = nullptr;
		HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);

		if (FAILED(hr) || !dev)
		{
			d3d->Release();
			::DestroyWindow(hwnd);
			return false;
		}

		void** Devicevftable = *reinterpret_cast<void***>(dev);
		*ESFn = Devicevftable[42];
		*RFn = Devicevftable[16];

		void** D3Dvftable = *reinterpret_cast<void***>(d3d);
		*CDFn = D3Dvftable[16];

		dev->Release();
		d3d->Release();
		::DestroyWindow(hwnd);
		return true;
	}

	static TextEditor::LanguageDefinition KoreF3Lua()
	{
		TextEditor::LanguageDefinition lang;

		lang.mName = "Lua";

		static constexpr std::string_view keywords[] = {
			"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return",
			"then", "true", "until", "while"
		};

		for (const auto& k : keywords)
			lang.mKeywords.insert(std::string(k));

		for (const std::string_view name : kAllGlobals)
		{
			if (name.empty())
				continue;

			if (lang.mKeywords.count(std::string(name)))
				continue;

			TextEditor::Identifier id;
			id.mDeclaration = "Built-in global";
			lang.mIdentifiers.insert(std::make_pair(std::string(name), id));
		}

		for (const CustomAPIInfo& api : F3SEEditorClosures)
		{
			TextEditor::Identifier id;
			id.mDeclaration = std::string(api.desc);
			lang.mIdentifiers.insert(std::make_pair(std::string(api.name), id));
		}

		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", TextEditor::PaletteIndex::String));
		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("\\\'[^\\\']*\\\'", TextEditor::PaletteIndex::String));
		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", TextEditor::PaletteIndex::Number));
		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", TextEditor::PaletteIndex::Number));
		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", TextEditor::PaletteIndex::Number));
		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*", TextEditor::PaletteIndex::Identifier));
		lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>("[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", TextEditor::PaletteIndex::Punctuation));

		lang.mSingleLineComment = "--";
		lang.mCommentStart = "--[[";
		lang.mCommentEnd = "]]";

		lang.mCaseSensitive = true;
		lang.mAutoIndentation = true;
		lang.mPreprocChar = 0;
		lang.mTokenize = nullptr;

		return lang;
	}


	static constexpr ImVec4 kBg = ImVec4(0.06f, 0.07f, 0.09f, 0.88f);
	static constexpr ImVec4 kBgDeep = ImVec4(0.04f, 0.05f, 0.07f, 0.92f);
	static constexpr ImVec4 kPanel = ImVec4(0.09f, 0.10f, 0.13f, 0.90f);
	static constexpr ImVec4 kGutter = ImVec4(0.11f, 0.12f, 0.16f, 0.90f);
	static constexpr ImVec4 kAccent = ImVec4(0.40f, 0.68f, 0.90f, 1.00f);
	static constexpr ImVec4 kAccentHi = ImVec4(0.52f, 0.78f, 0.98f, 1.00f);
	static constexpr ImVec4 kText = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);

	static ImU32 C(int r, int g, int b, int a = 255)
	{
		return IM_COL32(r, g, b, a);
	}

	static TextEditor::Palette MakeKoreLuaPalette()
	{
		auto pal = TextEditor::GetDarkPalette();


		pal[(int)TextEditor::PaletteIndex::Background] = C(22, 24, 32, 230);
		pal[(int)TextEditor::PaletteIndex::Default] = C(228, 230, 236);
		pal[(int)TextEditor::PaletteIndex::Identifier] = C(228, 230, 236);

		pal[(int)TextEditor::PaletteIndex::Keyword] = C(120, 176, 236);
		pal[(int)TextEditor::PaletteIndex::KnownIdentifier] = C(110, 204, 196);
		pal[(int)TextEditor::PaletteIndex::Number] = C(186, 214, 170);
		pal[(int)TextEditor::PaletteIndex::String] = C(224, 168, 140);
		pal[(int)TextEditor::PaletteIndex::CharLiteral] = C(224, 168, 140);
		pal[(int)TextEditor::PaletteIndex::Comment] = C(118, 138, 152);
		pal[(int)TextEditor::PaletteIndex::MultiLineComment] = C(118, 138, 152);
		pal[(int)TextEditor::PaletteIndex::Preprocessor] = C(168, 168, 184);
		pal[(int)TextEditor::PaletteIndex::PreprocIdentifier] = C(168, 168, 184);
		pal[(int)TextEditor::PaletteIndex::Punctuation] = C(156, 164, 176);


		pal[(int)TextEditor::PaletteIndex::LineNumber] = C(148, 156, 172);
		pal[(int)TextEditor::PaletteIndex::CurrentLineFill] = C(40, 48, 64, 140);
		pal[(int)TextEditor::PaletteIndex::CurrentLineFillInactive] = C(32, 36, 48, 80);
		pal[(int)TextEditor::PaletteIndex::CurrentLineEdge] = C(80, 130, 180, 180);
		pal[(int)TextEditor::PaletteIndex::Selection] = C(64, 112, 176, 120);
		pal[(int)TextEditor::PaletteIndex::Cursor] = C(220, 232, 255);
		pal[(int)TextEditor::PaletteIndex::ErrorMarker] = C(200, 72, 72, 150);

		return pal;
	}

	static void ApplyExecutorTheme()
	{
		ImGuiStyle& s = ImGui::GetStyle();
		s.WindowRounding = 10.f;
		s.ChildRounding = 8.f;
		s.FrameRounding = 6.f;
		s.PopupRounding = 8.f;
		s.ScrollbarRounding = 8.f;
		s.GrabRounding = 6.f;
		s.TabRounding = 6.f;
		s.WindowPadding = ImVec2(14, 14);
		s.FramePadding = ImVec2(10, 6);
		s.ItemSpacing = ImVec2(10, 8);
		s.ScrollbarSize = 12.f;
		s.WindowBorderSize = 1.f;
		s.ChildBorderSize = 1.f;
		s.FrameBorderSize = 0.f;
		s.Alpha = 1.f;

		ImVec4* c = s.Colors;
		c[ImGuiCol_Text] = kText;
		c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.55f, 0.60f, 1.f);
		c[ImGuiCol_WindowBg] = kBg;
		c[ImGuiCol_ChildBg] = kPanel;
		c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.08f, 0.11f, 0.94f);
		c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.10f);
		c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TitleBg] = kBgDeep;
		c[ImGuiCol_TitleBgActive] = kBgDeep;
		c[ImGuiCol_TitleBgCollapsed] = kBgDeep;
		c[ImGuiCol_MenuBarBg] = kBgDeep;
		c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.15f, 0.19f, 0.85f);
		c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.26f, 0.95f);
		c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.24f, 0.32f, 1.f);
		c[ImGuiCol_Button] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.80f);
		c[ImGuiCol_ButtonHovered] = kAccentHi;
		c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.44f, 0.64f, 1.f);
		c[ImGuiCol_Header] = ImVec4(0.22f, 0.32f, 0.46f, 0.50f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.46f, 0.66f, 0.65f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.34f, 0.52f, 0.74f, 0.80f);
		c[ImGuiCol_Tab] = ImVec4(0.12f, 0.14f, 0.18f, 0.70f);
		c[ImGuiCol_TabHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.50f);
		c[ImGuiCol_TabActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.38f);
		c[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.11f, 0.14f, 0.60f);
		c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.16f, 0.20f, 0.28f, 0.70f);
		c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.20f);
		c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.16f);
		c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.26f);
		c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.36f);
		c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
		c[ImGuiCol_CheckMark] = kAccentHi;
		c[ImGuiCol_SliderGrab] = kAccent;
		c[ImGuiCol_SliderGrabActive] = kAccentHi;
		c[ImGuiCol_ResizeGrip] = ImVec4(1, 1, 1, 0.12f);
		c[ImGuiCol_ResizeGripHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.50f);
		c[ImGuiCol_ResizeGripActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.70f);
		c[ImGuiCol_PlotLines] = kAccent;
		c[ImGuiCol_PlotHistogram] = kAccent;
		c[ImGuiCol_NavHighlight] = kAccent;
	}

	static void InitLuaEditor()
	{
		if (LuaEditorSetup)
			return;
		LuaEditorSetup = true;

		LuaEditor.SetLanguageDefinition(KoreF3Lua());
		LuaEditor.SetShowWhitespaces(false);
		LuaEditor.SetTabSize(4);
		LuaEditor.SetImGuiChildIgnored(false);

		LuaEditor.SetPalette(MakeKoreLuaPalette());
	}

	static void InitImGui(IDirect3DDevice9* device)
	{
		if (ImGuiReady)
			return;

		D3DDEVICE_CREATION_PARAMETERS creationparams{};
		if (SUCCEEDED(device->GetCreationParameters(&creationparams)))
			EnsureWndProc(creationparams.hFocusWindow);
		if (!hWnd)
			hWnd = ::FindWindowA("Fable 3", nullptr);
		if (!hWnd)
			return;

		EnsureWndProc(hWnd);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.FontGlobalScale = 1.25f;
		ImGui::GetStyle().ScaleAllSizes(1.25f);
		ApplyExecutorTheme();
		ImGui_ImplWin32_Init(hWnd);
		ImGui_ImplDX9_Init(device);
		ImGuiReady = true;
	}

	static void DrawExecutor()
	{
		InitLuaEditor();

		ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.96f);

		if (!ImGui::Begin("F3SE Script Executor", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("Script"))
			{
				if (ImGui::MenuItem("Run"))
					ScriptSubmit(LuaEditor.GetText());

				if (ImGui::MenuItem("Clear output"))
					LogLines.clear();

				ImGui::EndMenu();
			}

			ImGui::EndMenuBar();
		}

		if (ImGui::Button("Run"))
		{
			LuaEditor.SetErrorMarkers({});
			std::string src = LuaEditor.GetText();
			ScriptSubmit(src);
		}

		ImGui::SameLine();
		if (ImGui::Button("Clear script"))
		{
			LuaEditor.SetErrorMarkers({});
			LuaEditor.SetText("");
		}
		ImGui::SameLine();
		ImGui::TextDisabled("F8 hide . input blocked while open");

		ImGui::Separator();

		ImGui::BeginChild("##edtior_host", ImVec2(0, -120), true);
		LuaEditor.Render("LuaEditor", ImGui::GetContentRegionAvail());
		ImGui::EndChild();

		ImGui::BeginChild("Output", ImVec2(0, 100.0f), true);
		ImGui::Separator();
		{
			std::lock_guard lock(LogMutex);
			for (const auto& line : LogLines)
			{
				ImVec4 c = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
				if (line.level == LogLine::Error) c = ImVec4(1.00f, 0.4f, 0.4f, 1.00f);
				if (line.level == LogLine::Warning) c = ImVec4(1.00f, 0.8f, 0.3f, 1.00f);
				ImGui::PushStyleColor(ImGuiCol_Text, c);
				ImGui::TextWrapped("%s", line.text.c_str());
				ImGui::PopStyleColor();
			}
		}
		if (OutputScrollToBottom)
		{
			ImGui::SetScrollHereY(1.00f);
			OutputScrollToBottom = false;
		}
		ImGui::EndChild();

		ImGui::End();
	}

	static void UpdateCursorForUI()
	{
		if (ExecutorOpen)
		{
			ImGui::GetIO().MouseDrawCursor = true;
			::ClipCursor(nullptr);
			::SetCursor(::LoadCursorA(nullptr, IDC_ARROW));
			while (::ShowCursor(TRUE) < 0) {}
		}
		else
		{
			ImGui::GetIO().MouseDrawCursor = false;
		}
	}

	/*
	* SetInputPollBlocked is an ugly hack function to make sure no inputs affect the game while you're using our executor
	* gui... it was very annoying opening game menus and moving around while trying to interact with the executor gui
	*/

	static void SetInputPollBlocked(bool block)
	{
		static const std::uintptr_t InputPollVA = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr)) + InputPollRVA;

		DWORD OP{};

		if (!::VirtualProtect(reinterpret_cast<void*>(InputPollVA), 1, PAGE_EXECUTE_READWRITE, &OP))
			return;

		if (block && !InputPollPatched)
		{
			*reinterpret_cast<std::uint8_t*>(InputPollVA) = 0xC3;
			InputPollPatched = true;
		}
		else if (!block && InputPollPatched)
		{
			*reinterpret_cast<std::uint8_t*>(InputPollVA) = 0x55;
			InputPollPatched = false;
		}

		::VirtualProtect(reinterpret_cast<void*>(InputPollVA), 1, OP, &OP);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(InputPollVA), 1);
	}

	static void OnHotkeys()
	{

		static bool WasDown = false;
		const bool Down = (::GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		if (Down && !WasDown)
		{
			ExecutorOpen = !ExecutorOpen;
			SetInputPollBlocked(ExecutorOpen);
		}
		WasDown = Down;
	}

	void FixPresentParams(D3DPRESENT_PARAMETERS* pp)
	{
		if (!pp)
			return;

		pp->FullScreen_RefreshRateInHz = 0;

		const UINT iv = pp->PresentationInterval;
		if (iv == D3DPRESENT_INTERVAL_TWO || iv == D3DPRESENT_INTERVAL_THREE || iv == D3DPRESENT_INTERVAL_FOUR) // yeah i know they only use two but just incase...
			pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;

		// vsync off means its immediate so we leave it alone
	}

	static HRESULT __stdcall HookEndScene(IDirect3DDevice9* device)
	{
		if (device && !WantShutdown)
		{
			if (!ImGuiReady)
				InitImGui(device);

			if (ImGuiReady)
			{
				ImGui_ImplDX9_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();

				OnHotkeys();
				UpdateCursorForUI();

				if (ExecutorOpen)
				{
					ImGui::GetIO().WantCaptureKeyboard = true;
					ImGui::GetIO().WantCaptureMouse = true;
					DrawExecutor();
				}

				ImGui::EndFrame();
				ImGui::Render();
				ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			}
		}

		return OrigEndScene(device);
	}

	static HRESULT __stdcall HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
	{
		if (ImGuiReady)
			ImGui_ImplDX9_InvalidateDeviceObjects();

		FixPresentParams(pp);

		const HRESULT hr = OrigReset(device, pp);

		if (SUCCEEDED(hr) && ImGuiReady)
			ImGui_ImplDX9_CreateDeviceObjects();

		return hr;
	}

	static HRESULT __stdcall HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
	{
		FixPresentParams(pp);

		return OrigCreateDevice(self, adapter, type, hwnd, flags, pp, out);
	}

	bool InstallD3DHook(ScriptSubmitFn OnSubmit)
	{
		if (Installed)
			return true;

		ScriptSubmit = std::move(OnSubmit);

		void* EndScenePtr = nullptr;
		void* ResetPtr = nullptr;
		void* CreateDevicePtr = nullptr;

		if (!ResolveD3D9Funcs(&EndScenePtr, &ResetPtr, &CreateDevicePtr))
			return false;

		if (MH_Initialize() != MH_OK)
			return false;

		if (MH_CreateHook(EndScenePtr, reinterpret_cast<void*>(&HookEndScene), reinterpret_cast<void**>(&OrigEndScene)) != MH_OK)
		{
			MH_Uninitialize();
			return false;
		}

		if (MH_CreateHook(ResetPtr, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&OrigReset)) != MH_OK)
		{
			MH_RemoveHook(EndScenePtr);
			MH_Uninitialize();
			return false;
		}

		if (MH_CreateHook(CreateDevicePtr, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&OrigCreateDevice)) != MH_OK)
		{
			MH_RemoveHook(EndScenePtr);
			MH_RemoveHook(ResetPtr);
			MH_Uninitialize();
			return false;
		}

		if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
		{
			MH_RemoveHook(EndScenePtr);
			MH_RemoveHook(ResetPtr);
			MH_RemoveHook(CreateDevicePtr);
			MH_Uninitialize();
			return false;
		}

		Installed = true;
		return true;
	}
	/*
	void Shutdown()
	{
		WantShutdown = true;
		if (!Installed)
			return;

		if (ImGuiReady)
		{
			ImGui_ImplDX9_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			ImGuiReady = false;
		}

		if (hWnd && PrevWndProc)
		{
			::SetWindowLongA(hWnd, GWL_WNDPROC, reinterpret_cast<LONG>(PrevWndProc));
			PrevWndProc = nullptr;
			hWnd = nullptr;
		}

		MH_Uninitialize();
		OrigEndScene = nullptr;
		OrigReset = nullptr;

		Installed = false;
	}
	*/
	void SetOpen(bool open)
	{
		ExecutorOpen = open;
	}

	static std::int32_t ParseLuaErrorLine(std::string_view l)
	{
		if (l.empty())
			return -1;

		std::int32_t lastline = -1;

		for (size_t i = 0; i + 1 < l.size(); ++i)
		{
			if (l[i] != ':' || !std::isdigit(static_cast<unsigned char>(l[i + 1])))
				continue;

			const char* begin = l.data() + i + 1;
			const char* end = l.data() + l.size();

			std::int32_t line = 0;
			auto res = std::from_chars(begin, end, line);

			if (res.ec == std::errc{} && res.ptr < end && *res.ptr == ':' && line > 0)
				lastline = line;
		}

		return lastline;
	}

	void AppendLog(std::string_view line, LogLine::Level level)
	{
		LogLine log{ level, std::string(line) };

		std::lock_guard lock(LogMutex);
		LogLines.push_back(log);
		if (LogLines.size() > MaxLogLines)
			LogLines.erase(LogLines.begin(), LogLines.begin() + (LogLines.size() - MaxLogLines));

		if (level == LogLine::Level::Error)
		{
			TextEditor::ErrorMarkers markers;
			std::int32_t ln = ParseLuaErrorLine(line);
			if (ln > 0)
				markers[ln] = line;
			else
				markers[1] = line;

			LuaEditor.SetErrorMarkers(markers);
		}

		OutputScrollToBottom = true;
	}

	void ClearLog()
	{
		std::lock_guard lock(LogMutex);
		LogLines.clear();
	}
}