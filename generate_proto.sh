#!/bin/bash
set -e
mkdir -p objs
cd objs
curl -sSL https://jpa.kapsi.fi/nanopb/download/nanopb-0.4.9-linux-x86.tar.gz -o nanopb.tar.gz

tar zxvf nanopb.tar.gz
mv nanopb-0.4.9-linux-x86 nanopb

python3 nanopb/generator/nanopb_generator.py backup.proto nacos_grpc_service.proto -I ../modules/nacos
