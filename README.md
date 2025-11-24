
# NGNIX NACOS 模块（插件）

nginx 订阅 nacos，实现 服务发现 和 配置 动态更新。 nacos 1.x 使用 udp 协议，nacos 2.x 使用 grpc 协议，本项目都支持
- 贡献者指南请参见 [AGENTS.md](AGENTS.md)。
- nginx 订阅 nacos 获取后端服务地址，不用在 upstream 中配置 ip 地址，后端服务发布下线自动更新。
- nginx 订阅 nacos 获取配置，写入 nginx 标准变量。（配置功能，只支持 grpc 协议）。
- nginx 自身作为服务，注册到 nacos。（只支持 grpc 协议）。
- openresty 中，使用 lua 脚本动态订阅 nacos 的服务和配置 .（只支持 grpc 协议）。

### 配置示例
基于 NACOS 2.x 版本开发。openresty is required if using lua.

```
nacos {
    server_list localhost:8848; # nacos 服务器列表，空格隔开

    # 可以使用 grpc 订阅。 nacos 2.x 版本才支持，推荐使用。
    grpc_server_list localhost:9848; # nacos grpc服务器列表，空格隔开。一般是 nacos 端口 + 1000
    
    # 也可使用udp。都配置的情况下使用 udp。
    #udp_port 19999; #udp 端口号
    #udp_ip 127.0.0.1; #udp ip 地址。
    #udp_bind 0.0.0.0:19999; # 绑定udp 地址
    error_log logs/nacos.log info;
    default_group DEFAULT_GROUP; # 默认的nacos group name
    cache_dir cmake-build-debug/nacos/;
    # local_ip 本机 ip 地址，不用于 物理连接，仅用于注册 或者 grpc 字段。不配置则扫描本机网卡获取
    
    ## 把 nginx 自己注册到 nacos. serviceName=nginx-server
    register nginx-server {
        # 注册到 nacos 的 ip，可选，不填则使用 nacos local_ip
        # ip 127.0.0.1;
        // 注册的端口
        port 9999;
        // 注册的 group name
        group DEFAULT_GROUP;
        // 集群名称
        cluster daily-default;
        // 注册的权重,nacos 的权重 是 0-1 之间的浮点数。与这里的权重是 1/100 的关系
        weight 100;
        // 注册服务的健康状态
        healthy on;
        // 自定义元数据
        metadata from nginx;
    }
}

http {
    upstream s {
        # 不需要写死 后端 ip。配置 nacos 服务名，nginx 自己订阅
        # server 127.0.0.1:8080;
        # 如果provider使用的spring，service_name 要和 spring.application.name一致
        # 不知道 provider 端怎么写请参考 https://github.com/zhwaaaaaa/springmvc-nacos-register
        nacos_subscribe_service service_name=springmvc-nacos-demo group=DEFAULT_GROUP;
        # upstream 使用的集群。可以配置多个，优先级从前到后。
        nacos_use_cluster $cluster_c1 $cluster_c2;
    }
    # include a lua file subscribe nacos. look conf/nacos.lua. (require openresty add nacos_lua module)
    init_nacos_by_lua_file ../conf/nacos.lua;
    
    # 订阅 nacos 的配置。$n_var = "content"  $dd = "md5"
    nacos_config_var $n_var data_id=aaabbbbccc group=DEFAULT_GROUP md5_var=$dd default=123456;
    
    server {
        # ... other config s
        location ^~ / {
            # 把 从 nacos 拿到的配置加入 header
            add_header X-Var-Nacos "$n_var" always;
            # 反向代理 到 s 服务。s服务是 从 nacos 动态订阅的。
            proxy_pass http://s;
        }
        location ^~ /echo {
            nacos_config_var $n_var data_id=ccdd;
            # 把 从 nacos 拿到的配置加入 header 和 body
            add_header X-Var-Nacos "$n_var";
            return 200 "hear .... $n_var .... $dd";
        }
        # ===== openresty access nacos config data ===
        location ^~ /echo-by-lua {
            content_by_lua_block {
                local md5 = ngx.var.md5_var;
                local content = ngx.var.n_var;
                if not md5 then
                    md5 = "not found md5";
                end
                if not content then
                    content = "not found content";
                end
                ngx.say("md5 = "..md5.."  content = "..content);
            }
        }
    }
}
```

### nginx 编译
- 本项目支持 nginx 1.10 及以上版本，所以需要提前下载相应版本的 [nginx](https://nginx.org/download)。例如1.15.2
```bash
wget https://nginx.org/download/nginx-1.15.2.tar.gz
tar zxvf nginx-1.15.2.tar.gz
```
- 下载本项目源代码 nginx-nacos-upstream
```bash
git clone https://github.com/nacos-group/nginx-nacos-upstream.git
```

- 如果选择以单独的辅助进程订阅nacos，则需要patch修改 nginx 源代码（可选但推荐：单独的nacos辅助进程不会因为本项目bug导致worker进程无法处理流量），否则在worker0 进程订阅nacos
```bash
cd nginx-1.15.2 && patch -p1 < ../nginx-nacos-upstream/patch/nginx.patch
```
- build nginx. ubuntu 下安装方式为

```bash
 sudo apt install build-essential libpcre3 libpcre3-dev zlib1g zlib1g-dev libssl-dev
./configure --add-module=../nginx-nacos-upstream/modules/auxiliary --add-module=../nginx-nacos-upstream/modules/nacos --with-http_ssl_module --with-http_v2_module && make
```

### openresty 编译
- 本项目支持 openresty 1.10 及以上版本，所以需要提前下载相应版本的 [openresty](https://openresty.org/cn/download.html)。例如1.25.3.2
```bash
wget https://openresty.org/download/openresty-1.25.3.2.tar.gz
tar zxvf openresty-1.25.3.2.tar.gz
```
- 下载本项目源代码 nginx-nacos-upstream .
```bash
git clone https://github.com/nacos-group/nginx-nacos-upstream.git
```

- 本项目对 nginx 源代码有少量修改,所以需要 打上 patch. 注意，这里使用的是openresty.patch 并且进入 bundle 下的 nginx 目录执行 (可选,同 nginx)
```bash
cd openresty-1.25.3.2/bundle/nginx-1.25.3 && patch -p1 < ../../../nginx-nacos-upstream/patch/openresty.patch
```

- build nginx. ubuntu 下安装方式为

```bash
cd ../..
sudo apt install build-essential libpcre3 libpcre3-dev zlib1g zlib1g-dev libssl-dev
./configure \
  --add-module=../nginx-nacos-upstream/modules/auxiliary \
  --add-module=../nginx-nacos-upstream/modules/nacos \
  --add-module=../nginx-nacos-upstream/modules/nacos_lua
make
```

### 原理
 - 新增加一个 auxiliary 模块, 启动一个单独辅助进程（或者在 worker0 进程），用于订阅和接受 nacos 的 grpc 或者 udp 消息推送，不影响 worker 进程的工作。
 - 收到消息推送后更新到共享内存，便于 worker 进程可以拿到最新的推送。 推送的数据也会缓存到磁盘，下次启动时候首先从磁盘读取。

# 详细配置
### nacos block
nacos {} 必需放在 http {} 的前面。下面的 小标题 配置 指令都放在 nacos {} 中

#### server_list
配置 nacos 的 http 地址，这个地址必需要配置。
```
 server_list ip1:8848 ip2:8848 ip3:8848;
```
#### grpc_server_list
配置 nacos 的 grpc 地址，nacos server 需要升级 到 2.x 版本才能支持。（推荐）
```
 grpc_server_list ip1:9848 ip2:9848 ip3:9848;
```

#### udp_port  （不推荐）
配置 nginx 的 udp 端口号。nacos 1.x 使用的是 udp 推送，nginx 会开启 udp 端口接受 nacos server 推送。不支持 配置
```
 udp_port 19999;
```
#### udp_ip  （不推荐）
nginx 自己的 ip。nginx 会把自己的 udp ip 和端口告诉 nacos, nacos 会给 nginx 发送 udp 消息。
```
 udp_ip 127.0.0.1;
```
#### udp_bind（不推荐）
nginx 监听的 udp ip 和端口。原则上和 上边的 ip+port 一致。如果使用的是 docker。上面配置的则 可能是主机的 ip 和 映射的端口
```
udp_bind 0.0.0.0:19999;
```

#### username （服务端没开启 auth 不需要）
nacos 开启 auth 之后的 username. nacos.core.auth.server.identity.key=xxxx
```
username "xxxx";
```

#### password （服务端没开启 auth 不需要）
nacos 开启 auth 之后的 password. nacos.core.auth.server.identity.value=xxxx
```
password "xxxx";
```

#### error_log 
nacos 日志文件 和 级别.
```
error_log logs/nacos.log info;
```
#### default_group (默认 DEFAULT_GROUP)
nacos 使用的是默认的 group。订阅的时候 可能只是制定了 data_id 或 service_name,没有指定 group= 则使用这个值
```
default_group DEFAULT_GROUP;
```
#### config_tenant （默认 空）
nacos config 功能所使用的 tenant. ;
```
config_tenant "";
```
#### service_namespace （默认 "public"）
nacos 服务功能所使用的 namespace.;
```
service_namespace "public";
```

#### cache_dir
nacos 的文件 缓存目录，下次启动 会优先从 这个目录读取数据，加快启动时间，否则从 http 地址拉取。保证该目录 nobody 用户可读写
```
cache_dir nacos_cache;
```


#### server_host （可省略，默认值：nacos）
nacos http 请求所使用的 host
```
server_host nacos;
```
#### key_zone_size （默认 4M）
共享内存大小，订阅 nacos 的所有数据都会缓存到共享内存。以便于 work 进程能够获取。订阅的配置多需要调大
```
key_zone_size 16M;
```
#### keys_hash_max_size （默认 128）
nacos service 的hash 表大小
```
keys_hash_max_size 128;
```
#### keys_bucket_size （默认 128）
nacos service 的 hash 表桶大小
```
keys_bucket_size 128;
```
#### config_keys_hash_max_size （默认 128）
nacos config 的hash 表大小
```
config_keys_hash_max_size 128;
```
#### config_keys_bucket_size （默认 128）
nacos config 的 hash 表桶大小
```
config_keys_bucket_size 128;
```
#### udp_pool_size （默认 8192）
连接的 pool 大小
```
udp_pool_size 8192;
```

#### local_ip （默认 扫描本机网卡获取）
用于 grpc 消息中的 local_ip 或 服务注册，(不用于物理连接)
```
local_ip 172.2.2.1;
```

#### register block 注册 nginx 为 nacos 服务
```nginx
nacos {
    server_list ip1:8848 ip2:8848 ip3:8848;
    grpc_server_list ip1:9848 ip2:9848 ip3:9848;
    // ...
    
    register nginx-server1 {
        port 80; # 端口
    }
    register nginx-server2 {
        port 8080; # 端口
    }
}

```
#### register.port 服务注册的端口


#### register.ip 服务注册的 ip
```
register nginx-server {
    port 80;
    ip 127.0.0.1;
}
```

#### register.weight 服务注册的权重
```
register nginx-server {
    weight 100; # nacos 上看到的权重是 weight / 100.0 
}
```

#### register.group 服务注册group
```
register nginx-server {
    // ...
    group DEFAULT_GROUP; # 默认使用 nacos.default_group
}
```
#### register.cluster 服务注册的cluster (默认 “DEFAULT”)
```
register nginx-server {
    // ...
    cluster default; # 默认 ”DEFAULT“
}
```
#### register.healthy 服务注册的健康状态  on/off (默认 “on”)
```
register nginx-server {
    // ...
    healthy on; # 默认 ”on“
}
```
#### register.metadata 服务注册 元数据
```
register nginx-server {
    // ...
    metadata from nginx;
    metadata idc test;
}
```

### nacos_subscribe_service
订阅 nacos 服务，这是 本项目核心指令。在 upstream 中，不需要配置 后端 ip 和 端口。
通过 nacos_subscribe_service 指定服务名，nginx 会订阅 nacos 中的服务，自动填充。
服务发布下线 也会 自动更新。
```
upstream backend {
    # 不需要写死 后端 ip。配置 nacos 服务名，nginx 自己订阅
    # server 127.0.0.1:8080; # 防止 nginx 启动时候报错
    # 如果provider使用的spring，service_name 要和 spring.application.name一致
    # 不知道 provider 端怎么写请参考 https://github.com/zhwaaaaaa/springmvc-nacos-registry
    # weight * nacos_weight 是 nginx 的权重，默认1 max_fails=1 fail_timeout 对应 nginx 的server 配置
    nacos_subscribe_service service_name=springmvc-nacos-demo group=DEFAULT_GROUP weight=1 max_fails=1 fail_timeout=10s;
}
```

### nacos_use_cluster
如果指定，则对应使用 cluster ip. 支持变量和字面量组合. 不指定，则使用所有集群 ip。
可以配置多个，优先级从前到后（比如 c1 c2: 选择 IP 的时候，如果 c1 集群没有，则使用 c2 集群。如果都没有，则请求 失败 返回 500）
配置了 此 cluster 之后，可以使用 内置 nginx 变量 $nacos_real_cluster 获取实际 使用的 cluster （用于日至，监控等）。
```
set $cluster "DEFAULT";

upstream backend {
    nacos_subscribe_service service_name=springmvc-nacos-demo group=DEFAULT_GROUP weight=100 max_fails=10 fail_timeout=10s;
    nacos_use_cluster $cluster1 $cluster2;
}

log_format "$host $uri ==> $nacos_real_cluster $upstream_addr";
```


### nacos_config_var
订阅 nacos 的配置，nginx把它写到 http 变量中。这个配置项可以出现在 http server location if {} 块中。
```
nacos_config_var $var_name md5_var=$md5_var_name data_id=xxxx group=xxx default=def_value;
```
通过 $var_name 获取到 配置的内容。$md5_var_name 可以获取到配置的 md5。
如果nacos中指定的 data_id 和 group 不存在，使用 默认值 def_value
nacos 变量功能让 nginx 的灵活性大大增强了。

### init_nacos_by_lua_file
通过 lua 脚本初始化 nacos 配置。在这个脚本中 可以 require "nacos" 动态订阅 nacos 服务和配置.
类似于 openresty 的 init_worker_by_lua 不支持 yield.
需要安装 [nacos_lua](modules/nacos_lua) 
```nginx
init_nacos_by_lua_file /path/to/init.lua;
```
- nacos.log(LEVEL, "xxxxx") print log
- nacos.listen_config(data_id, group, listener) listen nacos config change. listener is a function
- nacos.subscribe_service(service_name, group, listener) listen nacos service change, listener is a function
- 订阅到的内容可以通过 ngx.shared.XXX 跟worker 进程共享: 
```lua
local nacos = require "nacos"
local function conf_listener(data)
    -- data is a table like {data_id: "xxxx", group: "xxxx", content: "xxxx", md5: "xxxx"}
    print("data_id: " .. data.data_id .. " group: " .. data.group .. " content: " .. data.content)
end
local function service_listener(data)
    -- data is a table like :
    --  {service_name: "xxxx", group: "xxxx", "instances": [{ip: "10.10.10.10", port: 8080, weight: 100, cluster: "DEFAULT"}, 
    --  {ip: "10.10.10.11", port: 8080, weight: 100, cluster: "DEFAULT"}] }
end
local unlisten_conf = nacos.listen_config("gateway.server.route.json", "pro", conf_listener)
-- unlisten_conf() can unlisten the conf listener. unlisten_conf 函数 gc 也会取消订阅
-- unlisten_conf.notified is number of times that the conf listener notified.
-- unlisten_conf.data_id is the data_id of the conf, nlisten_conf.group is the group of the conf

local unlisten_service = nacos.subscribe_service("gateway.server.route.json", "test", service_listener)
-- unlisten_service() can unlisten the service listener, unlisten_service 函数 gc 也会取消订阅
-- unlisten_service.notified is number of times that the conf listener notified.
-- unlisten_service.service_name is the service_name of the service, unlisten_service.group is the group of the conf

nacos.log(ngx.INFO, "listen start ...")
```

# 开发计划
 * 通过 UDP 协议订阅 nacos 服务。（✅）
 * 通过 GRPC 协议订阅 nacos 服务。（✅）
 * nginx 通过 GRPC 协议订阅 nacos 配置。（✅）
 * 发布 1.0 版本，可以基本使用。（✅）
 * 删除 nginx 原有代码，对 nginx 原有代码的修改通过 patch 支持各个 nginx 版本。（✅）
 * 支持集成 openresty。（✅）

# License
- The project is licensed under the Apache License Version 2.0 except for yaij and pb,  Copyright (c) 2022-2024, Zhwaaaaaa
- code in module/nacos/yaij is from [yajl](https://github.com/lloyd/yajl) Licensed under the ISC License, Copyright (c) 2007-2014, Lloyd Hilaiel
- code in modules/nacos/pb  is from [nanopb](https://github.com/nanopb/nanopb) Licensed under the Zlib License, Copyright (c) 2011 Petteri Aimonen <jpa at nanopb.mail.kapsi.fi>

# 致谢
感谢 [JetBrains](https://www.jetbrains.com.cn) 公司赠送激活码，作者使用 [JetBrains Clion](https://www.jetbrains.com.cn/clion) 开发本项目过程中大大提升了开发效率。
