#python dependencies
sudo pip3 install pyautogui
sudo apt-get install scrot
sudo apt-get install python3-tk python3-dev


git clone https://github.com/axboe/liburing.git
cd ./liburing
./configure --cc=gcc --cxx=g++
make -j$(nproc)
sudo make install
