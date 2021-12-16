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
		ShowStartupScreen();
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

map<UINT64, bool> isCache;
map<UINT64, bool> hasStartPatch;
map<UINT64, bool> hasStartFix;

char cwd[MAX_PATH];

typedef HRESULT(STDMETHODCALLTYPE* D3D11_VS)(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader);
static struct {
	SIZE_T nHookId;
	D3D11_VS fnCreateVertexShader;
} sCreateVertexShader_Hook = { 1, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateVertexShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) {
	FILE* f;
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	char buffer[80];
	char path[MAX_PATH];
	
	LogInfo("Create VertexShader: %016llX\n", _crc);
	if (gl_dumpBin) {
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderCache");
		CreateDirectory(path, NULL);

		sprintf_s(buffer, 80, "\\ShaderCache\\%016llX-vs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(pShaderBytecode, 1, BytecodeLength, f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	ID3D11VertexShader *pVertexShaderNew;
	HRESULT res;
	if (isCache.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-vs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, file.data(), file.size(), pClassLinkage, ppVertexShader);
	} else if (hasStartPatch.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-vs.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		vector<byte> byteCode(BytecodeLength);
		memcpy(byteCode.data(), pShaderBytecode, BytecodeLength);

		byteCode = assembler(file, byteCode);
		if (gl_cache_shaders) {
			FILE* f;
			sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-vs.bin", _crc);
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, buffer);
			
			EnterCriticalSection(&gl_CS);
			fopen_s(&f, path, "wb");
			fwrite(byteCode.data(), 1, byteCode.size(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, byteCode.data(), byteCode.size(), pClassLinkage, ppVertexShader);
	} else if (hasStartFix.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-vs_replace.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		string shdModel = "vs_5_0";
		ID3DBlob* pByteCode = nullptr;
		ID3DBlob* pErrorMsgs = nullptr;
		HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
			"main", shdModel.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
		if (ret == S_OK) {
			if (gl_cache_shaders) {
				FILE* f;
				sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-vs.bin", _crc);
				path[0] = 0;
				strcat_s(path, MAX_PATH, cwd);
				strcat_s(path, MAX_PATH, buffer);

				EnterCriticalSection(&gl_CS);
				fopen_s(&f, path, "wb");
				fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
				fclose(f);
				LeaveCriticalSection(&gl_CS);
			}
			res = sCreateVertexShader_Hook.fnCreateVertexShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppVertexShader);
		} else {
			LogInfo("compile error:%s\n", path);
			res = sCreateVertexShader_Hook.fnCreateVertexShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
		}
	} else {
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
	}
	return res;
}
typedef HRESULT(STDMETHODCALLTYPE* D3D11_PS)(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader);
static struct {
	SIZE_T nHookId;
	D3D11_PS fnCreatePixelShader;
} sCreatePixelShader_Hook = { 2, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreatePixelShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) {
	FILE* f;
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	char buffer[80];
	char path[MAX_PATH];

	LogInfo("Create PixelShader: %016llX\n", _crc);

	if (gl_dumpBin) {
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderCache");
		CreateDirectory(path, NULL);

		sprintf_s(buffer, 80, "\\ShaderCache\\%016llX-ps.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(pShaderBytecode, 1, BytecodeLength, f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	HRESULT res;
	if (isCache.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ps.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, file.data(), file.size(), pClassLinkage, ppPixelShader);
	} else if (hasStartPatch.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ps.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		vector<byte> byteCode(BytecodeLength);
		memcpy(byteCode.data(), pShaderBytecode, BytecodeLength);

		byteCode = assembler(file, byteCode);
		if (gl_cache_shaders) {
			FILE* f;
			sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ps.bin", _crc);
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, buffer);

			EnterCriticalSection(&gl_CS);
			fopen_s(&f, path, "wb");
			fwrite(byteCode.data(), 1, byteCode.size(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, byteCode.data(), byteCode.size(), pClassLinkage, ppPixelShader);
	} else if (hasStartFix.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ps_replace.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		string shdModel = "ps_5_0";
		ID3DBlob* pByteCode = nullptr;
		ID3DBlob* pErrorMsgs = nullptr;
		HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
			"main", shdModel.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
		if (ret == S_OK) {
			if (gl_cache_shaders) {
				FILE* f;
				sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ps.bin", _crc);
				path[0] = 0;
				strcat_s(path, MAX_PATH, cwd);
				strcat_s(path, MAX_PATH, buffer);

				EnterCriticalSection(&gl_CS);
				fopen_s(&f, path, "wb");
				fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
				fclose(f);
				LeaveCriticalSection(&gl_CS);
			}
			res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppPixelShader);
		} else {
			LogInfo("compile error:\n%s", path);
			res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
		}
	} else {
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
	}
	return res;
}
typedef HRESULT(STDMETHODCALLTYPE* D3D11_CS)(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader);
static struct {
	SIZE_T nHookId;
	D3D11_CS fnCreateComputeShader;
} sCreateComputeShader_Hook = { 3, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateComputeShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader) {
	FILE* f;
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	char buffer[80];
	char path[MAX_PATH];

	LogInfo("Create ComputeShader: %016llX\n", _crc);

	if (gl_dumpBin) {
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderCache");
		CreateDirectory(path, NULL);

		sprintf_s(buffer, 80, "\\ShaderCache\\%016llX-cs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(pShaderBytecode, 1, BytecodeLength, f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	HRESULT res;
	if (isCache.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-cs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, file.data(), file.size(), pClassLinkage, ppComputeShader);
	} else if (hasStartPatch.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-cs.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		vector<byte> byteCode(BytecodeLength);
		memcpy(byteCode.data(), pShaderBytecode, BytecodeLength);

		byteCode = assembler(file, byteCode);
		if (gl_cache_shaders) {
			FILE* f;
			sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-cs.bin", _crc);
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, buffer);

			EnterCriticalSection(&gl_CS);
			fopen_s(&f, path, "wb");
			fwrite(byteCode.data(), 1, byteCode.size(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, byteCode.data(), byteCode.size(), pClassLinkage, ppComputeShader);
	} else if (hasStartFix.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-cs_replace.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		string shdModel = "cs_5_0";
		ID3DBlob* pByteCode = nullptr;
		ID3DBlob* pErrorMsgs = nullptr;
		HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
			"main", shdModel.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
		if (ret == S_OK) {
			if (gl_cache_shaders) {
				FILE* f;
				sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-cs.bin", _crc);
				path[0] = 0;
				strcat_s(path, MAX_PATH, cwd);
				strcat_s(path, MAX_PATH, buffer);

				EnterCriticalSection(&gl_CS);
				fopen_s(&f, path, "wb");
				fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
				fclose(f);
				LeaveCriticalSection(&gl_CS);
			}
			res = sCreateComputeShader_Hook.fnCreateComputeShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppComputeShader);
		} else {
			LogInfo("compile error:\n%s", path);
			res = sCreateComputeShader_Hook.fnCreateComputeShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader);
		}
	} else {
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader);
	}
	return res;
}
typedef HRESULT(STDMETHODCALLTYPE* D3D11_GS)(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader);
static struct {
	SIZE_T nHookId;
	D3D11_GS fnCreateGeometryShader;
} sCreateGeometryShader_Hook = { 4, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateGeometryShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) {
	FILE* f;
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	char buffer[80];
	char path[MAX_PATH];

	LogInfo("Create GeometryShader: %016llX\n", _crc);

	if (gl_dumpBin) {
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderCache");
		CreateDirectory(path, NULL);

		sprintf_s(buffer, 80, "\\ShaderCache\\%016llX-gs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(pShaderBytecode, 1, BytecodeLength, f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	HRESULT res;
	if (isCache.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-gs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, file.data(), file.size(), pClassLinkage, ppGeometryShader);
	} else if (hasStartPatch.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-gs.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		vector<byte> byteCode(BytecodeLength);
		memcpy(byteCode.data(), pShaderBytecode, BytecodeLength);

		byteCode = assembler(file, byteCode);
		if (gl_cache_shaders) {
			FILE* f;
			sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-gs.bin", _crc);
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, buffer);

			EnterCriticalSection(&gl_CS);
			fopen_s(&f, path, "wb");
			fwrite(byteCode.data(), 1, byteCode.size(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, byteCode.data(), byteCode.size(), pClassLinkage, ppGeometryShader);
	} else if (hasStartFix.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-gs_replace.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		string shdModel = "gs_5_0";
		ID3DBlob* pByteCode = nullptr;
		ID3DBlob* pErrorMsgs = nullptr;
		HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
			"main", shdModel.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
		if (ret == S_OK) {
			if (gl_cache_shaders) {
				FILE* f;
				sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-gs.bin", _crc);
				path[0] = 0;
				strcat_s(path, MAX_PATH, cwd);
				strcat_s(path, MAX_PATH, buffer);

				EnterCriticalSection(&gl_CS);
				fopen_s(&f, path, "wb");
				fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
				fclose(f);
				LeaveCriticalSection(&gl_CS);
			}
			res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppGeometryShader);
		} else {
			LogInfo("compile error:\n%s", path);
			res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader);
		}
	} else {
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader);
	}
	return res;
}
typedef HRESULT(STDMETHODCALLTYPE* D3D11_DS)(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader);
static struct {
	SIZE_T nHookId;
	D3D11_DS fnCreateDomainShader;
} sCreateDomainShader_Hook = { 5, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateDomainShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader) {
	FILE* f;
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	UINT64 _crc2 = 0;
	char buffer[80];
	char path[MAX_PATH];

	LogInfo("Create ComputeShader: %016llX\n", _crc);

	if (gl_dumpBin) {
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderCache");
		CreateDirectory(path, NULL);

		sprintf_s(buffer, 80, "\\ShaderCache\\%016llX-ds.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(pShaderBytecode, 1, BytecodeLength, f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	HRESULT res;
	if (isCache.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ds.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);
		_crc2 = fnv_64_buf(file.data(), file.size());
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, file.data(), file.size(), pClassLinkage, ppDomainShader);
	} else if (hasStartPatch.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ds.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		vector<byte> byteCode(BytecodeLength);
		memcpy(byteCode.data(), pShaderBytecode, BytecodeLength);

		byteCode = assembler(file, byteCode);
		_crc2 = fnv_64_buf(byteCode.data(), byteCode.size());
		if (gl_cache_shaders) {
			FILE* f;
			sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ds.bin", _crc);
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, buffer);

			EnterCriticalSection(&gl_CS);
			fopen_s(&f, path, "wb");
			fwrite(byteCode.data(), 1, byteCode.size(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, byteCode.data(), byteCode.size(), pClassLinkage, ppDomainShader);
	} else if (hasStartFix.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ds_replace.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		string shdModel = "ds_5_0";
		ID3DBlob* pByteCode = nullptr;
		ID3DBlob* pErrorMsgs = nullptr;
		HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
			"main", shdModel.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
		if (ret == S_OK) {
			_crc2 = fnv_64_buf(pByteCode->GetBufferPointer(), pByteCode->GetBufferSize());
			if (gl_cache_shaders) {
				FILE* f;
				sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-ds.bin", _crc);
				path[0] = 0;
				strcat_s(path, MAX_PATH, cwd);
				strcat_s(path, MAX_PATH, buffer);

				EnterCriticalSection(&gl_CS);
				fopen_s(&f, path, "wb");
				fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
				fclose(f);
				LeaveCriticalSection(&gl_CS);
			}
			res = sCreateDomainShader_Hook.fnCreateDomainShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppDomainShader);
		} else {
			LogInfo("compile error:\n%s", path);
			res = sCreateDomainShader_Hook.fnCreateDomainShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader);
		}
	} else {
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader);
	}
	return res;
}
typedef HRESULT(STDMETHODCALLTYPE* D3D11_HS)(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader);
static struct {
	SIZE_T nHookId;
	D3D11_HS fnCreateHullShader;
} sCreateHullShader_Hook = { 6, NULL };
HRESULT STDMETHODCALLTYPE D3D11_CreateHullShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader) {
	FILE* f;
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	char buffer[80];
	char path[MAX_PATH];

	LogInfo("Create GeometryShader: %016llX\n", _crc);

	if (gl_dumpBin) {
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderCache");
		CreateDirectory(path, NULL);

		sprintf_s(buffer, 80, "\\ShaderCache\\%016llX-hs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(pShaderBytecode, 1, BytecodeLength, f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	HRESULT res;
	if (isCache.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-hs.bin", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);
		res = sCreateHullShader_Hook.fnCreateHullShader(This, file.data(), file.size(), pClassLinkage, ppHullShader);
	} else if (hasStartPatch.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-hs.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		vector<byte> byteCode(BytecodeLength);
		memcpy(byteCode.data(), pShaderBytecode, BytecodeLength);

		byteCode = assembler(file, byteCode);
		if (gl_cache_shaders) {
			FILE* f;
			sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-hs.bin", _crc);
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, buffer);

			EnterCriticalSection(&gl_CS);
			fopen_s(&f, path, "wb");
			fwrite(byteCode.data(), 1, byteCode.size(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
		res = sCreateHullShader_Hook.fnCreateHullShader(This, byteCode.data(), byteCode.size(), pClassLinkage, ppHullShader);
	} else if (hasStartFix.count(_crc)) {
		sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-hs_replace.txt", _crc);
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, buffer);
		auto file = readFile(path);

		string shdModel = "hs_5_0";
		ID3DBlob* pByteCode = nullptr;
		ID3DBlob* pErrorMsgs = nullptr;
		HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
			"main", shdModel.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
		if (ret == S_OK) {
			if (gl_cache_shaders) {
				FILE* f;
				sprintf_s(buffer, 80, "\\ShaderFixes\\%016llX-hs.bin", _crc);
				path[0] = 0;
				strcat_s(path, MAX_PATH, cwd);
				strcat_s(path, MAX_PATH, buffer);

				EnterCriticalSection(&gl_CS);
				fopen_s(&f, path, "wb");
				fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
				fclose(f);
				LeaveCriticalSection(&gl_CS);
			}
			res = sCreateHullShader_Hook.fnCreateHullShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppHullShader);
		} else {
			LogInfo("compile error:\n%s", path);
			res = sCreateHullShader_Hook.fnCreateHullShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader);
		}
	}
	else {
		res = sCreateHullShader_Hook.fnCreateHullShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader);
	}
	return res;
}

class DeviceClass {
public:
	DeviceClass(ID3D11Device *This, nv::stereo::ParamTextureManagerD3D11 *TexMgr) {
		mDevice = This;
		mParamTextureManager = TexMgr;
		mStereoHandle = NULL;
		mStereoTexture = NULL;
		mStereoResourceView = NULL;
		mIniTexture = NULL;
		mIniResourceView = NULL;
	}

	ID3D11Device *mDevice;
	StereoHandle mStereoHandle;
	nv::stereo::ParamTextureManagerD3D11 *mParamTextureManager;
	ID3D11Texture2D *mStereoTexture;
	ID3D11ShaderResourceView *mStereoResourceView;
	ID3D11Texture1D *mIniTexture;
	ID3D11ShaderResourceView *mIniResourceView;
};

map<ID3D11DeviceContext *, DeviceClass *> deviceMap;
map<ID3D11Device *, DeviceClass *> Devices;

ID3D11DeviceContext * gContext = NULL;
DeviceClass * gDevice = NULL;

typedef void(STDMETHODCALLTYPE* D3D11_GIC)(ID3D11Device * This, ID3D11DeviceContext **ppImmediateContext);
static struct {
	SIZE_T nHookId;
	D3D11_GIC fnGetImmediateContext;
} sGetImmediateContext_Hook = { 14, NULL };
void STDMETHODCALLTYPE D3D11_GetImmediateContext(ID3D11Device * This, ID3D11DeviceContext **ppImmediateContext) {
	sGetImmediateContext_Hook.fnGetImmediateContext(This, ppImmediateContext);
	LogInfo("D3D11_GetImmediateContext, Device: %p, Context: %p\n", This, *ppImmediateContext);
	deviceMap[*ppImmediateContext] = Devices[This];
	hook(ppImmediateContext);
}

#pragma region SetShader
typedef void(STDMETHODCALLTYPE* D3D11C_PSSS)(ID3D11DeviceContext* This, ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_PSSS fnPSSetShader;
} sPSSetShader_Hook = { 15, NULL };
void STDMETHODCALLTYPE D3D11C_PSSetShader(ID3D11DeviceContext* This, ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sPSSetShader_Hook.fnPSSetShader(This, pPixelShader, ppClassInstances, NumClassInstances);
	if (gContext != This) {
		gDevice = deviceMap[This];
		gContext = This;
	}
	This->PSSetShaderResources(125, 1, &gDevice->mStereoResourceView);
	if (gDevice->mIniResourceView != NULL)
		This->PSSetShaderResources(120, 1, &gDevice->mIniResourceView);
}
typedef void(STDMETHODCALLTYPE* D3D11C_VSSS)(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_VSSS fnVSSetShader;
} sVSSetShader_Hook = { 16, NULL };
void STDMETHODCALLTYPE D3D11C_VSSetShader(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sVSSetShader_Hook.fnVSSetShader(This, pVertexShader, ppClassInstances, NumClassInstances);
	if (gContext != This) {
		gDevice = deviceMap[This];
		gContext = This;
	}
	This->VSSetShaderResources(125, 1, &gDevice->mStereoResourceView);
	if (gDevice->mIniResourceView != NULL)
		This->VSSetShaderResources(120, 1, &gDevice->mIniResourceView);
}
typedef void(STDMETHODCALLTYPE* D3D11C_CSSS)(ID3D11DeviceContext* This, ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_CSSS fnCSSetShader;
} sCSSetShader_Hook = { 17, NULL };
void STDMETHODCALLTYPE D3D11C_CSSetShader(ID3D11DeviceContext* This, ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sCSSetShader_Hook.fnCSSetShader(This, pComputeShader, ppClassInstances, NumClassInstances);
	if (gContext != This) {
		gDevice = deviceMap[This];
		gContext = This;
	}
	This->CSSetShaderResources(125, 1, &gDevice->mStereoResourceView);
	if (gDevice->mIniResourceView != NULL)
		This->CSSetShaderResources(120, 1, &gDevice->mIniResourceView);
}
typedef void(STDMETHODCALLTYPE* D3D11C_GSSS)(ID3D11DeviceContext* This, ID3D11GeometryShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_GSSS fnGSSetShader;
} sGSSetShader_Hook = { 18, NULL };
void STDMETHODCALLTYPE D3D11C_GSSetShader(ID3D11DeviceContext* This, ID3D11GeometryShader* pGeometryShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sGSSetShader_Hook.fnGSSetShader(This, pGeometryShader, ppClassInstances, NumClassInstances);
	if (gContext != This) {
		gDevice = deviceMap[This];
		gContext = This;
	}
	This->GSSetShaderResources(125, 1, &gDevice->mStereoResourceView);
	if (gDevice->mIniResourceView != NULL)
		This->GSSetShaderResources(120, 1, &gDevice->mIniResourceView);
}
typedef void(STDMETHODCALLTYPE* D3D11C_HSSS)(ID3D11DeviceContext* This, ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_HSSS fnHSSetShader;
} sHSSetShader_Hook = { 19, NULL };
void STDMETHODCALLTYPE D3D11C_HSSetShader(ID3D11DeviceContext* This, ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sHSSetShader_Hook.fnHSSetShader(This, pHullShader, ppClassInstances, NumClassInstances);
	if (gContext != This) {
		gDevice = deviceMap[This];
		gContext = This;
	}
	This->HSSetShaderResources(125, 1, &gDevice->mStereoResourceView);
	if (gDevice->mIniResourceView != NULL)
		This->HSSetShaderResources(120, 1, &gDevice->mIniResourceView);
}
typedef void(STDMETHODCALLTYPE* D3D11C_DSSS)(ID3D11DeviceContext* This, ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_DSSS fnDSSetShader;
} sDSSetShader_Hook = { 20, NULL };
void STDMETHODCALLTYPE D3D11C_DSSetShader(ID3D11DeviceContext* This, ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	sDSSetShader_Hook.fnDSSetShader(This, pDomainShader, ppClassInstances, NumClassInstances);
	if (gContext != This) {
		gDevice = deviceMap[This];
		gContext = This;
	}
	This->DSSetShaderResources(125, 1, &gDevice->mStereoResourceView);
	if (gDevice->mIniResourceView != NULL)
		This->DSSetShaderResources(120, 1, &gDevice->mIniResourceView);
}
#pragma endregion

#pragma region buttons
enum buttonPress { Unchanged, Down, Up };

class button {
public:
	virtual buttonPress buttonCheck() = 0;
};

class keyboardMouseKey : public button {
public:
	keyboardMouseKey(string s) {
		VKey = ParseVKey(s.c_str());
		oldState = 0;
	}
	buttonPress buttonCheck() {
		SHORT state = GetAsyncKeyState(VKey);
		buttonPress status = buttonPress::Unchanged;
		if ((state & 0x8000) && !(oldState & 0x8000)) {
			status = buttonPress::Down;
		}
		if (!(state & 0x8000) && (oldState & 0x8000)) {
			status = buttonPress::Up;
		}
		oldState = state;
		return status;
	}
private:
	SHORT oldState;
	int VKey;
};

WORD getXInputButton(const char* button) {
	if (_stricmp(button, "A") == 0)
		return XINPUT_GAMEPAD_A;
	if (_stricmp(button, "B") == 0)
		return XINPUT_GAMEPAD_B;
	if (_stricmp(button, "X") == 0)
		return XINPUT_GAMEPAD_X;
	if (_stricmp(button, "Y") == 0)
		return XINPUT_GAMEPAD_Y;
	if (_stricmp(button, "START") == 0)
		return XINPUT_GAMEPAD_START;
	if (_stricmp(button, "BACK") == 0)
		return XINPUT_GAMEPAD_BACK;
	if (_stricmp(button, "DPAD_RIGHT") == 0)
		return XINPUT_GAMEPAD_DPAD_RIGHT;
	if (_stricmp(button, "DPAD_LEFT") == 0)
		return XINPUT_GAMEPAD_DPAD_LEFT;
	if (_stricmp(button, "DPAD_UP") == 0)
		return XINPUT_GAMEPAD_DPAD_UP;
	if (_stricmp(button, "DPAD_DOWN") == 0)
		return XINPUT_GAMEPAD_DPAD_DOWN;
	if (_stricmp(button, "RIGHT_SHOULDER") == 0)
		return XINPUT_GAMEPAD_RIGHT_SHOULDER;
	if (_stricmp(button, "LEFT_SHOULDER") == 0)
		return XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (_stricmp(button, "RIGHT_THUMB") == 0)
		return XINPUT_GAMEPAD_RIGHT_THUMB;
	if (_stricmp(button, "LEFT_THUMB") == 0)
		return XINPUT_GAMEPAD_LEFT_THUMB;
	if (_stricmp(button, "LEFT_TRIGGER") == 0)
		return 0x400;
	if (_stricmp(button, "RIGHT_TRIGGER") == 0)
		return 0x800;
	return 0;
}

class xboxKey : public button {
public:
	xboxKey(string s) {
		if (s[2] == '_') {
			c = 0;
			XKey = getXInputButton(s.c_str() + 3);
		} else {
			c = s[2] - '0' - 1;
			XKey = getXInputButton(s.c_str() + 4);
		}
		ZeroMemory(&oldState, sizeof(XINPUT_STATE));
		XInputGetState(c, &oldState);
	}
	buttonPress buttonCheck() {
		buttonPress status = buttonPress::Unchanged;
		XINPUT_STATE state;
		ZeroMemory(&state, sizeof(XINPUT_STATE));
		XInputGetState(c, &state);
		if (XKey == 0x400) {
			if (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Down;
			if (state.Gamepad.bLeftTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Up;
		} else if (XKey == 0x800) {
			if (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Down;;
			if (state.Gamepad.bRightTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Up;
		} else {
			if (state.Gamepad.wButtons & XKey && !(oldState.Gamepad.wButtons & XKey))
				status = buttonPress::Down;
			if (!(state.Gamepad.wButtons & XKey) && oldState.Gamepad.wButtons & XKey)
				status = buttonPress::Up;
		}
		oldState = state;
		return status;
	}
private:
	XINPUT_STATE oldState;
	WORD XKey;
	int c;
};

button* createButton(string key) {
	if (_strnicmp(key.c_str(), "XB", 2) == 0) {
		return new xboxKey(key);
	} else {
		return new keyboardMouseKey(key);
	}
}

string& trim(string& str)
{
	str.erase(str.begin(), find_if(str.begin(), str.end(),
		[](char& ch)->bool { return !isspace(ch); }));
	str.erase(find_if(str.rbegin(), str.rend(),
		[](char& ch)->bool { return !isspace(ch); }).base(), str.end());
	return str;
}

enum KeyType { Activate, Hold, Toggle, Cycle };
enum TransitionType { Linear, Cosine };

class ButtonHandler {
public:
	ButtonHandler(button* b, KeyType type, int variable, int IniNum, vector<string> value, TransitionType tt, TransitionType rtt) {
		Button = b;
		Type = type;
		Variable = variable;
		TT = tt;
		rTT = rtt;

		delay = 0;
		transition = 0;
		releaseDelay = 0;
		releaseTransition = 0;

		cyclePosition = 0;
		Value = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
		if (Type == KeyType::Cycle) {
			for (int i = 0; i < 8; i++) {
				LogInfo("%s\n", value[i].c_str());
				if (variable & 1 << i) {
					vector<float> store;
					while (true) {
						int pos = value[i].find(',');
						if (pos == value[i].npos) {
							string val = value[i];
							val = trim(val);
							if (val.size() == 0) {
								store.push_back(FLT_MAX);
							} else {
								store.push_back(stof(val));
							}
							break;
						} else {
							string val = value[i].substr(0, pos);
							val = trim(val);
							if (val.size() == 0) {
								store.push_back(FLT_MAX);
							} else {
								store.push_back(stof(val));
							}
							value[i] = value[i].substr(pos + 1);
						}
					}
					mArray.push_back(store);
				} else {
					vector<float> store;
					store.push_back(FLT_MAX);
					mArray.push_back(store);
				}
			}
			for (int i = 0; i < 8; i++) {
				maxSize = max(maxSize, mArray[i].size());
			}
			for (int i = 0; i < 8; i++) {
				if (maxSize > mArray[i].size()) {
					for (int j = mArray[i].size(); j < maxSize; j++) {
						mArray[i].push_back(mArray[i][j - 1]);
					}
				}
			}
			initializeDelay(cyclePosition);
		} else {
			if (variable & 0x001) Value[0] = stof(value[0]);
			if (variable & 0x002) Value[1] = stof(value[1]);
			if (variable & 0x004) Value[2] = stof(value[2]);
			if (variable & 0x008) Value[3] = stof(value[3]);
			if (variable & 0x010) Value[4] = stof(value[4]);
			if (variable & 0x020) Value[5] = stof(value[5]);
			if (variable & 0x040) delay = stol(value[6]);
			if (variable & 0x080) transition = stol(value[7]);
			if (variable & 0x100) releaseDelay = stol(value[8]);
			if (variable & 0x200) releaseTransition = stol(value[9]);
		}
		SavedValue = readINI();
		toggleDown = true;

		curDelay = 0;
		curDelayUp = 0;
		curTransition = 0;
		curTransitionUp = 0;

		iniNum = iniNum;
	}
	void initializeDelay(int c) {
		delay = 0;
		if (mArray[6][c] != FLT_MAX)
			delay = mArray[6][c];
	}
	void initializeCycle(int c) {
		Variable = 0;
		for (int i = 0; i < 6; i++) {
			if (mArray[i][c] != FLT_MAX) {
				Variable |= 1 << i;
				Value[i] = mArray[i][c];
			}
		}
		transition = 0;
		if (mArray[7][c] != FLT_MAX)
			transition = mArray[7][c];
	}
	void Handle() {
		buttonPress status = Button->buttonCheck();

		if (status == buttonPress::Down) {
			if (delay > 0) {
				curDelay = GetTickCount64() + delay;
			} else {
				buttonDown();
			}
		}
		if (status == buttonPress::Up) {
			if (releaseDelay > 0) {
				curDelayUp = GetTickCount64() + releaseDelay;
			} else {
				buttonUp();
			}
		}

		if (delay > 0 && curDelay > 0 && GetTickCount64() > curDelay) {
			buttonDown();
			curDelay = 0;
		}
		if (releaseDelay > 0 && curDelayUp > 0 && GetTickCount64() > curDelayUp) {
			buttonUp();
			curDelayUp = 0;
		}
		if (transition > 0 && curTransition > 0) {
			if (GetTickCount64() > curTransition) {
				setVariable(transitionVariable(transition, curTransition, TT));
				curTransition = 0;
			} else {
				ULONGLONG newTick = GetTickCount64();
				if (newTick != lastTick) {
					setVariable(transitionVariable(transition, curTransition, TT));
					lastTick = newTick;
				}
			}
		}
		if (releaseTransition > 0 && curTransitionUp > 0) {
			if (GetTickCount64() > curTransitionUp) {
				setVariable(transitionVariable(releaseTransition, curTransitionUp, rTT));
				curTransitionUp = 0;
			} else {
				ULONGLONG newTick = GetTickCount64();
				if (newTick != lastTick) {
					setVariable(transitionVariable(releaseTransition, curTransitionUp, rTT));
					lastTick = newTick;
				}
			}
		}
	}
private:
	void buttonUp() {
		if (Type == KeyType::Hold) {
			sT = readVariable();
			Store = SavedValue;
			if (curDelay > 0)
				curDelay = 0; // cancel delayed keypress
			if (curTransition > 0)
				curTransition = 0; // cancel transition
			if (releaseTransition > 0) {
				lastTick = GetTickCount64();
				curTransitionUp = lastTick + releaseTransition;
			} else {
				setVariable(Store);
			}
		}
	}
	void buttonDown() {
		sT = readVariable();
		if (Type == KeyType::Toggle) {
			if (toggleDown) {
				SavedValue = readStereo(SavedValue);
				toggleDown = false;
				Store = Value;
			} else {
				toggleDown = true;
				Store = SavedValue;
			}
		} else if (Type == KeyType::Hold) {
			if (curDelayUp > 0)
				curDelayUp = 0; // cancel delayed keypress
			if (curTransitionUp > 0)
				curTransitionUp = 0; // cancel transition
			SavedValue = readStereo(SavedValue);
			Store = Value;
		} else if (Type == KeyType::Activate) {
			Store = Value;
		} else if (Type == KeyType::Cycle) {
			initializeCycle(cyclePosition++);
			if (cyclePosition == maxSize)
				cyclePosition = 0;
			initializeDelay(cyclePosition);
			Store = Value;
		}
		if (transition > 0) {
			lastTick = GetTickCount64();
			curTransition = lastTick + transition;
		} else {
			setVariable(Store);
		}
	}
	vector<float> transitionVariable(ULONGLONG transition, ULONGLONG curTransition, TransitionType tt) {
		vector<float> f(6);
		ULONGLONG transitionAmount = transition;
		if (GetTickCount64() < curTransition) {
			transitionAmount = transition - (curTransition - GetTickCount64());
		}
		float percentage = transitionAmount / (float)transition;
		if (tt == TransitionType::Cosine)
			percentage = (1 - cos(percentage * M_PI)) / 2;
		if (Variable & 0x01) f[0] = sT[0] + (Store[0] - sT[0]) * percentage;
		if (Variable & 0x02) f[1] = sT[1] + (Store[1] - sT[1]) * percentage;
		if (Variable & 0x04) f[2] = sT[2] + (Store[2] - sT[2]) * percentage;
		if (Variable & 0x08) f[3] = sT[3] + (Store[3] - sT[3]) * percentage;
		if (Variable & 0x10) f[4] = sT[4] + (Store[4] - sT[4]) * percentage;
		if (Variable & 0x20) f[5] = sT[5] + (Store[5] - sT[5]) * percentage;
		return f;
	}
	vector<float> readVariable() {
		vector<float> f(6);
		if (Variable & 0x01) f[0] = iniParams[iniNum].x;
		if (Variable & 0x02) f[1] = iniParams[iniNum].y;
		if (Variable & 0x04) f[2] = iniParams[iniNum].z;
		if (Variable & 0x08) f[3] = iniParams[iniNum].w;
		if (Variable & 0x10) NvAPI_Stereo_GetConvergence(gDevice->mStereoHandle, &f[4]);
		if (Variable & 0x20) NvAPI_Stereo_GetSeparation(gDevice->mStereoHandle, &f[5]);
		return f;
	}
	vector<float> readINI() {
		vector<float> f(6);
		if (Variable & 0x01) f[0] = iniParams[iniNum].x;
		if (Variable & 0x02) f[1] = iniParams[iniNum].y;
		if (Variable & 0x04) f[2] = iniParams[iniNum].z;
		if (Variable & 0x08) f[3] = iniParams[iniNum].w;
		return f;
	}
	vector<float> readStereo(vector<float> f) {
		if (Variable & 0x10) NvAPI_Stereo_GetConvergence(gDevice->mStereoHandle, &f[4]);
		if (Variable & 0x20) NvAPI_Stereo_GetSeparation(gDevice->mStereoHandle, &f[5]);
		return f;
	}
	void setVariable(vector<float> f) {
		if (Variable & 0x01) iniParams[iniNum].x = f[0];
		if (Variable & 0x02) iniParams[iniNum].y = f[1];
		if (Variable & 0x04) iniParams[iniNum].z = f[2];
		if (Variable & 0x08) iniParams[iniNum].w = f[3];
		if (Variable & 0x10) NvAPI_Stereo_SetConvergence(gDevice->mStereoHandle, f[4]);
		if (Variable & 0x20) NvAPI_Stereo_SetSeparation(gDevice->mStereoHandle, f[5]);
		if (Variable & 0x0F) {
			D3D11_MAPPED_SUBRESOURCE mappedResource;
			gContext->Map(gDevice->mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			memcpy(mappedResource.pData, &iniParams, sizeof(iniParams));
			gContext->Unmap(gDevice->mIniTexture, 0);
		}
	}
	button* Button;
	KeyType Type;
	// Variable Flags
	// 1 INIParams.x
	// 2 INIParams.y
	// 4 INIParams.z
	// 8 INIParams.w
	// 16 Convergence
	// 32 Separation
	int Variable;
	int iniNum;
	TransitionType TT;
	TransitionType rTT;
	vector<float> Value;
	vector<float> SavedValue;
	vector<float> Store;
	vector<float> sT; // start transition
	ULONGLONG lastTick;

	ULONGLONG delay;
	ULONGLONG releaseDelay;
	ULONGLONG curDelay;
	ULONGLONG curDelayUp;

	ULONGLONG transition;
	ULONGLONG releaseTransition;
	ULONGLONG curTransition;
	ULONGLONG curTransitionUp;
	bool toggleDown;
	int cyclePosition;
	vector<vector<float>> mArray;
	int maxSize = 0;
};

vector<ButtonHandler*> BHs;

void frameFunction() {
	for (size_t i = 0; i < BHs.size(); i++) {
		BHs[i]->Handle();
	}
}
#pragma endregion

#pragma region hook
void hook(ID3D11DeviceContext** ppContext) {
	if (ppContext != NULL && *ppContext != NULL) {
		LogInfo("Hook  Context: %p\n", *ppContext);
		if (!gl_hookedContext) {
			LogInfo("Hook attatched\n");
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppContext;
			D3D11C_PSSS origPSSS = (D3D11C_PSSS)(*vTable)[9];
			D3D11C_VSSS origVSSS = (D3D11C_VSSS)(*vTable)[11];
			D3D11C_GSSS origGSSS = (D3D11C_GSSS)(*vTable)[23];
			D3D11C_HSSS origHSSS = (D3D11C_HSSS)(*vTable)[60];
			D3D11C_DSSS origDSSS = (D3D11C_DSSS)(*vTable)[64];
			D3D11C_CSSS origCSSS = (D3D11C_CSSS)(*vTable)[69];

			cHookMgr.Hook(&(sPSSetShader_Hook.nHookId), (LPVOID*)&(sPSSetShader_Hook.fnPSSetShader), origPSSS, D3D11C_PSSetShader);
			cHookMgr.Hook(&(sVSSetShader_Hook.nHookId), (LPVOID*)&(sVSSetShader_Hook.fnVSSetShader), origVSSS, D3D11C_VSSetShader);
			cHookMgr.Hook(&(sGSSetShader_Hook.nHookId), (LPVOID*)&(sGSSetShader_Hook.fnGSSetShader), origGSSS, D3D11C_GSSetShader);
			cHookMgr.Hook(&(sHSSetShader_Hook.nHookId), (LPVOID*)&(sHSSetShader_Hook.fnHSSetShader), origHSSS, D3D11C_HSSetShader);
			cHookMgr.Hook(&(sDSSetShader_Hook.nHookId), (LPVOID*)&(sDSSetShader_Hook.fnDSSetShader), origDSSS, D3D11C_DSSetShader);
			cHookMgr.Hook(&(sCSSetShader_Hook.nHookId), (LPVOID*)&(sCSSetShader_Hook.fnCSSetShader), origCSSS, D3D11C_CSSetShader);

			gl_hookedContext = true;
		}
	}
}

HRESULT CreateStereoParamTextureAndView(ID3D11Device* d3d11)
{
	// This function creates a texture that is suitable to be stereoized by the driver.
	// Note that the parameters primarily come from nvstereo.h
	using nv::stereo::ParamTextureManagerD3D11;

	HRESULT hr = 0;

	D3D11_TEXTURE2D_DESC desc;
	desc.Width = ParamTextureManagerD3D11::Parms::StereoTexWidth;
	desc.Height = ParamTextureManagerD3D11::Parms::StereoTexHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = ParamTextureManagerD3D11::Parms::StereoTexFormat;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = 0;
	d3d11->CreateTexture2D(&desc, NULL, &Devices[d3d11]->mStereoTexture);

	// Since we need to bind the texture to a shader input, we also need a resource view.
	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;
	descRV.Format = desc.Format;
	descRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels = 1;
	descRV.Texture2D.MostDetailedMip = 0;
	descRV.Texture2DArray.MostDetailedMip = 0;
	descRV.Texture2DArray.MipLevels = 1;
	descRV.Texture2DArray.FirstArraySlice = 0;
	descRV.Texture2DArray.ArraySize = desc.ArraySize;
	d3d11->CreateShaderResourceView(Devices[d3d11]->mStereoTexture, &descRV, &Devices[d3d11]->mStereoResourceView);

	return S_OK;
}

void CreateINITexture(ID3D11Device* d3d11) {
	D3D11_TEXTURE1D_DESC desc;
	memset(&desc, 0, sizeof(D3D11_TEXTURE1D_DESC));
	D3D11_SUBRESOURCE_DATA initialData;
	initialData.pSysMem = &iniParams;
	initialData.SysMemPitch = sizeof(DirectX::XMFLOAT4) * INI_PARAMS_SIZE;	// only one 4 element struct

	desc.Width = 1;												// 1 texel, .rgba as a float4
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;	// float4
	desc.Usage = D3D11_USAGE_DYNAMIC;				// Read/Write access from GPU and CPU
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;		// As resource view, access via t120
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;		// allow CPU access for hotkeys
	desc.MiscFlags = 0;
	HRESULT ret = d3d11->CreateTexture1D(&desc, &initialData, &Devices[d3d11]->mIniTexture);
	// Since we need to bind the texture to a shader input, we also need a resource view.
	// The pDesc is set to NULL so that it will simply use the desc format above.
	ret = d3d11->CreateShaderResourceView(Devices[d3d11]->mIniTexture, NULL, &Devices[d3d11]->mIniResourceView);
}

typedef HRESULT(STDMETHODCALLTYPE* DXGI_Present)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
static struct {
	SIZE_T nHookId;
	DXGI_Present fnDXGI_Present;
} sDXGI_Present_Hook = { 12, NULL };
HRESULT STDMETHODCALLTYPE DXGIH_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
	LogInfo("Present\n");
	frameFunction();
	if (gDevice != 0)
		gDevice->mParamTextureManager->UpdateStereoTexture(gDevice->mDevice, gContext, gDevice->mStereoTexture, false);
	return sDXGI_Present_Hook.fnDXGI_Present(This, SyncInterval, Flags);
}

void hook(ID3D11Device** ppDevice, nv::stereo::ParamTextureManagerD3D11 *gStereoTexMgr) {
	if (ppDevice != NULL && *ppDevice != NULL) {
		LogInfo("Hook device: %p\n", *ppDevice);
		if (!gl_hookedDevice) {
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppDevice;
			//D3D11_CT2D origCT2D = (D3D11_CT2D)(*vTable)[5];

			D3D11_VS origVS = (D3D11_VS)(*vTable)[12];
			D3D11_GS origGS = (D3D11_GS)(*vTable)[13];
			D3D11_PS origPS = (D3D11_PS)(*vTable)[15];
			D3D11_HS origHS = (D3D11_HS)(*vTable)[16];
			D3D11_DS origDS = (D3D11_DS)(*vTable)[17];
			D3D11_CS origCS = (D3D11_CS)(*vTable)[18];

			D3D11_GIC origGIC = (D3D11_GIC)(*vTable)[40];

			//cHookMgr.Hook(&(sCreateTexture2D_Hook.nHookId), (LPVOID*)&(sCreateTexture2D_Hook.fnCreateTexture2D), origCT2D, D3D11_CreateTexture2D);

			cHookMgr.Hook(&(sCreatePixelShader_Hook.nHookId), (LPVOID*)&(sCreatePixelShader_Hook.fnCreatePixelShader), origPS, D3D11_CreatePixelShader);
			cHookMgr.Hook(&(sCreateVertexShader_Hook.nHookId), (LPVOID*)&(sCreateVertexShader_Hook.fnCreateVertexShader), origVS, D3D11_CreateVertexShader);
			cHookMgr.Hook(&(sCreateComputeShader_Hook.nHookId), (LPVOID*)&(sCreateComputeShader_Hook.fnCreateComputeShader), origCS, D3D11_CreateComputeShader);
			cHookMgr.Hook(&(sCreateGeometryShader_Hook.nHookId), (LPVOID*)&(sCreateGeometryShader_Hook.fnCreateGeometryShader), origGS, D3D11_CreateGeometryShader);
			cHookMgr.Hook(&(sCreateDomainShader_Hook.nHookId), (LPVOID*)&(sCreateDomainShader_Hook.fnCreateDomainShader), origDS, D3D11_CreateDomainShader);
			cHookMgr.Hook(&(sCreateHullShader_Hook.nHookId), (LPVOID*)&(sCreateHullShader_Hook.fnCreateHullShader), origHS, D3D11_CreateHullShader);

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

		auto gDevice = *ppDevice;
		auto device = new DeviceClass(gDevice, gStereoTexMgr);
		Devices[gDevice] = device;
		if (NVAPI_OK != NvAPI_Stereo_CreateHandleFromIUnknown(*ppDevice, &device->mStereoHandle))
			device->mStereoHandle = 0;

		CreateINITexture(gDevice);
		// Create our stereo parameter texture
		CreateStereoParamTextureAndView(gDevice);
		// Initialize the stereo texture manager. Note that the StereoTextureManager was created
		// before the device. This is important, because NvAPI_Stereo_CreateConfigurationProfileRegistryKey
		// must be called BEFORE device creation.
		gStereoTexMgr->Init(gDevice);
	} else {
		delete gStereoTexMgr;
	}
}

void ShowStartupScreen()
{
	BOOL affinity = -1;
	DWORD_PTR one = 0x01;
	DWORD_PTR before = 0;
	DWORD_PTR before2 = 0;
	affinity = GetProcessAffinityMask(GetCurrentProcess(), &before, &before2);
	affinity = SetProcessAffinityMask(GetCurrentProcess(), one);
	HBITMAP hBM = ::LoadBitmap(gl_hThisInstance, MAKEINTRESOURCE(IDB_STARTUP));
	if (hBM) {
		HDC hDC = ::GetDC(NULL);
		if (hDC) {
			int iXPos = (::GetDeviceCaps(hDC, HORZRES) / 2) - (128 / 2);
			int iYPos = (::GetDeviceCaps(hDC, VERTRES) / 2) - (128 / 2);

			// paint the "GPP active" sign on desktop
			HDC hMemDC = ::CreateCompatibleDC(hDC);
			HBITMAP hBMold = (HBITMAP) ::SelectObject(hMemDC, hBM);
			::BitBlt(hDC, iXPos, iYPos, 128, 128, hMemDC, 0, 0, SRCCOPY);

			//Cleanup
			::SelectObject(hMemDC, hBMold);
			::DeleteDC(hMemDC);
			::ReleaseDC(NULL, hDC);

			// Wait 2 seconds before proceeding
			::Sleep(2000);
		}
		::DeleteObject(hBM);
	}
	affinity = SetProcessAffinityMask(GetCurrentProcess(), before);
}

// Exported function (faking d3d11.dll's export)
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, 
									UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"
	
	// Hooking IDirect3D Object from Original Library
	typedef HRESULT (WINAPI* D3D11_Type)(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
											UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);
	D3D11_Type D3D11CreateDevice_fn = (D3D11_Type) GetProcAddress( gl_hOriginalDll, "D3D11CreateDevice");
	// ParamTextureManager must be created before the device to give our settings-loading code a chance to fire.
	auto gStereoTexMgr = new nv::stereo::ParamTextureManagerD3D11;
	HRESULT res = D3D11CreateDevice_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
	if (!FAILED(res)) {
		hook(ppDevice, gStereoTexMgr);
	} else {
		delete gStereoTexMgr;
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
	// ParamTextureManager must be created before the device to give our settings-loading code a chance to fire.
	auto gStereoTexMgr = new nv::stereo::ParamTextureManagerD3D11;
	HRESULT res = D3D11CreateDeviceAndSwapChain_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
	if (!FAILED(res)) {
		hook(ppDevice, gStereoTexMgr);
	} else {
		delete gStereoTexMgr;
	}
	return res;
}
#pragma endregion

void InitInstance()
{
	// Initialisation
	char setting[MAX_PATH];
	char INIfile[MAX_PATH];
	char LOGfile[MAX_PATH];
	int read;

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

	// Read in any constants defined in the ini, for use as shader parameters
	for (int i = 0; i < INI_PARAMS_SIZE; i++) {
		iniParams[i].x = FLT_MAX;
		iniParams[i].y = FLT_MAX;
		iniParams[i].z = FLT_MAX;
		iniParams[i].w = FLT_MAX;
	}

	read = GetPrivateProfileString("Constants", "x", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[0].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[0].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[0].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[0].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x1", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[1].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y1", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[1].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z1", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[1].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w1", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[1].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x2", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[2].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y2", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[2].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z2", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[2].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w2", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[2].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x3", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[3].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y3", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[3].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z3", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[3].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w3", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[3].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x4", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[4].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y4", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[4].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z4", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[4].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w4", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[4].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x5", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[5].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y5", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[5].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z5", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[5].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w5", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[5].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x6", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[6].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y6", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[6].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z6", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[6].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w6", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[6].w = stof(setting);

	read = GetPrivateProfileString("Constants", "x7", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[7].x = stof(setting);
	read = GetPrivateProfileString("Constants", "y7", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[7].y = stof(setting);
	read = GetPrivateProfileString("Constants", "z7", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[7].z = stof(setting);
	read = GetPrivateProfileString("Constants", "w7", 0, setting, MAX_PATH, INIfile);
	if (read) iniParams[7].w = stof(setting);

	if (gl_log) {
		if (LogFile == NULL) {
			strcat_s(LOGfile, MAX_PATH, "\\d3d11_log.txt");
			LogFile = _fsopen(LOGfile, "w", _SH_DENYNO);
			setvbuf(LogFile, NULL, _IONBF, 0);
		}
	}

	KeyType type;
	char key[MAX_PATH];
	char buf[MAX_PATH];

	vector<string> Keys;
	vector<string> Textures;
	char sectionNames[10000];
	GetPrivateProfileSectionNames(sectionNames, 10000, INIfile);
	int position = 0;
	int length = strlen(&sectionNames[position]);
	while (length != 0) {
		if (strncmp(&sectionNames[position], "Key", 3) == 0)
			Keys.push_back(&sectionNames[position]);
		position += length + 1;
		length = strlen(&sectionNames[position]);
	}

	for (size_t i = 0; i < Keys.size(); i++) {
		const char* id = Keys[i].c_str();
		if (!GetPrivateProfileString(id, "Key", 0, key, MAX_PATH, INIfile))
			continue;

		type = KeyType::Activate;

		if (GetPrivateProfileString(id, "type", 0, buf, MAX_PATH, INIfile)) {
			if (!_stricmp(buf, "hold")) {
				type = KeyType::Hold;
			} else if (!_stricmp(buf, "toggle")) {
				type = KeyType::Toggle;
			} else if (!_stricmp(buf, "cycle")) {
				type = KeyType::Cycle;
			}
		}

		TransitionType tType = TransitionType::Linear;
		if (GetPrivateProfileString(id, "transition_type", 0, buf, MAX_PATH, INIfile)) {
			if (!_stricmp(buf, "cosine"))
				tType = TransitionType::Cosine;
		}

		TransitionType rtType = TransitionType::Linear;
		if (GetPrivateProfileString(id, "release_transition_type", 0, buf, MAX_PATH, INIfile)) {
			if (!_stricmp(buf, "cosine"))
				rtType = TransitionType::Cosine;
		}

		vector<string> fs = { "", "", "", "", "", "", "", "", "", "" };
		int varFlags = 0;

		int iniNum = 0;
		if (GetPrivateProfileString(id, "x", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
		}
		if (GetPrivateProfileString(id, "y", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
		}
		if (GetPrivateProfileString(id, "z", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
		}
		if (GetPrivateProfileString(id, "w", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
		}
		if (GetPrivateProfileString(id, "x0", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
		}
		if (GetPrivateProfileString(id, "y0", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
		}
		if (GetPrivateProfileString(id, "z0", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
		}
		if (GetPrivateProfileString(id, "w0", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
		}
		if (GetPrivateProfileString(id, "x1", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 1;
		}
		if (GetPrivateProfileString(id, "y1", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 1;
		}
		if (GetPrivateProfileString(id, "z1", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 1;
		}
		if (GetPrivateProfileString(id, "w1", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 1;
		}
		if (GetPrivateProfileString(id, "x2", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 2;
		}
		if (GetPrivateProfileString(id, "y2", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 2;
		}
		if (GetPrivateProfileString(id, "z2", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 2;
		}
		if (GetPrivateProfileString(id, "w2", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 2;
		}
		if (GetPrivateProfileString(id, "x3", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 3;
		}
		if (GetPrivateProfileString(id, "y3", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 3;
		}
		if (GetPrivateProfileString(id, "z3", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 3;
		}
		if (GetPrivateProfileString(id, "w3", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 3;
		}
		if (GetPrivateProfileString(id, "x4", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 4;
		}
		if (GetPrivateProfileString(id, "y4", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 4;
		}
		if (GetPrivateProfileString(id, "z4", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 4;
		}
		if (GetPrivateProfileString(id, "w4", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 4;
		}
		if (GetPrivateProfileString(id, "x5", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 5;
		}
		if (GetPrivateProfileString(id, "y5", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 5;
		}
		if (GetPrivateProfileString(id, "z5", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 5;
		}
		if (GetPrivateProfileString(id, "w5", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 5;
		}
		if (GetPrivateProfileString(id, "x6", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 6;
		}
		if (GetPrivateProfileString(id, "y6", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 6;
		}
		if (GetPrivateProfileString(id, "z6", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 6;
		}
		if (GetPrivateProfileString(id, "w6", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 6;
		}
		if (GetPrivateProfileString(id, "x7", 0, buf, MAX_PATH, INIfile)) {
			fs[0] = buf;
			varFlags |= 1;
			iniNum = 7;
		}
		if (GetPrivateProfileString(id, "y7", 0, buf, MAX_PATH, INIfile)) {
			fs[1] = buf;
			varFlags |= 2;
			iniNum = 7;
		}
		if (GetPrivateProfileString(id, "z7", 0, buf, MAX_PATH, INIfile)) {
			fs[2] = buf;
			varFlags |= 4;
			iniNum = 7;
		}
		if (GetPrivateProfileString(id, "w7", 0, buf, MAX_PATH, INIfile)) {
			fs[3] = buf;
			varFlags |= 8;
			iniNum = 7;
		}
		if (GetPrivateProfileString(id, "convergence", 0, buf, MAX_PATH, INIfile)) {
			fs[4] = buf;
			varFlags |= 16;
		}
		if (GetPrivateProfileString(id, "separation", 0, buf, MAX_PATH, INIfile)) {
			fs[5] = buf;
			varFlags |= 32;
		}
		if (GetPrivateProfileString(id, "delay", 0, buf, MAX_PATH, INIfile)) {
			fs[6] = buf;
			varFlags |= 64;
		}
		if (GetPrivateProfileString(id, "transition", 0, buf, MAX_PATH, INIfile)) {
			fs[7] = buf;
			varFlags |= 128;
		}
		if (GetPrivateProfileString(id, "release_delay", 0, buf, MAX_PATH, INIfile)) {
			fs[8] = buf;
			varFlags |= 256;
		}
		if (GetPrivateProfileString(id, "release_transition", 0, buf, MAX_PATH, INIfile)) {
			fs[9] = buf;
			varFlags |= 512;
		}
		//BHs.push_back(new ButtonHandler(createButton(key), type, varFlags, iniNum, fs, tType, rtType));
	}

	WIN32_FIND_DATA findFileData;

	HANDLE hFind = FindFirstFile("ShaderFixes\\????????????????-??.bin", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			isCache[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}

	hFind = FindFirstFile("ShaderFixes\\????????????????-??.txt", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			hasStartPatch[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}

	hFind = FindFirstFile("ShaderFixes\\????????????????-??_replace.txt", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			hasStartFix[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}
	LogInfo("ini loaded:\n");
}

void LoadOriginalDll(void)
{
	wchar_t sysDir[MAX_PATH];
	::GetSystemDirectoryW(sysDir, MAX_PATH);
	wcscat_s(sysDir, MAX_PATH, L"\\D3D11.dll");
	if (!gl_hOriginalDll) gl_hOriginalDll = ::LoadLibraryExW(sysDir, NULL, NULL);
}

void ExitInstance() 
{    
	if (gl_hOriginalDll)
	{
		::FreeLibrary(gl_hOriginalDll);
	    gl_hOriginalDll = NULL;  
	}
}