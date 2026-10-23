#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Super Mario Galaxy en Ports: portada y ficha en la lista de juegos de EmulationStation (KNULLI).
#
# La portada, la captura y la pantalla de titulo vienen de libretro-thumbnails (las que usa RetroArch) y se bajan
# una vez a esta carpeta; la ficha (nombre, descripcion, desarrollador...) se escribe en ../gamelist.xml, la de
# Ports, sin tocar las demas entradas ni lo que EmulationStation guarda (veces jugado, ultima vez...).
# EmulationStation la lee al arrancar o con "Actualizar listas de juegos". Uso: gamelist.py <carpeta smg>
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
             "en la Mali-G31; el juego sale de tu propio disco."),
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
    if os.path.isfile(target) and os.path.getsize(target) > 1024:
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
            log("%s descargada" % os.path.basename(target))
            return True
        if os.path.exists(partial):
            os.remove(partial)
    log("no se pudo bajar %s (sin red?); se intenta en el proximo arranque" % os.path.basename(target))
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
    folder = os.path.basename(smg)
    have = {f: fetch(d, os.path.join(smg, f)) for f, d in MEDIA.items()}

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
        if os.path.normpath(path.lstrip("./")) == os.path.normpath(GAME_SCRIPT.lstrip("./")) or \
                os.path.normpath(os.path.join(ports, path)) == os.path.join(ports, GAME_SCRIPT[2:]):
            game = candidate
            break
    changed = game is None
    if game is None:
        game = ET.SubElement(root, "game")
        set_text(game, "path", GAME_SCRIPT)
    for tag, value in INFO.items():
        changed |= set_text(game, tag, value)
    rel = lambda f: "./%s/%s" % (folder, f)
    if have["cover.png"]:
        for tag in ("image", "thumbnail", "boxart"):
            changed |= set_text(game, tag, rel("cover.png"))
    if have["title.png"]:
        changed |= set_text(game, "titleshot", rel("title.png"))
    if have["screenshot.png"]:
        changed |= set_text(game, "fanart", rel("screenshot.png"))
    if not changed:
        log("la ficha de Ports ya esta al dia")
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
