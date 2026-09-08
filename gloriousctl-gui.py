#!/usr/bin/env python3
"""gloriousctl GUI -- a GTK3 front end for the gloriousctl command-line tool.

Everything this window does is a plain `gloriousctl --set-...` command. The
CLI stays the single implementation of the protocol; this file only builds
argument lists, runs them, and shows the output. State is read back with
`gloriousctl --state` (a cache read -- the Pixart mice are write-only, so what
is shown is what the tool last sent, not what the mouse reports).

Requires PyGObject with GTK 3 (python3-gi + gir1.2-gtk-3.0 on Debian/Ubuntu,
python3-gobject on Fedora, python-gobject on Arch). Nothing else.
"""

import os
import shutil
import subprocess
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

EFFECTS = [
    ("off", "Off"),
    ("glorious", "Glorious (rainbow)"),
    ("seamless", "Seamless breathing"),
    ("breathing", "Breathing (multi-colour)"),
    ("normally_on", "Static"),
    ("breathing_single", "Breathing (single colour)"),
    ("tail", "Tail"),
    ("rave", "Rave"),
    ("wave", "Wave"),
]
POLLING = [125, 250, 500, 1000]
MAX_STAGES = 6
DPI_STEP = 50
DPI_MIN = 100
DPI_MAX = 26000


def find_cli():
    """A gloriousctl next to this script wins: installed, that is the same
    /usr/local/bin binary PATH would find; run from the source tree, it is the
    build you just made rather than a stale installed copy."""
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "gloriousctl"), shutil.which("gloriousctl")):
        if cand and os.access(cand, os.X_OK):
            return cand
    return None


def hex_to_rgba(hexstr):
    rgba = Gdk.RGBA()
    rgba.parse("#" + hexstr)
    return rgba


def rgba_to_hex(rgba):
    return "%02X%02X%02X" % (
        round(rgba.red * 255), round(rgba.green * 255), round(rgba.blue * 255))


class Window(Gtk.Window):
    def __init__(self, cli):
        super().__init__(title="gloriousctl")
        self.cli = cli
        self.set_default_size(640, 560)
        self.set_border_width(8)

        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.add(outer)

        self.device_label = Gtk.Label(xalign=0)
        self.device_label.set_markup("<b>Detecting device…</b>")
        outer.pack_start(self.device_label, False, False, 0)

        self.notebook = Gtk.Notebook()
        outer.pack_start(self.notebook, True, True, 0)
        self.notebook.append_page(self.build_dpi_tab(), Gtk.Label(label="DPI"))
        self.notebook.append_page(self.build_lighting_tab(), Gtk.Label(label="Lighting"))
        self.notebook.append_page(self.build_settings_tab(), Gtk.Label(label="Settings"))
        self.notebook.append_page(self.build_device_tab(), Gtk.Label(label="Device"))

        # Command log: every run's exact command line and output. The CLI's
        # own wording about transport-only acknowledgement is shown verbatim.
        frame = Gtk.Frame(label="Command log")
        outer.pack_start(frame, True, True, 0)
        scroll = Gtk.ScrolledWindow()
        scroll.set_min_content_height(140)
        frame.add(scroll)
        self.log = Gtk.TextView(editable=False, monospace=True, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        scroll.add(self.log)

        self.refresh()

    # ----------------------------------------------------------- helpers --

    def run(self, args, capture_only=False):
        """Run the CLI; append command and output to the log; return
        (stdout, stderr). The CLI prints its "Detected ..." line on stderr."""
        cmd = [self.cli] + args
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as exc:
            self.append_log("$ " + " ".join(cmd) + "\n" + str(exc) + "\n")
            return "", ""
        if not capture_only:
            text = "$ " + " ".join(cmd) + "\n" + proc.stdout
            if proc.stderr:
                text += proc.stderr
            if proc.returncode != 0:
                text += "(exit status %d)\n" % proc.returncode
            self.append_log(text)
        return proc.stdout, proc.stderr

    def append_log(self, text):
        buf = self.log.get_buffer()
        buf.insert(buf.get_end_iter(), text + "\n")
        mark = buf.create_mark(None, buf.get_end_iter(), False)
        self.log.scroll_to_mark(mark, 0, False, 0, 0)

    def read_state(self):
        out, _ = self.run(["--state"], capture_only=True)
        state = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                state[k.strip()] = v.strip()
        return state

    def refresh(self):
        out, err = self.run(["--info"], capture_only=True)
        detected = next((l for l in (out + err).splitlines() if l.startswith("Detected")), None)
        if detected:
            self.device_label.set_markup("<b>%s</b>" % GLib.markup_escape_text(detected))
        else:
            self.device_label.set_markup(
                "<b>No supported device detected</b> — showing cached settings; "
                "applying will fail until a mouse is connected.")
        st = self.read_state()
        if not st:
            self.append_log("Could not read state from the CLI.")
            return
        self.load_dpi(st)
        self.load_lighting(st)
        self.load_settings(st)

    def apply(self, args, then_refresh=True):
        self.run(args)
        if then_refresh:
            self.refresh()

    # ----------------------------------------------------------- DPI tab --

    def build_dpi_tab(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, border_width=8)

        top = Gtk.Box(spacing=8)
        box.pack_start(top, False, False, 0)
        top.pack_start(Gtk.Label(label="Stages:"), False, False, 0)
        self.stages_spin = Gtk.SpinButton.new_with_range(4, MAX_STAGES, 1)
        self.stages_spin.connect("value-changed", lambda *_: self.update_stage_rows())
        top.pack_start(self.stages_spin, False, False, 0)
        top.pack_start(Gtk.Label(label="Active stage:"), False, False, 12)
        self.active_combo = Gtk.ComboBoxText()
        for i in range(MAX_STAGES):
            self.active_combo.append_text(str(i + 1))
        top.pack_start(self.active_combo, False, False, 0)

        grid = Gtk.Grid(column_spacing=10, row_spacing=6)
        box.pack_start(grid, False, False, 0)
        grid.attach(Gtk.Label(label="Stage", xalign=0), 0, 0, 1, 1)
        grid.attach(Gtk.Label(label="DPI", xalign=0), 1, 0, 1, 1)
        grid.attach(Gtk.Label(label="Indicator colour", xalign=0), 2, 0, 1, 1)

        self.stage_rows = []
        for i in range(MAX_STAGES):
            label = Gtk.Label(label=str(i + 1), xalign=0)
            spin = Gtk.SpinButton.new_with_range(DPI_MIN, DPI_MAX, DPI_STEP)
            spin.set_value(400 * (2 ** min(i, 3)))
            color = Gtk.ColorButton()
            note = Gtk.Label(xalign=0)
            note.get_style_context().add_class("dim-label")
            grid.attach(label, 0, i + 1, 1, 1)
            grid.attach(spin, 1, i + 1, 1, 1)
            grid.attach(color, 2, i + 1, 1, 1)
            grid.attach(note, 3, i + 1, 1, 1)
            self.stage_rows.append((label, spin, color, note))

        hint = Gtk.Label(xalign=0, wrap=True)
        hint.set_markup(
            "<small>Stages 2 and 4: the blue channel of the indicator shares a wire byte "
            "with the next stage's DPI, so their blue is set by that DPI value, not by "
            "the colour picker. The mouse only picks up a new DPI for the stage it is "
            "currently on after you cycle away and back with the DPI button.</small>")
        box.pack_start(hint, False, False, 0)

        btns = Gtk.Box(spacing=8)
        box.pack_end(btns, False, False, 0)
        apply_btn = Gtk.Button(label="Apply DPI stages")
        apply_btn.connect("clicked", self.on_apply_dpi)
        btns.pack_end(apply_btn, False, False, 0)
        active_btn = Gtk.Button(label="Set active stage only")
        active_btn.connect("clicked", self.on_apply_active)
        btns.pack_end(active_btn, False, False, 0)
        return box

    def update_stage_rows(self):
        n = int(self.stages_spin.get_value())
        for i, (label, spin, color, note) in enumerate(self.stage_rows):
            on = i < n
            for w in (label, spin, color, note):
                w.set_sensitive(on)

    def load_dpi(self, st):
        n = int(st.get("stages", 4))
        self.stages_spin.set_value(n)
        self.active_combo.set_active(int(st.get("active", 1)) - 1)
        for i, (label, spin, color, note) in enumerate(self.stage_rows):
            dpi = int(st.get("dpi%d" % (i + 1), 0))
            if dpi:
                spin.set_value(dpi)
            color.set_rgba(hex_to_rgba(st.get("color%d" % (i + 1), "000000")))
            shared = st.get("shared_blue%d" % (i + 1)) == "1"
            note.set_text("blue follows stage %d DPI" % (i + 2) if shared else "")
        self.update_stage_rows()

    def on_apply_dpi(self, _btn):
        n = int(self.stages_spin.get_value())
        dpis = ",".join(str(int(self.stage_rows[i][1].get_value())) for i in range(n))
        colors = ",".join(rgba_to_hex(self.stage_rows[i][2].get_rgba()) for i in range(n))
        args = ["--set-stages", str(n), "--set-dpi", dpis, "--set-dpi-color", colors]
        active = self.active_combo.get_active()
        if 0 <= active < n:
            args += ["--set-active-stage", str(active + 1)]
        self.apply(args)

    def on_apply_active(self, _btn):
        active = self.active_combo.get_active()
        if active >= 0:
            self.apply(["--set-active-stage", str(active + 1)])

    # ------------------------------------------------------ Lighting tab --

    def build_lighting_tab(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, border_width=8)
        grid = Gtk.Grid(column_spacing=10, row_spacing=8)
        box.pack_start(grid, False, False, 0)

        grid.attach(Gtk.Label(label="Effect:", xalign=0), 0, 0, 1, 1)
        self.effect_combo = Gtk.ComboBoxText()
        for key, name in EFFECTS:
            self.effect_combo.append(key, name)
        grid.attach(self.effect_combo, 1, 0, 1, 1)

        grid.attach(Gtk.Label(label="Brightness:", xalign=0), 0, 1, 1, 1)
        self.brightness_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 1, 4, 1)
        self.brightness_scale.set_hexpand(True)
        grid.attach(self.brightness_scale, 1, 1, 1, 1)

        grid.attach(Gtk.Label(label="Speed:", xalign=0), 0, 2, 1, 1)
        self.speed_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 3, 1)
        self.speed_scale.set_hexpand(True)
        grid.attach(self.speed_scale, 1, 2, 1, 1)
        speed_note = Gtk.Label(xalign=0, wrap=True)
        speed_note.set_markup("<small>Speed is written where the datasheet says, but no "
                              "speed change has been observed on hardware yet.</small>")
        grid.attach(speed_note, 1, 3, 1, 1)

        grid.attach(Gtk.Label(label="Colours:", xalign=0), 0, 4, 1, 1)
        cbox = Gtk.Box(spacing=6)
        grid.attach(cbox, 1, 4, 1, 1)
        self.color_count_spin = Gtk.SpinButton.new_with_range(1, 7, 1)
        self.color_count_spin.connect("value-changed", lambda *_: self.update_color_buttons())
        cbox.pack_start(self.color_count_spin, False, False, 0)
        self.color_buttons = []
        for _ in range(7):
            b = Gtk.ColorButton()
            cbox.pack_start(b, False, False, 0)
            self.color_buttons.append(b)

        apply_btn = Gtk.Button(label="Apply lighting")
        apply_btn.connect("clicked", self.on_apply_lighting)
        hb = Gtk.Box()
        hb.pack_end(apply_btn, False, False, 0)
        box.pack_end(hb, False, False, 0)
        return box

    def update_color_buttons(self):
        n = int(self.color_count_spin.get_value())
        for i, b in enumerate(self.color_buttons):
            b.set_sensitive(i < n)

    def load_lighting(self, st):
        effect = st.get("effect", "glorious")
        if not self.effect_combo.set_active_id(effect):
            self.effect_combo.set_active(1)
        br = int(st.get("brightness", 0)) or 4
        self.brightness_scale.set_value(br)
        sp = int(st.get("speed", -1))
        self.speed_scale.set_value(sp if sp >= 0 else 1)
        n = int(st.get("color_count", 1))
        self.color_count_spin.set_value(max(1, min(7, n)))
        for i, b in enumerate(self.color_buttons):
            b.set_rgba(hex_to_rgba(st.get("effect_color%d" % (i + 1), "000000")))
        self.update_color_buttons()

    def on_apply_lighting(self, _btn):
        n = int(self.color_count_spin.get_value())
        colors = ",".join(rgba_to_hex(self.color_buttons[i].get_rgba()) for i in range(n))
        args = ["--set-effect", self.effect_combo.get_active_id() or "glorious",
                "--set-brightness", str(int(self.brightness_scale.get_value())),
                "--set-speed", str(int(self.speed_scale.get_value())),
                "--set-colors", colors]
        self.apply(args)

    # ------------------------------------------------------ Settings tab --

    def build_settings_tab(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, border_width=8)
        grid = Gtk.Grid(column_spacing=10, row_spacing=8)
        box.pack_start(grid, False, False, 0)

        grid.attach(Gtk.Label(label="Polling rate (Hz):", xalign=0), 0, 0, 1, 1)
        self.polling_combo = Gtk.ComboBoxText()
        for hz in POLLING:
            self.polling_combo.append(str(hz), str(hz))
        grid.attach(self.polling_combo, 1, 0, 1, 1)

        grid.attach(Gtk.Label(label="Lift-off distance (mm):", xalign=0), 0, 1, 1, 1)
        self.lod_combo = Gtk.ComboBoxText()
        self.lod_combo.append("1", "1")
        self.lod_combo.append("2", "2")
        grid.attach(self.lod_combo, 1, 1, 1, 1)

        grid.attach(Gtk.Label(label="Debounce (ms, even):", xalign=0), 0, 2, 1, 1)
        self.debounce_spin = Gtk.SpinButton.new_with_range(4, 16, 2)
        grid.attach(self.debounce_spin, 1, 2, 1, 1)

        note = Gtk.Label(xalign=0, wrap=True)
        note.set_markup("<small>125, 250 and 500 Hz measured on hardware; 1000 Hz reads as "
                        "500–1000 on browser meters (use evhz for an exact figure). Debounce "
                        "is sent as documented and cannot be verified (write-only device).</small>")
        box.pack_start(note, False, False, 0)

        apply_btn = Gtk.Button(label="Apply settings")
        apply_btn.connect("clicked", self.on_apply_settings)
        hb = Gtk.Box()
        hb.pack_end(apply_btn, False, False, 0)
        box.pack_end(hb, False, False, 0)
        return box

    def load_settings(self, st):
        self.polling_combo.set_active_id(st.get("polling", "1000"))
        self.lod_combo.set_active_id(st.get("lod", "1"))
        self.debounce_spin.set_value(int(st.get("debounce", 10)))

    def on_apply_settings(self, _btn):
        self.apply(["--set-polling-rate", self.polling_combo.get_active_id() or "1000",
                    "--set-lod", self.lod_combo.get_active_id() or "1",
                    "--set-debounce-time", str(int(self.debounce_spin.get_value()))])

    # -------------------------------------------------------- Device tab --

    def build_device_tab(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, border_width=8)
        row = Gtk.Box(spacing=8)
        box.pack_start(row, False, False, 0)
        for label, args in (("Show cached info", ["--info"]),
                            ("Probe HID devices", ["--probe"]),
                            ("Collect device report", ["--collect"]),
                            ("Reset cache to defaults", ["--reset-cache"])):
            b = Gtk.Button(label=label)
            b.connect("clicked", lambda _b, a=args: self.apply(a, then_refresh=a == ["--reset-cache"]))
            row.pack_start(b, False, False, 0)
        note = Gtk.Label(xalign=0, wrap=True)
        note.set_markup("<small>“Collect device report” gathers report descriptors and IDs "
                        "locally and prints them to the log — nothing is sent anywhere. "
                        "Paste it into a GitHub issue to get an unsupported mouse added.</small>")
        box.pack_start(note, False, False, 0)
        return box


def main():
    cli = find_cli()
    if not cli:
        dlg = Gtk.MessageDialog(message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE,
                                text="gloriousctl not found",
                                secondary_text="Install the command-line tool first (./install.sh).")
        dlg.run()
        return 1
    win = Window(cli)
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
