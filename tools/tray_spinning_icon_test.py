#!/usr/bin/env python3
"""StatusNotifierItem whose pixmap icon animates continuously.

Exercises tray hosts against items that redraw their icon many times per second.
Every click is printed so lost clicks are easy to spot.
"""

import argparse
import math
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

SIZE = 32
FRAME_COUNT = 24
PATH = "/StatusNotifierItem"
IFACE = "org.kde.StatusNotifierItem"

XML = """
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
  </interface>
</node>
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Tray item with a continuously animating pixmap icon.")
    parser.add_argument("--fps", type=float, default=20.0, help="icon frames per second (default: 20)")
    parser.add_argument(
        "--tooltip-interval",
        type=float,
        default=0.0,
        help="also change the tooltip text every N seconds (default: 0, static tooltip)",
    )
    return parser.parse_args()


def render(step: int) -> bytes:
    """ARGB32 in network byte order: a ring with a dot orbiting it."""
    angle = step * 2.0 * math.pi / FRAME_COUNT
    dot_x = SIZE / 2 + math.cos(angle) * 10
    dot_y = SIZE / 2 + math.sin(angle) * 10
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            cx, cy = x + 0.5, y + 0.5
            if math.hypot(cx - dot_x, cy - dot_y) <= 4.5:
                out += bytes((255, 255, 80, 40))
            elif abs(math.hypot(cx - SIZE / 2, cy - SIZE / 2) - 10) <= 1.5:
                out += bytes((255, 120, 160, 255))
            else:
                out += bytes((0, 0, 0, 0))
    return bytes(out)


class SpinningTray:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.frames = [render(i) for i in range(FRAME_COUNT)]
        self.frame = 0
        self.clicks = 0
        self.tooltip_ticks = 0
        self.conn = None

    def tooltip_body(self) -> str:
        body = f"clicks: {self.clicks}"
        if self.args.tooltip_interval > 0:
            body += f", tooltip updates: {self.tooltip_ticks}"
        return body

    def get_property(self, _conn, _sender, _path, _iface, name):
        values = {
            "Category": GLib.Variant("s", "ApplicationStatus"),
            "Id": GLib.Variant("s", "spinning-tray-test"),
            "Title": GLib.Variant("s", "Spinning tray test"),
            "Status": GLib.Variant("s", "Active"),
            "IconName": GLib.Variant("s", ""),
            "IconPixmap": GLib.Variant("a(iiay)", [(SIZE, SIZE, self.frames[self.frame % FRAME_COUNT])]),
            "ItemIsMenu": GLib.Variant("b", False),
            "Menu": GLib.Variant("o", "/NO_DBUSMENU"),
            "ToolTip": GLib.Variant("(sa(iiay)ss)", ("", [], "Spinning tray test", self.tooltip_body())),
        }
        return values.get(name)

    def method_call(self, _conn, _sender, _path, _iface, method, params, invocation):
        self.clicks += 1
        print(f"[{self.clicks}] {method}{tuple(params.unpack())}", flush=True)
        invocation.return_value(None)
        self.emit("NewToolTip")

    def emit(self, signal: str) -> None:
        self.conn.emit_signal(None, PATH, IFACE, signal, None)

    def next_frame(self) -> bool:
        self.frame += 1
        self.emit("NewIcon")
        return True

    def next_tooltip(self) -> bool:
        self.tooltip_ticks += 1
        self.emit("NewToolTip")
        return True

    def on_bus(self, conn, _name) -> None:
        self.conn = conn
        node = Gio.DBusNodeInfo.new_for_xml(XML)
        conn.register_object_with_closures2(PATH, node.interfaces[0], self.method_call, self.get_property, None)

    def on_name(self, conn, name) -> None:
        conn.call_sync(
            "org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher",
            "RegisterStatusNotifierItem",
            GLib.Variant("(s)", (name,)),
            None,
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )
        print(f"registered {name}, animating at {self.args.fps:g} fps; click the ring icon", flush=True)
        GLib.timeout_add(max(1, int(1000 / self.args.fps)), self.next_frame)
        if self.args.tooltip_interval > 0:
            GLib.timeout_add(max(1, int(1000 * self.args.tooltip_interval)), self.next_tooltip)


def main() -> int:
    args = parse_args()
    if args.fps <= 0:
        print("--fps must be positive", file=sys.stderr)
        return 1
    tray = SpinningTray(args)
    Gio.bus_own_name(
        Gio.BusType.SESSION,
        f"org.kde.StatusNotifierItem-{os.getpid()}-1",
        Gio.BusNameOwnerFlags.NONE,
        tray.on_bus,
        tray.on_name,
        None,
    )
    try:
        GLib.MainLoop().run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
