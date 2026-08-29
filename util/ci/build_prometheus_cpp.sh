#!/bin/bash -eu
cd /tmp
git clone --recursive --depth 1 --shallow-submodules \
	https://github.com/jupp0r/prometheus-cpp
mkdir prometheus-cpp/build
cd prometheus-cpp/build
# civetweb must be installed as a system package before this configure
# step. Upstream prometheus-cpp used to vendor civetweb as a submodule
# (3rdparty/civetweb) with a USE_THIRDPARTY_LIBRARIES option to build
# from it instead of searching the system -- at some point that was
# removed entirely upstream (no more 3rdparty/ directory, no more that
# option), so pull/CMakeLists.txt now unconditionally does
# find_package(civetweb) against a real system install. --recursive
# above no longer provides it. Confirmed by actually building this
# end-to-end in a sandbox: apt's libcivetweb-dev alone isn't enough
# either -- its civetweb-targets.cmake references the "civetweb" binary
# package too (civetweb::server points at /usr/bin/civetweb), so both
# packages are needed, which is why install_linux_deps() (util/ci/
# common.sh) installs both before this script runs.
cmake .. \
	-DCMAKE_INSTALL_PREFIX=/usr/local \
	-DCMAKE_BUILD_TYPE=Release \
	-DENABLE_TESTING=0
make -j$(nproc)
sudo make install

