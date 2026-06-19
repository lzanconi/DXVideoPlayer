#pragma once
#include <d3d11.h>
#include <string>

class IApp;

class DXShader
{
private:
    const std::string RESET = "\033[0m";
    const std::string RED = "\033[31m";

public:
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;

public:
	DXShader() = default;
    ~DXShader();

    bool LoadFromFile(ID3D11Device* device, const std::wstring& filename);

private:
    bool CompileVertexShader(ID3D11Device* device, const std::wstring& filename);
    bool CompilePixelShader(ID3D11Device* device, const std::wstring& filename);
    void OutputCompileErrors(ID3DBlob* errorBlob, const std::wstring& filename, const std::wstring& shaderType);
};

