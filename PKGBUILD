# Maintainer: grm <grm@eyesin.space>
pkgbase=wl-tools
pkgname=(wlnch wlnch-in wlnch-out)
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

package_wlnch-out() {
  pkgdesc="Wayland output tool"
  replaces=(wout)
  conflicts=(wout)
  install -Dm755 "$startdir/wlnch-out" "$pkgdir/usr/bin/wlnch-out"
}

package_wlnch-in() {
  pkgdesc="Wayland input tool"
  replaces=(wnpt)
  conflicts=(wnpt)
  install -Dm755 "$startdir/wlnch-in" "$pkgdir/usr/bin/wlnch-in"
}
