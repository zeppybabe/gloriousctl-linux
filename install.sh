#!/usr/bin/env bash
#
# gloriousctl installer.
#
#   ./install.sh            build, install, set up udev, verify
#   ./install.sh --purge    remove binaries, udev rules and cached state
#   ./install.sh --verify   check permissions only, change nothing
#
# Run as your normal user. The script calls sudo for the privileged steps so
# it can still tell who you are -- running the whole thing under sudo makes
# $HOME point at /root and the cache lands in the wrong place.

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
# 60-, not 99-: the uaccess tag has to be set before systemd's
# 73-seat-late.rules runs the uaccess builtin, and udev reads rule files in
# lexical order. A 99- file tags the device and never gets an ACL.
RULES_NAME="60-glorious.rules"
RULES_PATH="/etc/udev/rules.d/${RULES_NAME}"
RULES_PATH_LEGACY="/etc/udev/rules.d/99-glorious.rules"
CACHE_NAME=".gloriousctl_state.bin"

RED=$'\e[31m'; GRN=$'\e[32m'; YLW=$'\e[33m'; RST=$'\e[0m'
ok()   { printf '%s  ok%s  %s\n'   "$GRN" "$RST" "$1"; }
warn() { printf '%swarn%s  %s\n'   "$YLW" "$RST" "$1"; }
err()  { printf '%sfail%s  %s\n'   "$RED" "$RST" "$1" >&2; }
step() { printf '\n== %s\n' "$1"; }

if [[ ${EUID} -eq 0 && -z ${SUDO_USER:-} ]]; then
    err "Run this as your normal user, not as root."
    exit 1
fi
REAL_USER="${SUDO_USER:-$USER}"
REAL_HOME="$(getent passwd "$REAL_USER" | cut -d: -f6)"

# ---------------------------------------------------------------- purge ----

purge() {
    step "Removing installed binaries"
    # Every gloriousctl on PATH, not just the one that would run.
    local found=0 p
    while IFS= read -r p; do
        [[ -n $p ]] || continue
        found=1
        echo "  removing $p"
        sudo rm -f "$p"
    done < <(type -a -P gloriousctl 2>/dev/null | sort -u)
    sudo rm -f "${PREFIX}/bin/gloriousctl"
    sudo rm -f "${PREFIX}/bin/gloriousctl-gui" "${PREFIX}/share/applications/gloriousctl.desktop"
    [[ $found -eq 1 ]] || ok "nothing on PATH"

    step "Removing udev rules"
    sudo rm -f "$RULES_PATH" "$RULES_PATH_LEGACY"
    sudo udevadm control --reload || true

    step "Removing cached state"
    # Both locations: the sudo-era cache and the user one.
    sudo rm -f "/root/${CACHE_NAME}"
    rm -f "${REAL_HOME}/${CACHE_NAME}"
    ok "caches cleared"

    step "Done"
    echo "Replug the mouse to drop the old permissions."
}

# --------------------------------------------------------------- verify ----

# Print the hidraw node of the first connected supported device, if any.
find_hidraw() {
    local bin="$1" tag="${2:-yes}" line
    "$bin" --probe 2>/dev/null | while IFS= read -r line; do
        # supported=yes is emitted only for nodes matching supported_devices[];
        # without it any vendor-page device that enumerated could be picked.
        if [[ $tag == yes ]]; then
            [[ $line =~ path=(/dev/hidraw[0-9]+)[[:space:]]+supported=yes ]] \
                && printf '%s\n' "${BASH_REMATCH[1]}"
        else
            [[ $line =~ path=(/dev/hidraw[0-9]+) ]] \
                && printf '%s\n' "${BASH_REMATCH[1]}"
        fi
    done | sort -u | head -n1
}

verify() {
    local bin="${PREFIX}/bin/gloriousctl"
    if [[ ! -x $bin ]]; then
        err "not installed at $bin"
        return 1
    fi
    ok "binary at $bin"

    if [[ -f $RULES_PATH ]]; then
        ok "udev rules at $RULES_PATH"
    else
        err "udev rules missing at $RULES_PATH"
        return 1
    fi

    # More than one gloriousctl on PATH is how you end up testing a stale build.
    local n
    n="$(type -a -P gloriousctl 2>/dev/null | sort -u | wc -l)"
    if [[ $n -gt 1 ]]; then
        warn "multiple gloriousctl on PATH:"
        type -a -P gloriousctl | sort -u | sed 's/^/        /'
        warn "run './install.sh --purge' then reinstall"
    fi

    local node
    node="$(find_hidraw "$bin" yes || true)"
    # An installed binary older than this script does not tag its --probe
    # output; fall back rather than claim no device is connected.
    if [[ -z $node ]]; then
        node="$(find_hidraw "$bin" no || true)"
        [[ -n $node ]] && warn "installed binary predates the tagged --probe output; reinstall to make this check exact"
    fi
    if [[ -z $node ]]; then
        warn "no supported device connected; cannot check permissions"
        return 0
    fi
    ok "device node $node"

    # uaccess grants access through a POSIX ACL, not the file mode.
    if getfacl -p "$node" 2>/dev/null | grep -q "^user:${REAL_USER}:rw-"; then
        ok "$REAL_USER has ACL access -- sudo is not required"
        return 0
    fi
    if [[ -r $node && -w $node ]]; then
        ok "$node is readable and writable"
        return 0
    fi

    err "$REAL_USER cannot access $node"
    echo
    # Tagged but no ACL is a specific, separable failure: the tag was set after
    # 73-seat-late.rules had already run the uaccess builtin. That is a rule
    # file naming problem, not a seat or replug problem.
    if udevadm info --query=property --name="$node" 2>/dev/null | grep -q "CURRENT_TAGS=.*uaccess"; then
        warn "device IS tagged uaccess but carries no ACL"
        local late
        late="$(find /etc/udev/rules.d /usr/lib/udev/rules.d -name '*glorious*.rules' 2>/dev/null \
                | grep -vE '/[0-6][0-9]-' || true)"
        if [[ -n $late ]]; then
            err "rules file sorts after 73-seat-late.rules, so the tag is set too late:"
            printf '%s\n' "$late" | sed 's/^/        /'
            echo "  Remove it and reinstall: ./install.sh --purge && ./install.sh"
            return 1
        fi
    fi
    echo "  The rules are installed but have not been applied to this device."
    echo "  Unplug the mouse, plug it back in, then run:"
    echo "      ./install.sh --verify"
    echo
    echo "  If it still fails after a replug, check that you are on a local"
    echo "  seat (uaccess does not apply over plain SSH):  loginctl show-session"
    return 1
}

# ---------------------------------------------------------- pygobject ----

have_pygobject() {
    python3 -c 'import gi; gi.require_version("Gtk","3.0"); from gi.repository import Gtk' 2>/dev/null
}

# The GUI is a Python script that needs the GTK3 introspection bindings. Most
# desktop installs already have them; when they are missing, offer the one
# package set the distribution uses. Declining just leaves the GUI unusable --
# the CLI does not depend on any of this.
ensure_pygobject() {
    if have_pygobject; then
        ok "already installed"
        return 0
    fi
    local cmd=""
    if   command -v apt-get >/dev/null 2>&1; then cmd="sudo apt-get install -y python3-gi gir1.2-gtk-3.0"
    elif command -v dnf     >/dev/null 2>&1; then cmd="sudo dnf install -y python3-gobject gtk3"
    elif command -v pacman  >/dev/null 2>&1; then cmd="sudo pacman -S --needed --noconfirm python-gobject gtk3"
    elif command -v zypper  >/dev/null 2>&1; then cmd="sudo zypper install -y python3-gobject typelib-1_0-Gtk-3_0"
    elif command -v apk     >/dev/null 2>&1; then cmd="sudo apk add py3-gobject3 gtk+3.0"
    fi
    if [[ -z $cmd ]]; then
        warn "unknown package manager; install PyGObject + GTK3 yourself for the GUI"
        return 0
    fi
    echo "  missing. The GUI needs it; the CLI does not."
    read -r -p "  run: ${cmd} ? [y/N] " reply
    if [[ ${reply,,} == y ]]; then
        if ${cmd} && have_pygobject; then
            ok "installed"
        else
            warn "install failed or bindings still not importable; GUI will not start"
        fi
    else
        warn "skipped -- gloriousctl-gui will not start until it is installed"
    fi
}

# -------------------------------------------------------------- install ----

install_all() {
    step "Checking for stale copies"
    local stale
    stale="$(type -a -P gloriousctl 2>/dev/null | sort -u | grep -v "^${PREFIX}/bin/gloriousctl$" || true)"
    if [[ -n $stale ]]; then
        warn "these will shadow or conflict with the new build:"
        printf '%s\n' "$stale" | sed 's/^/        /'
        read -r -p "  remove them? [y/N] " reply
        if [[ ${reply,,} == y ]]; then
            printf '%s\n' "$stale" | while IFS= read -r p; do sudo rm -f "$p"; done
            ok "removed"
        else
            warn "keeping them -- 'gloriousctl' may not run the build you just made"
        fi
    else
        ok "none"
    fi

    step "GUI dependency (PyGObject / GTK3)"
    ensure_pygobject

    step "Building"
    make
    ok "built"

    step "Clearing incompatible cached state"
    # The cache is versioned; a stale one is ignored at runtime but removing it
    # keeps the first run quiet, and clears anything root wrote earlier.
    sudo rm -f "/root/${CACHE_NAME}"
    rm -f "${REAL_HOME}/${CACHE_NAME}"
    ok "cleared"

    step "Installing"
    sudo make install PREFIX="$PREFIX"
    ok "installed"

    step "Verifying"
    if verify; then
        step "Ready"
        echo "Try:  gloriousctl --info"
        if python3 -c 'import gi; gi.require_version("Gtk","3.0"); from gi.repository import Gtk' 2>/dev/null; then
            ok "GUI available: gloriousctl-gui (also in your application menu)"
        else
            warn "GUI needs PyGObject/GTK3: python3-gi gir1.2-gtk-3.0 (Debian/Ubuntu), python3-gobject (Fedora), python-gobject (Arch)"
        fi
    else
        exit 1
    fi
}

case "${1:-}" in
    --purge)  purge ;;
    --verify) verify ;;
    "")       install_all ;;
    *)        echo "usage: $0 [--purge|--verify]" >&2; exit 2 ;;
esac
