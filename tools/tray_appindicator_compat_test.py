#!/usr/bin/env python3

import argparse
import signal
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Richer GTK/AppIndicator tray compatibility harness for Noctalia submenu testing."
    )
    parser.add_argument(
        "--force-appindicator",
        action="store_true",
        help="Use AppIndicator3 first and fail if it is unavailable.",
    )
    parser.add_argument(
        "--force-ayatana",
        action="store_true",
        help="Use AyatanaAppIndicator3 first and fail if it is unavailable.",
    )
    parser.add_argument(
        "--force-status-icon",
        action="store_true",
        help="Use legacy Gtk.StatusIcon instead of AppIndicator/Ayatana.",
    )
    return parser.parse_args()


def load_indicator_backend(args: argparse.Namespace):
    if args.force_status_icon:
        return None, "GtkStatusIcon"

    backends: list[tuple[str, str]]
    if args.force_ayatana:
        backends = [("AyatanaAppIndicator3", "AyatanaAppIndicator3")]
    elif args.force_appindicator:
        backends = [("AppIndicator3", "AppIndicator3")]
    else:
        backends = [
            ("AppIndicator3", "AppIndicator3"),
            ("AyatanaAppIndicator3", "AyatanaAppIndicator3"),
        ]

    errors: list[str] = []
    for namespace, label in backends:
        try:
            gi.require_version(namespace, "0.1")
            module = __import__("gi.repository", fromlist=[namespace])
            return getattr(module, namespace), label
        except (ImportError, ValueError) as exc:
            errors.append(f"{label}: {exc}")

    return None, "GtkStatusIcon" if not (args.force_appindicator or args.force_ayatana) else "Unavailable"


class CompatTrayHarness:
    def __init__(self, backend_module, backend_name: str):
        self.backend_module = backend_module
        self.backend_name = backend_name
        self.dynamic_build_count = 0
        self.dynamic_menu = Gtk.Menu()
        self.radio_menu = Gtk.Menu()
        self.icon_menu = Gtk.Menu()
        self.deep_menu = Gtk.Menu()
        self.root_menu = self.build_root_menu()
        self.indicator = None
        self.status_icon = None

    def log(self, message: str) -> None:
        print(f"[tray-compat] {message}", flush=True)

    def on_activate(self, label: str):
        def _handler(_widget):
            self.log(f"activate: {label}")

        return _handler

    def on_radio_toggled(self, item: Gtk.RadioMenuItem, label: str) -> None:
        if item.get_active():
            self.log(f"toggle: {label}")

    def create_plain_item(self, label: str, callback=None) -> Gtk.MenuItem:
        item = Gtk.MenuItem.new_with_label(label)
        if callback is not None:
            item.connect("activate", callback)
        return item

    def create_image_item(self, label: str, icon_name: str, callback=None) -> Gtk.ImageMenuItem:
        item = Gtk.ImageMenuItem.new_with_label(label)
        image = Gtk.Image.new_from_icon_name(icon_name, Gtk.IconSize.MENU)
        item.set_image(image)
        try:
            item.set_always_show_image(True)
        except AttributeError:
            pass
        if callback is not None:
            item.connect("activate", callback)
        return item

    def clear_menu(self, menu: Gtk.Menu) -> None:
        for child in list(menu.get_children()):
            menu.remove(child)

    def build_static_submenu(self) -> Gtk.MenuItem:
        submenu = Gtk.Menu()

        direct = self.create_plain_item("Static Action", self.on_activate("Static Action"))
        submenu.append(direct)

        nested_item = self.create_plain_item("Static Nested")
        nested_menu = Gtk.Menu()
        nested_item.set_submenu(nested_menu)

        nested_menu.append(self.create_plain_item("Static Nested Action A", self.on_activate("Static Nested Action A")))
        nested_menu.append(self.create_plain_item("Static Nested Action B", self.on_activate("Static Nested Action B")))

        deep_item = self.create_plain_item("Static Level 3")
        deep_menu = Gtk.Menu()
        deep_item.set_submenu(deep_menu)
        deep_menu.append(self.create_plain_item("Static Level 3 Action", self.on_activate("Static Level 3 Action")))
        nested_menu.append(deep_item)

        submenu.append(nested_item)
        submenu.show_all()

        item = self.create_plain_item("Static Submenu")
        item.set_submenu(submenu)
        submenu.connect("show", lambda *_: self.log("menu show: Static Submenu"))
        return item

    def rebuild_dynamic_menu(self) -> None:
        self.dynamic_build_count += 1
        build_no = self.dynamic_build_count
        self.log(f"menu rebuild: Dynamic Submenu build={build_no}")
        self.clear_menu(self.dynamic_menu)

        self.dynamic_menu.append(
            self.create_plain_item(
                f"Dynamic Action {build_no}.1",
                self.on_activate(f"Dynamic Action {build_no}.1"),
            )
        )

        image_leaf = self.create_image_item(
            f"Dynamic Image Action {build_no}",
            "applications-graphics",
            self.on_activate(f"Dynamic Image Action {build_no}"),
        )
        self.dynamic_menu.append(image_leaf)

        nested_item = self.create_plain_item(f"Dynamic Nested {build_no}")
        nested_menu = Gtk.Menu()
        nested_item.set_submenu(nested_menu)
        nested_menu.connect("show", lambda *_: self.log(f"menu show: Dynamic Nested build={build_no}"))
        nested_menu.append(
            self.create_plain_item(
                f"Dynamic Nested Leaf {build_no}.A",
                self.on_activate(f"Dynamic Nested Leaf {build_no}.A"),
            )
        )
        nested_menu.append(
            self.create_image_item(
                f"Dynamic Nested Image {build_no}.B",
                "applications-system",
                self.on_activate(f"Dynamic Nested Image {build_no}.B"),
            )
        )
        self.dynamic_menu.append(nested_item)
        self.dynamic_menu.show_all()

    def build_dynamic_submenu(self) -> Gtk.MenuItem:
        item = self.create_plain_item("Dynamic Submenu")
        item.set_submenu(self.dynamic_menu)

        def on_show(*_args):
            self.log("menu show: Dynamic Submenu")
            self.rebuild_dynamic_menu()

        self.dynamic_menu.connect("show", on_show)
        self.rebuild_dynamic_menu()
        return item

    def build_radio_submenu(self) -> Gtk.MenuItem:
        item = self.create_plain_item("Radio Submenu")
        item.set_submenu(self.radio_menu)
        self.radio_menu.connect("show", lambda *_: self.log("menu show: Radio Submenu"))

        group = None
        for label in ["Radio Choice 1", "Radio Choice 2", "Radio Choice 3"]:
            radio = Gtk.RadioMenuItem.new_with_label_from_widget(group, label)
            if group is None:
                group = radio
            radio.connect("toggled", self.on_radio_toggled, label)
            self.radio_menu.append(radio)

        nested_item = self.create_plain_item("Radio Nested Submenu")
        nested_menu = Gtk.Menu()
        nested_item.set_submenu(nested_menu)
        nested_menu.connect("show", lambda *_: self.log("menu show: Radio Nested Submenu"))
        nested_menu.append(self.create_plain_item("Radio Nested Action", self.on_activate("Radio Nested Action")))
        self.radio_menu.append(Gtk.SeparatorMenuItem())
        self.radio_menu.append(nested_item)
        self.radio_menu.show_all()
        return item

    def build_icon_submenu(self) -> Gtk.MenuItem:
        item = self.create_plain_item("Icon Submenu")
        item.set_submenu(self.icon_menu)
        self.icon_menu.connect("show", lambda *_: self.log("menu show: Icon Submenu"))

        self.icon_menu.append(
            self.create_image_item("Icon Action Folder", "folder", self.on_activate("Icon Action Folder"))
        )
        self.icon_menu.append(
            self.create_image_item("Icon Action Preferences", "preferences-system", self.on_activate("Icon Action Preferences"))
        )

        icon_nested_item = self.create_image_item("Icon Nested", "view-list", None)
        icon_nested_menu = Gtk.Menu()
        icon_nested_item.set_submenu(icon_nested_menu)
        icon_nested_menu.connect("show", lambda *_: self.log("menu show: Icon Nested"))
        icon_nested_menu.append(
            self.create_image_item("Icon Nested Leaf", "dialog-information", self.on_activate("Icon Nested Leaf"))
        )
        self.icon_menu.append(icon_nested_item)
        self.icon_menu.show_all()
        return item

    def build_deep_submenu(self) -> Gtk.MenuItem:
        level1_item = self.create_plain_item("Deep Nested Submenu")
        level1_item.set_submenu(self.deep_menu)
        self.deep_menu.connect("show", lambda *_: self.log("menu show: Deep Nested Submenu"))

        level2_item = self.create_plain_item("Deep Level 2")
        level2_menu = Gtk.Menu()
        level2_item.set_submenu(level2_menu)
        level2_menu.connect("show", lambda *_: self.log("menu show: Deep Level 2"))

        level3_item = self.create_plain_item("Deep Level 3")
        level3_menu = Gtk.Menu()
        level3_item.set_submenu(level3_menu)
        level3_menu.connect("show", lambda *_: self.log("menu show: Deep Level 3"))

        level4_item = self.create_plain_item("Deep Level 4")
        level4_menu = Gtk.Menu()
        level4_item.set_submenu(level4_menu)
        level4_menu.connect("show", lambda *_: self.log("menu show: Deep Level 4"))

        level4_menu.append(
            self.create_plain_item("Deep Final Action", self.on_activate("Deep Final Action"))
        )
        level4_menu.append(
            self.create_image_item("Deep Final Image Action", "media-playback-start", self.on_activate("Deep Final Image Action"))
        )

        level3_menu.append(level4_item)
        level2_menu.append(level3_item)
        self.deep_menu.append(level2_item)
        self.deep_menu.show_all()
        return level1_item

    def build_root_menu(self) -> Gtk.Menu:
        root = Gtk.Menu()
        root.append(self.create_plain_item("Direct Root Action", self.on_activate("Direct Root Action")))
        root.append(Gtk.SeparatorMenuItem())
        root.append(self.build_static_submenu())
        root.append(self.build_dynamic_submenu())
        root.append(self.build_radio_submenu())
        root.append(self.build_icon_submenu())
        root.append(self.build_deep_submenu())
        root.append(Gtk.SeparatorMenuItem())
        root.append(self.create_plain_item("Quit", lambda _w: Gtk.main_quit()))
        root.show_all()
        return root

    def setup(self) -> None:
        if self.backend_module is None:
            self.status_icon = Gtk.StatusIcon()
            self.status_icon.set_name("noctalia-tray-appindicator-compat-test")
            self.status_icon.set_from_icon_name("applications-system")
            self.status_icon.set_visible(True)
            self.status_icon.connect("popup-menu", self.on_popup_menu)
            self.status_icon.connect("activate", self.on_activate_status_icon)
            return

        self.indicator = self.backend_module.Indicator.new(
            "noctalia-tray-appindicator-compat-test",
            "applications-system",
            self.backend_module.IndicatorCategory.APPLICATION_STATUS,
        )
        self.indicator.set_status(self.backend_module.IndicatorStatus.ACTIVE)
        self.indicator.set_title("Tray AppIndicator Compat Test")
        self.indicator.set_menu(self.root_menu)

    def on_popup_menu(self, _icon, button: int, activate_time: int) -> None:
        self.log(f"popup-menu: button={button} time={activate_time}")
        self.root_menu.show_all()
        self.root_menu.popup(None, None, Gtk.StatusIcon.position_menu, self.status_icon, button, activate_time)

    def on_activate_status_icon(self, _icon) -> None:
        self.log("status-icon activate")
        self.root_menu.show_all()
        self.root_menu.popup(None, None, Gtk.StatusIcon.position_menu, self.status_icon, 1, Gtk.get_current_event_time())


def main() -> int:
    args = parse_args()
    backend_module, backend_name = load_indicator_backend(args)
    if (args.force_appindicator or args.force_ayatana) and backend_module is None:
        print("[tray-compat] requested indicator backend is unavailable", file=sys.stderr, flush=True)
        return 1

    harness = CompatTrayHarness(backend_module, backend_name)
    harness.setup()

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    harness.log(
        "running. backend={} direct/static/dynamic/icon/radio/deep submenu paths are ready.".format(backend_name)
    )
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())