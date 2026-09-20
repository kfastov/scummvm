#!/bin/bash
# Собирает APK ScummVM с нашими правками под Android (arm64, Pixel 8 Pro).
#
# Собираем не из scummvm-src напрямую, а из его слепка build-android/src:
# в scummvm-src работа может идти параллельно (Башня знаний), а сборка под
# Android занимает минуты и не должна ловить чужие полуготовые правки.
# Слепок снимается заново при каждом запуске — свежие правки подхватятся.
#
#   ./6-build-apk.sh          — слепок, при необходимости зависимости, сборка
#   ./6-build-apk.sh deps     — только пересобрать зависимости
#   SKIP_SYNC=1 ./6-build-apk.sh  — собрать из имеющегося слепка, не пересинхронизируя
#
# На выходе: build-android/src/ScummVM-debug.apk (ставится ./5-android.sh).
#
# Что нужно из инструментов (ставится один раз):
#   brew install --cask android-commandlinetools android-platform-tools
#   sdkmanager "platform-tools" "platforms;android-37.0" "build-tools;37.0.0" \
#              "ndk;23.2.8568313"
# Версия NDK не произвольная: configure сверяет её с dists/android/build.gradle
# и отказывается работать при расхождении.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/build-android/src"

export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_SDK_ROOT/ndk/23.2.8568313}"
export JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home -v 21)}"

# Зависимости живут вне проекта, и не случайно: libtool спотыкается о пробел
# в пути «old games» — при установке он теряет всё до пробела и падает.
PREFIX="$HOME/.cache/scummvm-android-deps"

NDK_TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64"

step() { echo; echo "==> $*"; }

# --- зависимости --------------------------------------------------------------
# Готовых сборок под Android для них нет, а ScummVM без них не соберётся:
#   ogg+vorbis — движок nancy объявляет vorbis обязательной зависимостью,
#                без неё configure молча выкидывает движок из сборки;
#   oboe       — звуковой слой самого Android-порта, подключается как .so и
#                кладётся в APK рядом с libscummvm.so.
build_deps() {
	local OGG=1.3.5 VORBIS=1.3.7 OBOE=1.9.3
	mkdir -p "$PREFIX/src"
	cd "$PREFIX/src"

	export CC="$NDK_TC/bin/aarch64-linux-android21-clang"
	export CXX="$NDK_TC/bin/aarch64-linux-android21-clang++"
	export AR="$NDK_TC/bin/llvm-ar" RANLIB="$NDK_TC/bin/llvm-ranlib" STRIP="$NDK_TC/bin/llvm-strip"

	step "libogg $OGG"
	[ -f "libogg-$OGG.tar.gz" ] || curl -sSLO "https://downloads.xiph.org/releases/ogg/libogg-$OGG.tar.gz"
	rm -rf "libogg-$OGG" && tar xzf "libogg-$OGG.tar.gz"
	(cd "libogg-$OGG" && ./configure --host=aarch64-linux-android --prefix="$PREFIX" \
		--disable-shared --enable-static >/dev/null && make -j8 >/dev/null && make install >/dev/null)

	step "libvorbis $VORBIS"
	[ -f "libvorbis-$VORBIS.tar.gz" ] || curl -sSLO "https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS.tar.gz"
	rm -rf "libvorbis-$VORBIS" && tar xzf "libvorbis-$VORBIS.tar.gz"
	(cd "libvorbis-$VORBIS" && ./configure --host=aarch64-linux-android --prefix="$PREFIX" \
		--with-ogg="$PREFIX" --disable-shared --enable-static --disable-oggtest >/dev/null \
		&& make -j8 >/dev/null && make install >/dev/null)

	step "oboe $OBOE"
	rm -rf oboe oboe-build
	git clone --quiet --depth 1 --branch "$OBOE" https://github.com/google/oboe.git
	cmake -S oboe -B oboe-build -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
		-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON >/dev/null
	cmake --build oboe-build -j8 >/dev/null
	cp oboe-build/liboboe.so "$PREFIX/lib/"
	"$NDK_TC/bin/llvm-strip" --strip-unneeded "$PREFIX/lib/liboboe.so"
	rm -rf "$PREFIX/include/oboe" && cp -r oboe/include/oboe "$PREFIX/include/"

	unset CC CXX AR RANLIB STRIP
	echo "Зависимости в $PREFIX"
}

if [ "${1:-}" = "deps" ]; then build_deps; exit 0; fi
[ -f "$PREFIX/lib/libvorbis.a" ] && [ -f "$PREFIX/lib/liboboe.so" ] || build_deps

# --- слепок исходников --------------------------------------------------------
if [ "${SKIP_SYNC:-0}" != "1" ]; then
	step "Снимаю слепок scummvm-src → build-android/src"
	# Исключения только по корню (ведущий слеш): без него «--exclude=scummvm»
	# выкинул бы и каталог java-пакета backends/platform/android/org/scummvm/scummvm.
	rsync -a --delete \
		--exclude='/.git/' --exclude='*.o' --exclude='*.d' --exclude='.deps/' \
		--exclude='/scummvm' --exclude='*.a' \
		--exclude='/config.log' --exclude='/config.mk' --exclude='/config.h' \
		--exclude='/configure.stamp' --exclude='android_project/' --exclude='/libscummvm.so' \
		"$ROOT/scummvm-src/" "$SRC/"

	# Обёртка gradle качает свой дистрибутив (134 МБ) с таймаутом в 10 секунд и
	# без повторов — на медленном канале это гарантированный обрыв. Правим только
	# в слепке: в scummvm-src лезть незачем.
	sed -i '' -e 's/^networkTimeout=.*/networkTimeout=120000/' \
		-e 's/^retries=.*/retries=3/' \
		"$SRC/dists/android/gradle/wrapper/gradle-wrapper.properties"
fi

# --- сборка -------------------------------------------------------------------
cd "$SRC"

# Движки только те, что нужны двум играм: полная сборка под Android — это ещё
# полсотни движков и десятки минут. toolbook не включаем сознательно: он сейчас
# в работе в соседнем треде, и его состояние не должно ронять сборку телефона.
step "configure (director + nancy, arm64-v8a)"
CXXFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
	./configure --host=android-arm64-v8a \
	--disable-all-engines --enable-engine=director,nancy \
	--with-ogg-prefix="$PREFIX" --with-vorbis-prefix="$PREFIX" \
	--disable-debug --enable-optimizations

grep -q "^ENABLE_NANCY" config.mk || { echo "nancy не попал в сборку — проверьте vorbis"; exit 1; }

step "libscummvm.so"
make -j8 libscummvm.so

# Оболочка ScummVM ждёт liboboe.so рядом с собой в APK: configure подключает
# её как системную (-loboe), а собирать её никто, кроме нас, не будет.
mkdir -p android_project/lib/arm64-v8a
cp "$PREFIX/lib/liboboe.so" android_project/lib/arm64-v8a/

step "APK"
make ScummVM-debug.apk

echo
ls -lh "$SRC/ScummVM-debug.apk"
echo "Ставить: ./5-android.sh"
