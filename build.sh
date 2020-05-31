#!/bin/sh -v
g++ -I./Seasocks/seasocks/src/main/c/ -I./g2log/g2log/src  -L./Seasocks/seasocks/build/src/main/c -L./g2log/g2log/build AnalyticalServer.cpp -lseasocks -lpthread -llib_g2logger -o AnalyticalServer
chmod +x AnalyticalServer
