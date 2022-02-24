// proxydll.cpp
#include "stdafx.h"
#include "proxydll.h"
#define NO_STEREO_D3D9
#define NO_STEREO_D3D10
#include "..\nvstereo.h"
#include <Xinput.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "resource.h"
#include "..\Nektra\NktHookLib.h"
#include "..\vkeys.h"
#include "..\log.h"

// global variables
#pragma data_seg (".d3d11_shared")
HINSTANCE			gl_hThisInstance;
HINSTANCE           gl_hOriginalDll = NULL;
bool				gl_hookedDevice = false;
bool				gl_hookedContext = false;
bool				gl_dumpBin = false;
bool				gl_log = false;
bool				gl_cache_shaders = false;
bool				gl_fix_enabled = true;
CRITICAL_SECTION	gl_CS;
// Our parameters for the stereo parameters texture.
DirectX::XMFLOAT4	iniParams[INI_PARAMS_SIZE];
FILE*				LogFile = NULL;
ID3D11Texture1D* gIniTexture = NULL;
ID3D11ShaderResourceView* gIniResourceView = NULL;
#pragma data_seg ()

CNktHookLib cHookMgr;

typedef HMODULE(WINAPI *lpfnLoadLibraryExW)(_In_ LPCWSTR lpLibFileName, _Reserved_ HANDLE hFile, _In_ DWORD dwFlags);
static HMODULE WINAPI Hooked_LoadLibraryExW(_In_ LPCWSTR lpLibFileName, _Reserved_ HANDLE hFile, _In_ DWORD dwFlags);
static struct
{
	SIZE_T nHookId;
	lpfnLoadLibraryExW fnLoadLibraryExW;
} sLoadLibraryExW_Hook = { 40, NULL };

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

char cwd[MAX_PATH];

#pragma region hook
typedef HRESULT(STDMETHODCALLTYPE* D3D11_VS)(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader);
static struct {
	SIZE_T nHookId;
	D3D11_VS fnCreateVertexShader;
} sCreateVertexShader_Hook = { 0, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateVertexShader(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	HRESULT res = sCreateVertexShader_Hook.fnCreateVertexShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
	return res;
}

typedef void(STDMETHODCALLTYPE* D3D11_GIC)(ID3D11Device* This, ID3D11DeviceContext** ppImmediateContext);
static struct {
	SIZE_T nHookId;
	D3D11_GIC fnGetImmediateContext;
} sGetImmediateContext_Hook = { 0, NULL };
void STDMETHODCALLTYPE D3D11_GetImmediateContext(ID3D11Device* This, ID3D11DeviceContext** ppImmediateContext) {
	sGetImmediateContext_Hook.fnGetImmediateContext(This, ppImmediateContext);
	hook(ppImmediateContext);
}

typedef void(STDMETHODCALLTYPE* D3D11C_VSSS)(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_VSSS fnVSSetShader;
} sVSSetShader_Hook = { 0, NULL };
void STDMETHODCALLTYPE D3D11C_VSSetShader(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sVSSetShader_Hook.fnVSSetShader(This, pVertexShader, ppClassInstances, NumClassInstances);
}

void hook(ID3D11DeviceContext** ppContext) {
	if (ppContext != NULL && *ppContext != NULL) {
		LogInfo("Hook  Context: %p\n", *ppContext);
		if (!gl_hookedContext) {
			LogInfo("Hook attatched\n");
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppContext;
			D3D11C_VSSS origVSSS = (D3D11C_VSSS)(*vTable)[11];

			cHookMgr.Hook(&(sVSSetShader_Hook.nHookId), (LPVOID*)&(sVSSetShader_Hook.fnVSSetShader), origVSSS, D3D11C_VSSetShader);

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
	return sDXGI_Present_Hook.fnDXGI_Present(This, SyncInterval, Flags);
}

void hook(ID3D11Device** ppDevice) {
	if (ppDevice != NULL && *ppDevice != NULL) {
		LogInfo("Hook device: %p\n", *ppDevice);
		if (!gl_hookedDevice) {
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppDevice;

			D3D11_VS origVS = (D3D11_VS)(*vTable)[12];

			D3D11_GIC origGIC = (D3D11_GIC)(*vTable)[40];

			cHookMgr.Hook(&(sCreateVertexShader_Hook.nHookId), (LPVOID*)&(sCreateVertexShader_Hook.fnCreateVertexShader), origVS, D3D11_CreateVertexShader);

			cHookMgr.Hook(&(sGetImmediateContext_Hook.nHookId), (LPVOID*)&(sGetImmediateContext_Hook.fnGetImmediateContext), origGIC, D3D11_GetImmediateContext);

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

	InitializeCriticalSection(&gl_CS);

	_getcwd(INIfile, MAX_PATH);
	_getcwd(LOGfile, MAX_PATH);
	strcat_s(INIfile, MAX_PATH, "\\d3dx.ini");
	_getcwd(cwd, MAX_PATH);

	// If specified in Debug section, wait for Attach to Debugger.
	bool waitfordebugger = GetPrivateProfileInt("Debug", "attach", 0, INIfile) > 0;
	if (waitfordebugger) {
		do {
			Sleep(250);
		} while (!IsDebuggerPresent());
	}

	gl_dumpBin = GetPrivateProfileInt("Rendering", "export_binary", gl_dumpBin, INIfile) > 0;
	gl_log = GetPrivateProfileInt("Logging", "calls", gl_log, INIfile) > 0;
	gl_cache_shaders = GetPrivateProfileInt("Rendering", "cache_shaders", gl_cache_shaders, INIfile) > 0;

	

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