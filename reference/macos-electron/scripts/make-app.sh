#!/bin/bash
# Собирает двойной-кликабельный .app (на базе Electron.app из node_modules),
# кладёт ярлык на рабочий стол. Запуск: bash scripts/make-app.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
APP_NAME="Снег"
APP="$ROOT/dist/$APP_NAME.app"
BUNDLE_ID="com.dmytro.desktopweather"

echo "==> сборка JS"
npm run build >/dev/null

echo "==> иконки"
mkdir -p build dist/assets
swiftc -O scripts/icon.swift -o build/icongen
./build/icongen build/icon_1024.png 1024 >/dev/null
mkdir -p build/AppIcon.iconset
for s in 16 32 128 256 512; do
    sips -z "$s" "$s" build/icon_1024.png --out "build/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z "$d" "$d" build/icon_1024.png --out "build/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
cp build/icon_1024.png build/AppIcon.iconset/icon_512x512@2x.png
iconutil -c icns build/AppIcon.iconset -o build/AppIcon.icns
./build/icongen dist/assets/trayTemplate.png 14 tray >/dev/null
./build/icongen dist/assets/trayTemplate@2x.png 28 tray >/dev/null

echo "==> сборка .app"
rm -rf "$APP"
cp -R node_modules/electron/dist/Electron.app "$APP"
mv "$APP/Contents/MacOS/Electron" "$APP/Contents/MacOS/$APP_NAME"
rm -f "$APP/Contents/Resources/default_app.asar"

PB=/usr/libexec/PlistBuddy
PLIST="$APP/Contents/Info.plist"
$PB -c "Set :CFBundleExecutable $APP_NAME" "$PLIST"
$PB -c "Set :CFBundleName $APP_NAME" "$PLIST"
$PB -c "Set :CFBundleDisplayName $APP_NAME" "$PLIST"
$PB -c "Set :CFBundleIdentifier $BUNDLE_ID" "$PLIST"
$PB -c "Set :CFBundleIconFile AppIcon" "$PLIST"
$PB -c "Set :LSUIElement true" "$PLIST" 2>/dev/null || $PB -c "Add :LSUIElement bool true" "$PLIST"

cp build/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

# сама программа (package.json + dist) внутрь бандла
mkdir -p "$APP/Contents/Resources/app"
cp package.json "$APP/Contents/Resources/app/package.json"
rm -rf "$APP/Contents/Resources/app/dist"
mkdir -p "$APP/Contents/Resources/app/dist"
for item in main.js preload.js settings.html settings.js renderer effects assets bin; do
    cp -R "dist/$item" "$APP/Contents/Resources/app/dist/$item"
done

echo "==> подпись (ad-hoc)"
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "    (подпись пропущена)"

echo "==> ярлык на рабочем столе"
DESKTOP="$HOME/Desktop"
# чистим прежние ярлыки и псевдонимы: при каждой пересборке Finder создавал новый
# «Снег псевдонім N», и на рабочем столе копился десяток файлов
rm -rf "$DESKTOP/$APP_NAME" "$DESKTOP/$APP_NAME.app" "$DESKTOP/$APP_NAME.app alias"
rm -rf "$DESKTOP/$APP_NAME псевдонім"*
# один детерминированный ярлык — символическая ссылка на актуальную сборку
ln -sfn "$APP" "$DESKTOP/$APP_NAME.app"
ALIAS="$DESKTOP/$APP_NAME.app"

echo "==> уборка артефактов сборки"
# иконсет и пробники нужны только на время сборки — они уже внутри бандла
rm -rf build

echo "OK: $APP"
echo "ярлык: $ALIAS"
