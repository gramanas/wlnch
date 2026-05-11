# Maintainer: grm <grm@eyesin.space>
pkgbase=wl-tools
pkgname=(wlnch wout wnpt)
pkgver=dev
pkgrel=1
pkgdesc="Wayland tools"
arch=(x86_64)
license=(MIT)
depends=(wayland libxkbcommon freetype2 fontconfig)
makedepends=(base-devel wayland libxkbcommon freetype2 fontconfig pkg-config)

build() {
  make -C "$startdir"
}

package_wlnch() {
  pkgdesc="Wayland launcher"
  install -Dm755 "$startdir/wlnch" "$pkgdir/usr/bin/wlnch"
}

package_wout() {
  pkgdesc="Wayland output tool"
  install -Dm755 "$startdir/wout" "$pkgdir/usr/bin/wout"
}

package_wnpt() {
  pkgdesc="Wayland input tool"
  install -Dm755 "$startdir/wnpt" "$pkgdir/usr/bin/wnpt"
}
