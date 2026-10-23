#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Super Mario Galaxy en Ports: portada y ficha en la lista de juegos de EmulationStation (KNULLI).
#
# La portada, la captura y la pantalla de titulo vienen de libretro-thumbnails (las que usa RetroArch) y se bajan
# una vez a esta carpeta; la ficha (nombre, descripcion, desarrollador...) se escribe en ../gamelist.xml, la de
# Ports a traves de la API local de EmulationStation (la carga en memoria y la guarda el); si no contesta, en
# ../gamelist.xml directamente, sin tocar las demas entradas. Uso: gamelist.py <carpeta smg>
import os
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

GAME_SCRIPT = "./Super Mario Galaxy.sh"
THUMBS = ("https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Wii/master/",
          "https://thumbnails.libretro.com/Nintendo%20-%20Wii/")
NAME = "Super Mario Galaxy (USA)"
MEDIA = {  # fichero local: carpeta de libretro-thumbnails
    "cover.png": "Named_Boxarts",
    "screenshot.png": "Named_Snaps",
    "title.png": "Named_Titles",
}
INFO = {
    "name": "Super Mario Galaxy",
    "desc": ("Mario viaja por el espacio para rescatar a la princesa Peach de Bowser, que se la ha llevado al centro "
             "del universo. Con la ayuda de Estela y los Destellos, salta entre planetoides con su propia gravedad, "
             "recoge Superestrellas para devolver la energía al Observatorio y explora decenas de galaxias. "
             "Port nativo para la RG35XX H: el código de Petari compilado para ARM64 con Nebula y gráficos OpenGL ES "
             "en la Mali-G31; el juego sale de tu propio disco. Mantén SELECT al abrirlo para cambiar el idioma."),
    "developer": "Nintendo EAD Tokio",
    "publisher": "Nintendo",
    "genre": "Plataformas",
    "releasedate": "20071112T000000",
    "players": "1-2",
    "lang": "es",
    "region": "us",
}


def log(text):
    print("portada: " + text, flush=True)


def fetch(folder, target):
    """Baja una imagen de libretro-thumbnails si aun no esta (urllib, si no curl o wget)."""
    stand_in = target + ".captura"  # a shot of the game stands in until the real picture comes
    if os.path.isfile(target) and os.path.getsize(target) > 1024 and not os.path.exists(stand_in):
        return True
    name = urllib.parse.quote(NAME + ".png")
    partial = target + ".part"
    for base in THUMBS:
        url = base + folder + "/" + name
        try:
            with urllib.request.urlopen(url, timeout=15) as reply, open(partial, "wb") as out:
                shutil.copyfileobj(reply, out)
        except Exception:
            for tool in (["curl", "-fsSL", "--max-time", "20", "-o", partial, url],
                         ["wget", "-q", "-T", "20", "-O", partial, url]):
                try:
                    if subprocess.run(tool, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                        break
                except OSError:
                    continue
        try:
            with open(partial, "rb") as f:
                is_png = f.read(8) == b"\x89PNG\r\n\x1a\n"
        except OSError:
            is_png = False
        if is_png and os.path.getsize(partial) > 1024:
            os.replace(partial, target)
            if os.path.exists(stand_in):
                os.remove(stand_in)
            log("%s descargada" % os.path.basename(target))
            return True
        if os.path.exists(partial):
            os.remove(partial)
    log("no se pudo bajar %s (sin red?); se intenta en el proximo arranque" % os.path.basename(target))
    return False


def ppm_to_png(source, target):
    """A shot the game took (shots/present_N.ppm, P6) as a PNG, so Ports has a picture without the network."""
    import struct
    import zlib
    with open(source, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:  # magic, width, height, maximum
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or int(fields[3]) != 255:
        return False
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        return False
    raw = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    chunk = lambda kind, body: struct.pack(">I", len(body)) + kind + body + \
        struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(target + ".part", "wb") as f:
        f.write(png)
    os.replace(target + ".part", target)
    return True


def own_shot(smg, target):
    """The latest shot of this game, if there is one (taken every 30 s while playing)."""
    shots = os.path.join(smg, "shots")
    try:
        names = [n for n in os.listdir(shots) if n.endswith(".ppm")]
    except OSError:
        return False
    names.sort(key=lambda n: os.path.getmtime(os.path.join(shots, n)))
    for name in reversed(names[1:] if len(names) > 1 else names):  # not the first: often the boot screen
        try:
            if ppm_to_png(os.path.join(shots, name), target):
                log("sin portada descargada: se usa la captura %s" % name)
                return True
        except (OSError, ValueError, IndexError):
            continue
    return False


def set_text(game, tag, value):
    node = game.find(tag)
    if node is None:
        node = ET.SubElement(game, tag)
    if node.text != value:
        node.text = value
        return True
    return False


def main():
    smg = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(__file__))
    ports = os.path.dirname(smg)
    result = 0
    have = {f: fetch(d, os.path.join(smg, f)) for f, d in MEDIA.items()}
    if not have["cover.png"] and own_shot(smg, os.path.join(smg, "captura.png")):
        os.replace(os.path.join(smg, "captura.png"), os.path.join(smg, "cover.png"))
        open(os.path.join(smg, "cover.png.captura"), "w").close()
        have["cover.png"] = True

    # The entry, with absolute paths (EmulationStation stores them relative to Ports).
    entry = ET.Element("game")
    set_text(entry, "path", os.path.join(ports, GAME_SCRIPT[2:]))
    for tag, value in INFO.items():
        set_text(entry, tag, value)
    if have["cover.png"]:
        for tag in ("image", "thumbnail", "boxart"):
            set_text(entry, tag, os.path.join(smg, "cover.png"))
    if have["title.png"]:
        set_text(entry, "titleshot", os.path.join(smg, "title.png"))
    if have["screenshot.png"]:
        set_text(entry, "fanart", os.path.join(smg, "screenshot.png"))
    return result | give(entry, GAME_SCRIPT, smg, ports)


def give(entry, script, smg, ports):
    """The entry to EmulationStation (its API), or into ../gamelist.xml if it does not answer."""
    folder = os.path.basename(smg)

    # EmulationStation keeps the game list in memory and, after a game, saves its own copy of the game's entry over
    # the file: an entry only written to gamelist.xml is lost. Its local API (127.0.0.1:1234, what Batocera's
    # scrapers use) takes the entry into memory and saves the file itself.
    body = b"<?xml version=\"1.0\"?>\n<gameList>" + ET.tostring(entry, encoding="utf-8") + b"</gameList>"
    try:
        request = urllib.request.Request("http://127.0.0.1:1234/addgames/ports", data=body, method="POST",
                                         headers={"Content-Type": "application/xml"})
        with urllib.request.urlopen(request, timeout=10) as reply:
            status = reply.status
        if 200 <= status < 300:
            log("%s: ficha y portada dadas a EmulationStation (%d); aparecen en Ports" % (script[2:-3], status))
            return 0
        log("EmulationStation contesto %d; se escribe gamelist.xml" % status)
    except Exception as failure:
        log("EmulationStation no contesta (%s); se escribe gamelist.xml" % failure)

    gamelist = os.path.join(ports, "gamelist.xml")
    try:
        tree = ET.parse(gamelist)
        root = tree.getroot()
    except (OSError, ET.ParseError):
        if os.path.exists(gamelist):
            log("gamelist.xml no se pudo leer; no se toca")
            return 1
        root = ET.Element("gameList")
        tree = ET.ElementTree(root)
    game = None
    for candidate in root.findall("game"):
        path = (candidate.findtext("path") or "").strip()
        if os.path.normpath(os.path.join(ports, path)) == os.path.join(ports, script[2:]):
            game = candidate
            break
    changed = game is None
    if game is None:
        game = ET.SubElement(root, "game")
        set_text(game, "path", script)
    for child in entry:
        if child.tag == "path":
            continue
        value = child.text
        if value.startswith(smg + os.sep):
            value = "./%s/%s" % (folder, os.path.basename(value))
        changed |= set_text(game, child.tag, value)
    if not changed:
        log("%s: la ficha de Ports ya esta al dia" % script[2:-3])
        return 0
    if os.path.exists(gamelist) and not os.path.exists(gamelist + ".antes-smg"):
        shutil.copy2(gamelist, gamelist + ".antes-smg")
    if hasattr(ET, "indent"):
        ET.indent(tree, space="\t")
    temporary = gamelist + ".smg-tmp"
    tree.write(temporary, encoding="utf-8", xml_declaration=True)
    os.replace(temporary, gamelist)
    log("ficha escrita en %s; en EmulationStation: START > Ajustes de juegos > Actualizar listas de juegos"
        % gamelist)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as failure:  # nunca debe impedir que arranque el juego
        log("fallo: %s" % failure)
        sys.exit(1)
