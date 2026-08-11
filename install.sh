#!/bin/bash -e

# compile gramine-tdx
    meson setup build-debug/ --buildtype=debug -Dtests=enabled \
              -Dskeleton=enabled -Ddirect=enabled -Dsgx=disabled -Dvm=enabled -Dtdx=disabled \
              --prefix=$PWD/built-debug

    ninja -C build-debug/
    ninja -C build-debug/ install

echo "Gramine-tdx installed successfully"

echo "Please add the following line to your .bashrc file:"
echo "export PATH=$PWD/built-debug/bin:$PATH"
