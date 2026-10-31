#!/usr/bin/env sh
# AgentXray Linux 安装脚本（无需编译）：下载预编译包 → 校验 sha256 → 装到 ~/.local/bin
#
#   curl -fsSL https://raw.githubusercontent.com/loadwww/agent-xray/master/scripts/install.sh | sh
#
# 可用环境变量：
#   AXR_INSTALL_DIR  安装目录（默认 ~/.local/bin）
#   AXR_VERSION      指定版本（如 v0.4.2），默认 latest
#   AXR_BASE_URL     自定义下载源（内网镜像 / 自测用），默认 GitHub release 地址
set -eu

REPO="loadwww/agent-xray"
BIN_DIR="${AXR_INSTALL_DIR:-$HOME/.local/bin}"

say() { printf '%s\n' "$*"; }
fail() {
    printf '错误: %s\n' "$1" >&2
    printf '手动下载: https://github.com/%s/releases/latest\n' "$REPO" >&2
    printf '从源码构建: https://github.com/%s#构建linuxmacos\n' "$REPO" >&2
    exit 1
}

need() { command -v "$1" >/dev/null 2>&1 || fail "缺少依赖: $1"; }
need uname

os=$(uname -s)
arch=$(uname -m)
case "$os" in
    Linux) os=linux ;;
    Darwin) fail "macOS 暂未提供预编译包（Homebrew formula 在计划中）" ;;
    *) fail "不支持的系统: $os" ;;
esac
case "$arch" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=aarch64 ;;
    *) fail "不支持的架构: $arch" ;;
esac
if [ "$arch" != "x86_64" ]; then
    fail "$os-$arch 暂未提供预编译包（当前仅 linux-x86_64）"
fi

need tar
tmp=$(mktemp -d "${TMPDIR:-/tmp}/axr-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

asset="axr-$os-$arch.tar.gz"
if [ -n "${AXR_BASE_URL:-}" ]; then
    base="$AXR_BASE_URL"
elif [ -n "${AXR_VERSION:-}" ]; then
    base="https://github.com/$REPO/releases/download/$AXR_VERSION"
else
    # 资产名不带版本号，所以 latest/download 可以直接用，不经过 api.github.com
    base="https://github.com/$REPO/releases/latest/download"
fi

download() { # $1=url $2=dest
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 2 --retry-delay 1 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        fail "需要 curl 或 wget"
    fi
}

sha256_of() { # $1=file
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        fail "缺少 sha256 校验工具（sha256sum / shasum / openssl）"
    fi
}

say "[install] 下载 $asset"
download "$base/$asset" "$tmp/$asset" || fail "下载失败：$base/$asset（网络不可达，或该版本还没有 Linux 资产）"
download "$base/SHA256SUMS" "$tmp/SHA256SUMS" || fail "下载校验和失败：$base/SHA256SUMS"

expected=$(awk -v a="$asset" '$2 == a {print $1}' "$tmp/SHA256SUMS" | head -n 1)
[ -n "$expected" ] || fail "SHA256SUMS 中没有 $asset（该版本可能未发布 Linux 资产）"
actual=$(sha256_of "$tmp/$asset")
[ "$actual" = "$expected" ] || fail "sha256 校验失败：期望 $expected，实际 $actual"

tar -xzf "$tmp/$asset" -C "$tmp" axr || fail "解压失败（包结构异常）"
mkdir -p "$BIN_DIR" || fail "无法创建安装目录: $BIN_DIR"
cp "$tmp/axr" "$BIN_DIR/axr" || fail "无法写入: $BIN_DIR/axr"
chmod 755 "$BIN_DIR/axr"

if ! "$BIN_DIR/axr" version >/dev/null 2>&1; then
    say ""
    say "提示: $BIN_DIR/axr 无法运行，通常是缺少 libcurl："
    say "  Debian/Ubuntu: sudo apt-get install -y libcurl4"
    say "  Fedora/RHEL:   sudo dnf install -y libcurl"
    fail "安装完成但无法执行"
fi

say "[install] 已安装: $BIN_DIR/axr ($("$BIN_DIR/axr" version | head -n 1))"
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        say ""
        say "$BIN_DIR 不在 PATH 中，把下面一行加到 ~/.bashrc / ~/.zshrc："
        say "  export PATH=\"$BIN_DIR:\$PATH\""
        ;;
esac

say ""
say "试试内置演示（离线，不需要 API key）："
say "  axr demo"
