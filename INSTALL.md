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
loglevel=2 : only show warnings, infos. 3 : show debug messages

## Configuration
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
### pushpin.conf file
For general options, please refer to [this](https://pushpin.org/docs/configuration/#pushpinconf-file).
Options for cache are added into this file.
* <a name="cache_enable"></a>cache enable flag (false=disable, true=enable)
	```
	cache_enable=true
	```
#### backends
* <a name="http_backend_urls"></a>http url path list of backends when failed to get response
	```
	http_backend_urls=http://localhost:7999/ws1,http://localhost:7999/ws2,http://localhost:7999/ws3 ...
	```
* <a name="ws_backend_urls"></a>websocket url paths for cache clients to connect to pushpin
	```
	ws_backend_urls=ws://localhost:7999/ws1,ws://localhost:7999/ws2,ws://localhost:7999/ws3 ...
	```
#### cache methods
* <a name="cache_methods"></a>cache methods
	```
	ws_cache_methods=author_hasKey,author_hasSessionKeys,...
	ws_subscribe_methods=beefy_subscribeJustifications,chain_subscribeAllHeads,...
	(* means ALL methods, ex : ws_cache_methods=*)
	```
#### cache refresh
* <a name="ws_never_timeout_methods"></a>never timeout/delete methods with params<br />
	ref: List of methods that are not even delete/auto-refresh if it has the valid parameters
	```
	ws_never_timeout_methods=chain_getBlockHash,chain_getBlock,chain_getHeader,state_queryStorageAt,...
	```
* <a name="ws_refresh_shorter_methods"></a>cache auto-refresh shorter timeout methods<br />
	ref: List of methods with shorter auto-refresh timeout periods (5 seconds)
	```
	ws_auto_refresh_longer_timeout_methods=chain_getBlockHash,chain_getHeader,state_getKeysPaged,state_queryStorageAt,...
	```
* <a name="ws_refresh_longer_methods"></a>cache auto-refresh longer timeout methods<br />
	ref: List of methods with longer auto-refresh timeout periods (60 seconds)
	```
	ws_refresh_longer_methods=state_getMetadata,...
	```
* <a name="ws_refresh_unerase_methods"></a>cache auto-refresh no delete methods<br />
	ref: List of methods that exist permanently in the cache list and do auto-refresh periodically
	```
	ws_refresh_unerase_methods=chain_getBlockHash,system_health,eth_getBalance,...
	```
* <a name="ws_refresh_exclude_methods"></a>cache no auto-refresh methods<br />
	ref: List of methods that need to be cached but not auto refreshed
	```
	ws_refresh_exclude_methods=state_getMetadata,...
	```
* <a name="ws_refresh_passthrough_methods"></a>cache pass-through methods<br />
	ref: List of methods that are not even cached but pass though to backend i.e. don't reject
	```
	ws_refresh_passthrough_methods=state_getStorage,...
	```
#### cache keys
* <a name="ws_cache_key"></a>important fields in request, used as a key to identify cache items
	```
	ws_cache_key = $request_json_value["method"]+$user_defined["request_args"]+$request_json_pair["jsonrpc"]
	request_args = $request_json_value["params"]
	(ex : $request_json_value["method"]="author_hasKey", $request_json_pair["jsonrpc"]="\"jsonrpc\":\"2.0\"")
	```
* <a name="message_id_attribute"></a>field name is used as ID in Request or Response (to support multi-protocol in future)
	```
	message_id_attribute="id"
	```
* <a name="message_method_attribute"></a>field name is used as Method in Request or Response (to support multi-protocol in future)
	```
	message_method_attribute="method"
	```
* <a name="message_params_attribute"></a>field name is used as Params in Request (to support never_timeout_methods_with_params)
	```
	message_params_attribute="params"
	```
#### prometheus
* <a name="prometheus_restore_allow_seconds"></a>prometheus restore allow seconds (default 300)
	```
	prometheus_restore_allow_seconds=250
	```
#### redis
* <a name="redis_enable"></a>redis enable flag (default false)
	```
	redis_enable=true
	```
* <a name="redis_host_addr"></a>redis server ip address (default 127.0.0.1)
	```
	redis_host_addr=127.0.0.1
	```
* <a name="redis_port"></a>redis server port number (default 6379)
	```
	redis_port=6379
	```
* <a name="redis_pool_count"></a>redis server port number (default 100)
	```
	redis_pool_count=100
	```
* <a name="redis_key_header"></a>Redis key prefix to identify each Pushpin instance when sharing Redis between multiple instances.
	```
	redis_key_header="pushpin1:"
	```
* <a name="replica_master_addr"></a>IP address of the replication master. If this is set to a valid IP address, Redis will act as a replica and attempt to connect to the specified master.
	```
	replica_master_addr=""
	```
* <a name="replica_master_port"></a>replication master port number (default 6379). Specifies the port Redis should use to connect to the master.
	```
	replica_master_port=6379
	```
#### Counts
* <a name="prometheus_count"></a>groups for promethus count
	```
	ws_count_groups=ws_key_group,ws_index_group,ws_block_group,ws_state_subscribe,ws_cache_candidates
	ws_key_group=author_hasKey,author_insertKey,...
	ws_block_group=chain_getBlock,chain_getBlockHash,...
	ws_state_subscribe=state_subscribeRuntimeVersion,state_subscribeStorage,...
	ws_cache_candidates=author_hasKey,author_hasSessionKeys,...
	```

## Appendix
### Installing redis-server
	```
	sudo apt update
	sudo apt install redis-server -y
	sudo systemctl enable redis-server
	sudo systemctl start redis-server
	```
