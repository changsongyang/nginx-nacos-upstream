#!/bin/bash
set -e

rm -rf nginx
rm -rf objs

nginx_version=1.25.3
curl -sSL https://nginx.org/download/nginx-${nginx_version}.tar.gz -o nginx.tar.gz

tar zxvf nginx.tar.gz
mv nginx-${nginx_version} nginx
cd nginx
patch -p1 < ../patch/nginx.patch


./configure \
  --with-http_v2_module \
  --with-http_ssl_module \
  --add-module=../modules/auxiliary \
  --add-module=../modules/nacos \
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
  --http-scgi-temp-path=scgi_temp

cd ..

mkdir -p objs/logs
mkdir -p objs/nacos
rm -f nginx.tar.gz