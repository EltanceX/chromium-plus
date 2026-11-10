cd C:\src\src\out\Debug
rem step_id=8e1bb5dd-39f9-4cc8-9d3f-f10908a0b601
rem LINK blink_unittests.exe blink_unittests.exe.pdb
..\..\third_party\llvm-build\Release+Asserts\bin\lld-link.exe "/OUT:./blink_unittests.exe" /nologo -libpath:../../third_party/llvm-build/Release+Asserts/lib/clang/24/lib/windows "-libpath:../../../../Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/ATLMFC/lib/x64" "-libpath:../../../../Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/lib/x64" "-libpath:../../../../Program Files (x86)/Windows Kits/10/lib/10.0.28000.0/ucrt/x64" "-libpath:../../../../Program Files (x86)/Windows Kits/10/lib/10.0.28000.0/um/x64" /MACHINE:X64  "/PDB:./blink_unittests.exe.pdb" "@./blink_unittests.exe.rsp"
