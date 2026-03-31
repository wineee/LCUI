target("hello")
    add_installfiles("app/*")
    set_kind("binary")
    set_rundir("app")
    add_files("src/*.c")
    local pkgroot = path.join(os.projectdir(), "..", "build", "packages")
    local mode = is_mode("release") and "release" or "debug"
    local arch = "x86_64"
    local plat = "linux"
    add_includedirs(
        path.join(pkgroot, "l", "lcui", plat, arch, mode, "include"),
        path.join(pkgroot, "y", "yutil", plat, arch, mode, "include"),
        path.join(pkgroot, "p", "pandagl", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libcss", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libui", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libui-cursor", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libui-server", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libui-xml", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libthread", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libptk", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libworker", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "librouter", plat, arch, mode, "include"),
        path.join(pkgroot, "l", "libi18n", plat, arch, mode, "include")
    )
    add_linkdirs(
        path.join(pkgroot, "l", "lcui", plat, arch, mode, "lib"),
        path.join(pkgroot, "y", "yutil", plat, arch, mode, "lib"),
        path.join(pkgroot, "p", "pandagl", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libcss", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libui", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libui-cursor", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libui-server", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libui-xml", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libthread", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libptk", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libworker", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "librouter", plat, arch, mode, "lib"),
        path.join(pkgroot, "l", "libi18n", plat, arch, mode, "lib")
    )
    add_links(
        "lcui",
        "libui-xml",
        "librouter",
        "libui-server",
        "libui-cursor",
        "libui",
        "libi18n",
        "libcss",
        "libworker",
        "libptk",
        "pandagl",
        "libthread",
        "yutil"
    )
    add_syslinks("fontconfig", "freetype", "png", "jpeg", "X11", "pthread", "dl", "m", "wayland-client", "xml2")
