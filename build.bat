echo on

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

meson build --backend=ninja -Dexample=true -Dtests=true
ninja -C build
ninja -C build test