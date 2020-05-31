#!/bin/sh -v

echo "Setting LD_LIBRARY_PATH.."
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:`pwd`/seasocks/build/src/main/c
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:`pwd`/g2log/g2log/build
export LD_LIBRARY_PATH

echo "Setting LD_LIBRARY_PATH..done"

echo "Building AnalyticalServer executable.."

g++ -I./seasocks/src/main/c/ -I./g2log/g2log/src  -L./seasocks/build/src/main/c -L./g2log/g2log/build AnalyticalServer.cpp -lseasocks -lpthread -llib_g2logger -o AnalyticalServer

chmod +x AnalyticalServer

echo "Building AnalyticalServer executable..done"
