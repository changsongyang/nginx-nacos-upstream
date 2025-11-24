#!/bin/bash
set -e

rm -rf openresty*
rm -rf nginx*
rm -rf objs

nginx_version=1.27.1

openresty_version=${nginx_version}.2
curl -sSL https://openresty.org/download/openresty-${openresty_version}.tar.gz -o openresty-${openresty_version}.tar.gz

tar zxvf openresty-${openresty_version}.tar.gz
mv openresty-${openresty_version} openresty

cd openresty/bundle/nginx-${nginx_version}

patch -p1 < ../../../patch/openresty.patch
cd ../..

./configure \
  --with-http_v2_module \
  --with-http_ssl_module \
  --add-module=../modules/auxiliary \
  --add-module=../modules/nacos \
  --add-module=../modules/nacos_lua \
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

make -j8 && make install
mkdir -p ../objs/logs
mkdir -p ../objs/nacos

cd ..
rm -f openresty-${openresty_version}.tar.gz