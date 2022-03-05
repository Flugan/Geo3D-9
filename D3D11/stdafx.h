// stdafx.h 
#pragma once


#define WIN32_LEAN_AND_MEAN		
#include <windows.h>
#include <share.h>
#include <stdio.h>
#include <direct.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include "d3d11.h"

using namespace std;

struct VSO {
	ID3D11VertexShader* Left;
	ID3D11VertexShader* Neutral;
	ID3D11VertexShader* Right;
};

map<ID3D11VertexShader*, VSO> VSOmap;

vector<byte> disassembler(vector<byte> buffer);
vector<byte> assembler(vector<byte> asmFile, vector<byte> buffer);
vector<byte> readFile(string fileName);
string shaderModel(byte* buffer);
vector<string> stringToLines(const char* start, int size);