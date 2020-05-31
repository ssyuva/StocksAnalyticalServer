#!/bin/sh -v
#install seasocks and g2lib

echo "Cloning seasocks.."
git clone https://github.com/mattgodbolt/seasocks

echo "Installing seasocks.."
cd Seasocks/seasocks
mkdir build
cd build
cmake ..
make

echo "Installing seasocks..done"



echo "Cloning g2lib"
hg clone https://bitbucket.org/KjellKod/g2log

echo "Installing g2lib"
cd g2log/g2log
mkdir build
cd build 
cmake ..
make
echo "Installing g2lib..done"
