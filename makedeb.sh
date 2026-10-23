#!/bin/sh

make clean
dpkg-buildpackage -us -uc -b

# Собирает deb-пакет и кладёт его в директорию уровнем выше.
# dpkg-deb -I: зависимости резолвятся автоматически (libc6, libqt6core6/gui6/multimedia6/sql6/widgets6, libsndfile1, libstdc++6, libtag1v5 + вручную добавленные ffmpeg, alsa-utils, libqt6sql6-sqlite).
# dpkg-deb -c: /usr/bin/audioscout, /usr/share/applications/audioscout.desktop, changelog.Debian.gz, copyright — всё на месте #(install() в CMakeLists работает).
# Установка dpkg -i + smoke-тест установленного бинарника (QT_QPA_PLATFORM=offscreen) — приложение запускается; затем пакет снят.
# lintian audioscout_1.0.0-1_amd64.deb - формальная проверка policy
