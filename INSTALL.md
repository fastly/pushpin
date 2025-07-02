## Building from source
```sh
git clone -b cache-dev https://github.com/radiumb/pushpin.git
```

### Dependencies
---
If you’re using a recent version of Debian or Ubuntu, then all dependencies can be installed via package management:
```sh
sudo apt-get install make pkg-config rustc cargo g++ libzmq3-dev libssl-dev libboost-dev qtbase5-dev
```
If you’re on macOS, then all dependencies can be installed via Homebrew:
```sh
brew install pkg-config rust zeromq boost qt
```
### Compiling
---
Simply run make:
```sh
make
```
Optionally, you can install:
```sh
sudo make install
```
## Running
Running on Debian and Ubuntu
```sh
sudo pushpin --loglevel=2,proxy:3 --logfile=/var/log/pushpin/pushpin.log
```
loglevel=2 : only show warnings. 3 : show debug infos
