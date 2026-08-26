#include <iostream>
#include <string_view>

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring_view{argv[1]} == L"--self-test")
    {
        return 0;
    }

    std::wcout << L"Forge Conductor Windows port bootstrap" << std::endl;
    return 0;
}
