// stdafx.h 
#pragma once


#define WIN32_LEAN_AND_MEAN		
#include <windows.h>
#include <stdio.h>
#include <direct.h>
#include "d3d9.h"
#include "D3DX9Shader.h"
#include "D3dx9core.h"
#include "D3Dcompiler.h"
#include <map>
#include <vector>

struct VSO {
	IDirect3DVertexShader9* Left;
	IDirect3DVertexShader9* Neutral;
	IDirect3DVertexShader9* Right;
};

std::map<IDirect3DVertexShader9*, VSO> VSOmap;