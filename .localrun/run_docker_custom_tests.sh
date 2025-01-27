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
quiet_=1
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

redirect_cmd apt-get update $qqq

redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends lsb-release
system__=$(lsb_release -i|cut -d ':' -f2|sed -e "s#\s##g")
version__=$(lsb_release -r|cut -d ':' -f2|sed -e "s#\s##g")
echo "compiling on: $system__ $version__"

echo "installing more system packages ..."

# ---- TZ ----
export DEBIAN_FRONTEND=noninteractive
ln -fs /usr/share/zoneinfo/America/New_York /etc/localtime
redirect_cmd apt-get install $qqq -y --force-yes tzdata
redirect_cmd dpkg-reconfigure --frontend noninteractive tzdata
# ---- TZ ----

pkgs="
    ca-certificates
    rsync
    nano
    clang
    cmake
    git
    libconfig-dev
    libgtest-dev
    libopus-dev
    libsodium-dev
    libvpx-dev
    ninja-build
    pkg-config
"

for i in $pkgs ; do
    redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends $i
done

pkgs_z="
    binutils
    llvm-dev
    libavutil-dev
    libavcodec-dev
    libavformat-dev
    libavfilter-dev
    libx264-dev
"

for i in $pkgs_z ; do
    redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends $i
done


echo ""
echo ""
echo "--------------------------------"
echo "clang version:"
c++ --version
echo "--------------------------------"
echo ""
echo ""

cd /c-toxcore

rm -Rf /workspace/_build/
rm -Rf /workspace/auto_tests/
rm -Rf /workspace/cmake/
rm -f  /workspace/CMakeLists.txt
rm -Rfv /workspace/custom_tests/

echo "make a local copy ..."
redirect_cmd rsync -avz --exclude=".localrun" ./ /workspace/

cd /workspace/

CC=clang cmake -B_build -H. -GNinja \
    -DCMAKE_INSTALL_PREFIX:PATH="$PWD/_install" \
    -DCMAKE_C_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_CXX_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_EXE_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_SHARED_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DMIN_LOGGER_LEVEL=INFO \
    -DMUST_BUILD_TOXAV=ON \
    -DNON_HERMETIC_TESTS=OFF \
    -DSTRICT_ABI=OFF \
    -DUSE_IPV6=OFF \
    -DAUTOTEST=OFF \
    -DBUILD_MISC_TESTS=OFF \
    -DBUILD_FUN_UTILS=OFF
cd _build
ninja install -j"$(nproc)"

cd /workspace/
pwd
ls -1 ./custom_tests/*.c
export PKG_CONFIG_PATH="$PWD"/_install/lib/pkgconfig
export LD_LIBRARY_PATH="$PWD"/_install/lib
for i in $(ls -1 ./custom_tests/*.c) ; do
    echo "CCC:--------------- ""$i"" ---------------"
    rm -f test
    clang -g -O1 -fno-omit-frame-pointer -fsanitize=address \
    -Wno-everything -Wno-missing-variable-declarations \
    $(pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium) \
    $(pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium) \
    "$i" \
    -o test
    echo "RUN:--------------- ""$i"" ---------------"
    ./test
    if [ $? -ne 0 ]; then
        echo "ERR:--------------- ""$i"" ---------------"
        exit $?
    else
        echo "OK :*************** ""$i"" ***************"
    fi
done

mkdir -p /artefacts/custom_tests/
chmod a+rwx -R /workspace/
chmod a+rwx -R /artefacts/

' > $_HOME_/script/do_it___external.sh

chmod a+rx $_HOME_/script/do_it___external.sh


system_to_build_for="ubuntu:20.04"

cd $_HOME_/
docker run -ti --rm \
  -v $_HOME_/artefacts:/artefacts \
  -v $_HOME_/script:/script \
  -v $_HOME_/../:/c-toxcore \
  -v $_HOME_/workspace:/workspace \
  -e DISPLAY=$DISPLAY \
  "$system_to_build_for" \
  /bin/sh -c "apk add bash >/dev/null 2>/dev/null; /bin/bash /script/do_it___external.sh"

