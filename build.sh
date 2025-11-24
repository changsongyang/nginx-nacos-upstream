#!/bin/bash
set -e

rm -rf openresty*
rm -rf nginx*

nginx_version=1.27.1
#curl -sSL https://nginx.org/download/nginx-${nginx_version}.tar.gz -o nginx-${nginx_version}.tar.gz
#tar zxvf nginx-${nginx_version}.tar.gz
#mv nginx-${nginx_version} nginx

openresty_version=${nginx_version}.2
curl -sSL https://openresty.org/download/openresty-${openresty_version}.tar.gz -o openresty-${openresty_version}.tar.gz

tar zxvf openresty-${openresty_version}.tar.gz
mv openresty-${openresty_version} openresty

mv openresty/bundle/LuaJIT-2.1-20250117 openresty/LuaJIT
mv openresty/bundle/lua-cjson-2.1.0.14 openresty/lua-cjson
mv openresty/bundle/ngx_lua-0.10.28 openresty/ngx_lua
mv openresty/bundle/ngx_lua_upstream-0.07 openresty/ngx_lua_upstream
cp -rf openresty/bundle/lua-resty-core-0.1.31/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-signal-0.04/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-string-0.16/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-shell-0.03/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-upload-0.11/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-websocket-0.12/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-tablepool-0.03/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-lock-0.09/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-limit-traffic-0.09/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-dns-0.23/lib/* cmake-build-debug
cp -rf openresty/bundle/lua-resty-lrucache-0.15/lib/* cmake-build-debug

make -C openresty/LuaJIT -j8
mv openresty/LuaJIT/src/libluajit.a openresty/LuaJIT/src/libluajit-5.1.a
export LUAJIT_LIB=../openresty/LuaJIT/src
export LUAJIT_INC=../openresty/LuaJIT/src

mv openresty/bundle/nginx-${nginx_version} nginx
cd nginx
patch -p1 < ../patch/openresty.patch

./configure \
  --with-http_v2_module \
  --with-http_ssl_module \
  --add-module=../modules/auxiliary \
  --add-module=../modules/nacos \
  --add-module=../openresty/ngx_lua \
  --add-module=../modules/nacos_lua \
  --add-module=../openresty/ngx_lua_upstream \
  --prefix=../objs \
  --conf-path=../conf/my.conf \
  --error-log-path=logs/error.log \
  --pid-path=logs/nginx.pid \
  --lock-path=logs/nginx.lock \
  --http-log-path=logs/access.log \
  --http-client-body-temp-path=client_body_temp \
  --http-proxy-temp-path=proxy_temp \
  --http-fastcgi-temp-path=fastcgi_temp \
  --http-uwsgi-temp-path=uwsgi_temp \
  --http-scgi-temp-path=scgi_temp \
  --with-debug

cd ..

mkdir -p objs/logs
mkdir -p objs/nacos
rm -f openresty-${openresty_version}.tar.gz