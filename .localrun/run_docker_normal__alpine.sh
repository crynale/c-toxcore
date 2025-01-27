#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

# docker info

mkdir -p $_HOME_/artefacts
mkdir -p $_HOME_/script
mkdir -p $_HOME_/workspace

echo '#! /bin/bash

## ----------------------
numcpus_=$(nproc)
quiet_=0
## ----------------------

echo "hello"

export qqq=""

if [ "$quiet_""x" == "1x" ]; then
	export qqq=" -qq "
fi


redirect_cmd() {
    if [ "$quiet_""x" == "1x" ]; then
        "$@" > /dev/null 2>&1
    else
        "$@"
    fi
}


echo "installing system packages ..."

redirect_cmd apk update

system__=alpine
version__=$(cat /etc/alpine-release)
echo "compiling on: $system__ $version__"

echo "installing more system packages ..."

# ---- TZ ----
export DEBIAN_FRONTEND=noninteractive
ln -fs /usr/share/zoneinfo/America/New_York /etc/localtime
# ---- TZ ----

pkgs="
    wget
    git
    cmake
    automake
    autoconf
    check
    libtool
    rsync
    nano
    gcc
    g++
    clang
    clang-extra-tools
    clang-analyzer
    libconfig-dev
    libsodium-dev
    opus-dev
    libvpx-dev
    x264-dev
    ffmpeg4-dev
    ninja
    pkgconf-dev
    make
    yasm
    file
    linux-headers
"

for i in $pkgs ; do
    redirect_cmd apk add $i
done

pkgs_z="
    binutils
    gtest-dev
    llvm-dev
"

for i in $pkgs_z ; do
    redirect_cmd apk add $i
done

rm -f /usr/bin/c++
rm -f /usr/bin/g++
cd /usr/bin/
ln -f /usr/bin/clang++ c++
ln -f /usr/bin/clang++ g++

echo ""
echo ""
echo "--------------------------------"
echo "clang version:"
c++ --version
echo "cmake version:"
cmake --version
echo "--------------------------------"
echo ""
echo ""

cd /c-toxcore

rm -Rf /workspace/_build/
rm -Rf /workspace/auto_tests/
rm -Rf /workspace/cmake/
rm -f  /workspace/CMakeLists.txt

echo "make a local copy ..."
redirect_cmd rsync -avz --exclude=".localrun" ./ /workspace/

cd /workspace/

CC=clang .circleci/cmake-normal

mkdir -p /artefacts/normal/
chmod a+rwx -R /workspace/
chmod a+rwx -R /artefacts/

cp /workspace/_build/Testing/Temporary/* /artefacts/normal/
cp /workspace/_build/unit_* /workspace/_build/auto_* /artefacts/normal/

' > $_HOME_/script/do_it___external.sh

chmod a+rx $_HOME_/script/do_it___external.sh


system_to_build_for="alpine"

cd $_HOME_/
docker run -ti --rm \
  -v $_HOME_/artefacts:/artefacts \
  -v $_HOME_/script:/script \
  -v $_HOME_/../:/c-toxcore \
  -v $_HOME_/workspace:/workspace \
  -e DISPLAY=$DISPLAY \
  "$system_to_build_for" \
  /bin/sh -c "apk add bash >/dev/null 2>/dev/null; /bin/bash /script/do_it___external.sh"

# bash /script/do_it___external.sh 2>&1 |grep -C3 -A3 'error: '
