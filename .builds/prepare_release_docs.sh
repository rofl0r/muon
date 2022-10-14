#!/bin/sh

set -eux

build=$1
cd "$build/doc"

mkdir man
cp meson.build.5 muon.1 man
tar cvf man.tar man/*
gzip man.tar

mkdir docs
mv website/status.css website/status.html man.tar.gz docs
