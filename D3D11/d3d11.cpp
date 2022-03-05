// proxydll.cpp
#include "stdafx.h"
#include "resource.h"
#include "..\Nektra\NktHookLib.h"
#include "..\log.h"

// global variables
#pragma data_seg (".d3d11_shared")
HINSTANCE			gl_hThisInstance;
HINSTANCE           gl_hOriginalDll = NULL;
bool				gl_hookedDevice = false;
bool				gl_hookedContext = false;
bool				gl_log = false;
bool				gl_left = true;
FILE*				LogFile = NULL;
string				sep = "0.1";
string				conv = "1.0";
#pragma data_seg ()

CNktHookLib cHookMgr;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	bool result = true;

	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		gl_hThisInstance = hinstDLL;
		InitInstance();
		break;

	case DLL_PROCESS_DETACH:
		ExitInstance();
		break;

	case DLL_THREAD_ATTACH:
		// Do thread-specific initialization.
		break;

	case DLL_THREAD_DETACH:
		// Do thread-specific cleanup.
		break;
	}

	return result;
}

// 64 bit magic FNV-0 and FNV-1 prime
#define FNV_64_PRIME ((UINT64)0x100000001b3ULL)
static UINT64 fnv_64_buf(const void *buf, size_t len)
{
	UINT64 hval = 0;
	unsigned const char *bp = (unsigned const char *)buf;	/* start of buffer */
	unsigned const char *be = bp + len;		/* beyond end of buffer */

	// FNV-1 hash each octet of the buffer
	while (bp < be) {
		// multiply by the 64 bit FNV magic prime mod 2^64 */
		hval *= FNV_64_PRIME;
		// xor the bottom with the current octet
		hval ^= (UINT64)*bp++;
	}
	return hval;
}

string changeASM(vector<byte> ASM, bool left) {
	auto lines = stringToLines((char*)ASM.data(), ASM.size());
	string shader;
	string oReg;
	bool dcl = false;
	bool dcl_ICB = false;
	int temp = 0;
	for (int i = 0; i < lines.size(); i++) {
		string s = lines[i];
		if (s.find("dcl") == 0) {
			dcl = true;
			dcl_ICB = false;
			if (s.find("dcl_output_siv") == 0 && s.find("position") != string::npos) {
				oReg = s.substr(15, 2);
				shader += s + "\n";
			}
			else if (s.find("dcl_temps") == 0) {
				string num = s.substr(10);
				temp = atoi(num.c_str()) + 2;
				shader += "dcl_temps " + to_string(temp) + "\n";
			}
			else if (s.find("dcl_immediateConstantBuffer") == 0) {
				dcl_ICB = true;
				shader += s + "\n";
			}
			else {
				shader += s + "\n";
			}
		}
		else if (dcl_ICB == true) {
			shader += s + "\n";
		}
		else if (dcl == true) {
			// after dcl
			if (s.find("ret") < s.size()) {
				string changeSep = left ? "l(-" + sep + ")" : "l(" + sep + ")";
				shader +=
					"eq r" + to_string(temp - 2) + ".x, r" + to_string(temp - 1) + ".w, l(1.0)\n" +
					"if_z r" + to_string(temp - 2) + ".x\n"
					"  add r" + to_string(temp - 2) + ".x, r" + to_string(temp - 1) + ".w, l(-" + conv + ")\n" +
					"  mad r" + to_string(temp - 2) + ".x, r" + to_string(temp - 2) + ".x, " + changeSep + ", r" + to_string(temp - 1) + ".x\n" +
					"  mov " + oReg + ".x, r" + to_string(temp - 2) + ".x\n" +
					"  ret\n" +
					"endif\n";
			}
			if (oReg.size() == 0) {
				// no output
				return "";
			}
			if (temp == 0) {
				// add temps
				temp = 2;
				shader += "dcl_temps 2\n";
			}
			shader += s + "\n";
			auto pos = s.find(oReg);
			if (pos != string::npos) {
				string reg = "r" + to_string(temp - 1);
				for (int i = 0; i < s.size(); i++) {
					if (i < pos) {
						shader += s[i];
					}
					else if (i == pos) {
						shader += reg;
					}
					else if (i > pos + 1) {
						shader += s[i];
					}
				}
				shader += "\n";
			}
		}
		else {
			// before dcl
			shader += s + "\n";
		}
	}
	return shader;
}

#pragma region hook
typedef HRESULT(STDMETHODCALLTYPE* D3D11_VS)(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader);
static struct {
	SIZE_T nHookId;
	D3D11_VS fn;
} sCreateVertexShader_Hook = { 0, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateVertexShader(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) {
	UINT64 crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	HRESULT hr;
	vector<byte> v;
	vector<byte> a;
	VSO vso = {};
	vso.crc = crc;

	byte* bArray = (byte*)pShaderBytecode;
	for (int i = 0; i < BytecodeLength; i++) {
		v.push_back(bArray[i]);
	}
	vector<byte> ASM = disassembler(v);

	string shaderL = changeASM(ASM, true);
	string shaderR = changeASM(ASM, false);

	

	if (shaderL == "") {
		hr = sCreateVertexShader_Hook.fn(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
		vso.Neutral = (ID3D11VertexShader*)*ppVertexShader;
		VSOmap[vso.Neutral] = vso;
		return hr;
	}

	a.clear();
	for (int i = 0; i < shaderL.length(); i++) {
		a.push_back(shaderL[i]);
	}
	auto compiled = assembler(a, v);
	hr = sCreateVertexShader_Hook.fn(This, compiled.data(), compiled.size(), pClassLinkage, ppVertexShader);
	vso.Left = (ID3D11VertexShader*)*ppVertexShader;

	a.clear();
	for (int i = 0; i < shaderR.length(); i++) {
		a.push_back(shaderR[i]);
	}
	compiled = assembler(a, v);
	hr = sCreateVertexShader_Hook.fn(This, compiled.data(), compiled.size(), pClassLinkage, ppVertexShader);
	vso.Right = (ID3D11VertexShader*)*ppVertexShader;
	VSOmap[vso.Right] = vso;
	return hr;
}

typedef void(STDMETHODCALLTYPE* D3D11_GIC)(ID3D11Device* This, ID3D11DeviceContext** ppImmediateContext);
static struct {
	SIZE_T nHookId;
	D3D11_GIC fn;
} sGetImmediateContext_Hook = { 0, NULL };
void STDMETHODCALLTYPE D3D11_GetImmediateContext(ID3D11Device* This, ID3D11DeviceContext** ppImmediateContext) {
	sGetImmediateContext_Hook.fn(This, ppImmediateContext);
	hook(ppImmediateContext);
}

typedef void(STDMETHODCALLTYPE* D3D11C_VSSS)(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_VSSS fn;
} sVSSetShader_Hook = { 0, NULL };
void STDMETHODCALLTYPE D3D11C_VSSetShader(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	if (VSOmap.count(pVertexShader) == 1) {
		VSO* vso = &VSOmap[pVertexShader];
		if (vso->Neutral) {
			LogInfo("No output VS: %016llX\n", vso->crc);
			sVSSetShader_Hook.fn(This, vso->Neutral, ppClassInstances, NumClassInstances);
		}
		else {
			LogInfo("Stereo VS: %016llX\n", vso->crc);
			if (gl_left) {
				sVSSetShader_Hook.fn(This, vso->Left, ppClassInstances, NumClassInstances);
			}
			else {
				sVSSetShader_Hook.fn(This, vso->Right, ppClassInstances, NumClassInstances);
			}
		}
	}
	else {
		LogInfo("Unknown VS\n");
		sVSSetShader_Hook.fn(This, pVertexShader, ppClassInstances, NumClassInstances);
	}
}

void hook(ID3D11DeviceContext** ppContext) {
	if (ppContext != NULL && *ppContext != NULL) {
		LogInfo("Hook  Context: %p\n", *ppContext);
		if (!gl_hookedContext) {
			LogInfo("Hook attatched\n");
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppContext;
			D3D11C_VSSS origVSSS = (D3D11C_VSSS)(*vTable)[11];

			cHookMgr.Hook(&(sVSSetShader_Hook.nHookId), (LPVOID*)&(sVSSetShader_Hook.fn), origVSSS, D3D11C_VSSetShader);

			gl_hookedContext = true;
		}
	}
}

typedef HRESULT(STDMETHODCALLTYPE* DXGI_Present)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
static struct {
	SIZE_T nHookId;
	DXGI_Present fnDXGI_Present;
} sDXGI_Present_Hook = { 0, NULL };
HRESULT STDMETHODCALLTYPE DXGIH_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
	LogInfo("Present\n");
	gl_left = !gl_left;
	return sDXGI_Present_Hook.fnDXGI_Present(This, SyncInterval, Flags);
}

void hook(ID3D11Device** ppDevice) {
	if (ppDevice != NULL && *ppDevice != NULL) {
		LogInfo("Hook device: %p\n", *ppDevice);
		if (!gl_hookedDevice) {
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppDevice;

			D3D11_VS origVS = (D3D11_VS)(*vTable)[12];

			D3D11_GIC origGIC = (D3D11_GIC)(*vTable)[40];

			cHookMgr.Hook(&(sCreateVertexShader_Hook.nHookId), (LPVOID*)&(sCreateVertexShader_Hook.fn), origVS, D3D11_CreateVertexShader);

			cHookMgr.Hook(&(sGetImmediateContext_Hook.nHookId), (LPVOID*)&(sGetImmediateContext_Hook.fn), origGIC, D3D11_GetImmediateContext);

			IDXGIFactory1 * pFactory;
			HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&pFactory));

			// Temp window
			HWND dummyHWND = ::CreateWindow("STATIC", "dummy", WS_DISABLED, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
			::SetWindowTextA(dummyHWND, "Dummy Window!");

			// create a struct to hold information about the swap chain
			DXGI_SWAP_CHAIN_DESC scd;

			// clear out the struct for use
			ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC));

			// fill the swap chain description struct
			scd.BufferCount = 1;									// one back buffer
			scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;		// use 32-bit color
			scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;		// how swap chain is to be used
			scd.OutputWindow = dummyHWND;							// the window to be used
			scd.SampleDesc.Count = 1;								// how many multisamples
			scd.Windowed = TRUE;									// windowed/full-screen mode

			IDXGISwapChain * pSC;

			pFactory->CreateSwapChain(*ppDevice, &scd, &pSC);

			DWORD_PTR*** vTable2 = (DWORD_PTR***)pSC;
			DXGI_Present origPresent = (DXGI_Present)(*vTable2)[8];

			pSC->Release();
			pFactory->Release();
			::DestroyWindow(dummyHWND);

			cHookMgr.Hook(&(sDXGI_Present_Hook.nHookId), (LPVOID*)&(sDXGI_Present_Hook.fnDXGI_Present), origPresent, DXGIH_Present);

			gl_hookedDevice = true;
		}
	}
}

// Exported function (faking d3d11.dll's export)
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, 
									UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"
	
	// Hooking IDirect3D Object from Original Library
	typedef HRESULT (WINAPI* D3D11_Type)(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
											UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);
	D3D11_Type D3D11CreateDevice_fn = (D3D11_Type) GetProcAddress( gl_hOriginalDll, "D3D11CreateDevice");
	HRESULT res = D3D11CreateDevice_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
	if (res == S_OK) {
		hook(ppDevice);
	}
	return res;
}
HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
							const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"

	// Hooking IDirect3D Object from Original Library
	typedef HRESULT(WINAPI* D3D11_Type)(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, INT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
							const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);
	D3D11_Type D3D11CreateDeviceAndSwapChain_fn = (D3D11_Type)GetProcAddress(gl_hOriginalDll, "D3D11CreateDeviceAndSwapChain");
	HRESULT res = D3D11CreateDeviceAndSwapChain_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
	if (res == S_OK) {
		hook(ppDevice);
	}
	return res;
}
#pragma endregion

void InitInstance() {
	// Initialisation
	char INIfile[MAX_PATH];
	char LOGfile[MAX_PATH];

	_getcwd(INIfile, MAX_PATH);
	_getcwd(LOGfile, MAX_PATH);
	strcat_s(INIfile, MAX_PATH, "\\d3dx.ini");

	// If specified in Debug section, wait for Attach to Debugger.
	bool waitfordebugger = GetPrivateProfileInt("Debug", "attach", 0, INIfile) > 0;
	if (waitfordebugger) {
		do {
			Sleep(250);
		} while (!IsDebuggerPresent());
	}

	gl_log = GetPrivateProfileInt("Logging", "calls", gl_log, INIfile) > 0;

	if (gl_log) {
		if (LogFile == NULL) {
			strcat_s(LOGfile, MAX_PATH, "\\d3d11_log.txt");
			LogFile = _fsopen(LOGfile, "w", _SH_DENYNO);
			setvbuf(LogFile, NULL, _IONBF, 0);
		}
	}
	LogInfo("ini loaded:\n");
}

void LoadOriginalDll(void) {
	wchar_t sysDir[MAX_PATH];
	::GetSystemDirectoryW(sysDir, MAX_PATH);
	wcscat_s(sysDir, MAX_PATH, L"\\D3D11.dll");
	if (!gl_hOriginalDll) gl_hOriginalDll = ::LoadLibraryExW(sysDir, NULL, NULL);
}

void ExitInstance() {    
	if (gl_hOriginalDll) {
		::FreeLibrary(gl_hOriginalDll);
	    gl_hOriginalDll = NULL;  
	}
}