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
### Configuration
Pushpin has two primary configuration files: `pushpin.conf` and routes. The `pushpin.conf` file refers to the routes file via the routesfile field:
```sh
[proxy]
routesfile=routes
```
If a relative path is used, then the file is looked up relative to the location of `pushpin.conf`.
```sh
sudo cp examples/config/pushpin.conf /usr/local/etc/pushpin/pushpin.conf
```
```sh
sudo cp examples/config/routes /usr/local/etc/pushpin/routes
```
## Running
Running on Debian and Ubuntu
```sh
sudo pushpin --loglevel=2,proxy:3 --logfile=/var/log/pushpin/pushpin.log
```
loglevel=2 : only show warnings. 3 : show debug infos
