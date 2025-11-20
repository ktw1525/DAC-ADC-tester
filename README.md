# DAC-ADC-tester
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libglew-dev
 
rm -rf CMakeFiles CMakeCache.txt build
mkdir build
cd build
cmake ..
make -j

