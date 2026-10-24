#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Super Mario Galaxy en Ports: portada y ficha en la lista de juegos del firmware, en el idioma del juego.
#
# La portada, la captura y la pantalla de titulo vienen de libretro-thumbnails (las que usa RetroArch) y se bajan
# una vez a esta carpeta. La ficha (nombre, descripcion, desarrollador...) va donde la lee cada firmware:
#   KNULLI, Batocera, ROCKNIX: la API local de EmulationStation (127.0.0.1:1234) la carga en memoria y la guarda el
#     en ports/gamelist.xml.
#   ArkOS, dArkOS, AmberELEC (EmulationStation sin API): ports/gamelist.xml. EmulationStation guarda al cerrarse su
#     copia en memoria de la entrada del juego (la tiene sucia por las partidas) y pisaria la nueva, asi que queda
#     smg/.ficha-pendiente y, al salir del juego, el .sh lo reinicia una vez escribiendo la ficha mientras esta parado
#     (gamelist.py <smg> --tras-cerrar-es).
#   muOS (no usa gamelist.xml): MUOS/info/catalogue/<catalogo de Ports>/text|box|preview/Super Mario Galaxy.*
# Ademas smg/gameinfo.xml (la copia de PortMaster) en el mismo idioma.
# Uso: gamelist.py <carpeta smg> [--cfw <CFW_NAME>] [--tras-cerrar-es]
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
# The game's entry in the language the game was installed in (smg/idioma.txt, written by Cactus Galaxy Wrapper:
# "spanish", "english", "french", or "<slot> ptbr" for the Brazilian Portuguese translation).
COMMON = {
    "name": "Super Mario Galaxy",
    "publisher": "Nintendo",
    "releasedate": "20071112T000000",
    "players": "1-2",
    "region": "us",
}
INFO_BY_LANGUAGE = {
    "es": {
        "desc": ("Mario viaja por el espacio para rescatar a la princesa Peach de Bowser, que se la ha llevado al "
                 "centro del universo. Con la ayuda de Estela y los Destellos, salta entre planetoides con su propia "
                 "gravedad, recoge Superestrellas para devolver la energía al Observatorio y explora decenas de "
                 "galaxias. Port nativo: el código de Petari compilado para ARM64 con Nebula y gráficos OpenGL ES en "
                 "la Mali-G31; el juego sale de tu propio disco. Mantén SELECT al abrirlo para cambiar el idioma."),
        "developer": "Nintendo EAD Tokio",
        "genre": "Plataformas",
        "lang": "es",
    },
    "en": {
        "desc": ("Mario travels through space to rescue Princess Peach from Bowser, who has taken her to the center of "
                 "the universe. With the help of Rosalina and the Lumas, he leaps between planetoids with their own "
                 "gravity, collects Power Stars to bring energy back to the Comet Observatory and explores dozens of "
                 "galaxies. Native port: Petari's code compiled for ARM64 with Nebula and OpenGL ES graphics on the "
                 "Mali-G31; the game comes from your own disc. Hold SELECT as it opens to change the language."),
        "developer": "Nintendo EAD Tokyo",
        "genre": "Platform",
        "lang": "en",
    },
    "fr": {
        "desc": ("Mario voyage dans l'espace pour sauver la princesse Peach, enlevée par Bowser au centre de "
                 "l'univers. Avec l'aide d'Harmonie et des Lumas, il saute de planétoïde en planétoïde, chacun avec sa "
                 "propre gravité, récupère des Grandes Étoiles pour rendre son énergie à l'Observatoire et explore des "
                 "dizaines de galaxies. Port natif : le code de Petari compilé pour ARM64 avec Nebula et des graphismes "
                 "OpenGL ES sur la Mali-G31 ; le jeu vient de ton propre disque. Maintiens SELECT au lancement pour "
                 "changer de langue."),
        "developer": "Nintendo EAD Tokyo",
        "genre": "Plates-formes",
        "lang": "fr",
    },
    "pt": {
        "desc": ("Mario viaja pelo espaço para resgatar a Princesa Peach de Bowser, que a levou para o centro do "
                 "universo. Com a ajuda de Rosalina e dos Lumas, ele salta entre planetoides com gravidade própria, "
                 "coleta Superestrelas para devolver a energia ao Observatório e explora dezenas de galáxias. Port "
                 "nativo: o código de Petari compilado para ARM64 com Nebula e gráficos OpenGL ES na Mali-G31; o jogo "
                 "vem do seu próprio disco, aqui com a tradução para o português do Brasil. Segure SELECT ao abrir "
                 "para mudar o idioma."),
        "developer": "Nintendo EAD Tóquio",
        "genre": "Plataforma",
        "lang": "pt",
    },
}


def game_language(smg):
    """es, en, fr or pt: the language the game was installed in (Spanish when none was chosen yet)."""
    try:
        with open(os.path.join(smg, "idioma.txt"), encoding="utf-8", errors="replace") as f:
            words = f.read().split()
    except OSError:
        return "es"
    if len(words) >= 2 and words[1].lower() == "ptbr":
        return "pt"
    return {"spanish": "es", "english": "en", "french": "fr"}.get(words[0].lower() if words else "", "es")


def game_info(smg):
    info = dict(COMMON)
    info.update(INFO_BY_LANGUAGE[game_language(smg)])
    return info


def write_gameinfo(smg, info):
    """PortMaster's copy of the entry (smg/gameinfo.xml), in the same language."""
    root = ET.Element("gameList")
    game = ET.SubElement(root, "game")
    for tag, value in [("path", GAME_SCRIPT)] + [(k, info[k]) for k in ("name", "desc", "releasedate", "developer",
                                                                          "publisher", "genre", "players")]:
        ET.SubElement(game, tag).text = value
    ET.SubElement(game, "image").text = "./smg/cover.png"
    tree = ET.ElementTree(root)
    if hasattr(ET, "indent"):
        ET.indent(tree, space="\t")
    target = os.path.join(smg, "gameinfo.xml")
    try:
        tree.write(target + ".tmp", encoding="utf-8", xml_declaration=True)
        os.replace(target + ".tmp", target)
    except OSError:
        pass


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
    args = sys.argv[1:]
    after_es = "--tras-cerrar-es" in args
    cfw = os.environ.get("CFW_NAME", "")
    if "--cfw" in args and args.index("--cfw") + 1 < len(args):
        cfw = args[args.index("--cfw") + 1]
    paths = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] != "--cfw")]
    smg = os.path.abspath(paths[0] if paths else os.path.dirname(__file__))
    ports = os.path.dirname(smg)
    result = 0
    if after_es:  # EmulationStation is stopped: no downloads, only the entry
        have = {f: os.path.isfile(os.path.join(smg, f)) for f in MEDIA}
    else:
        have = {f: fetch(d, os.path.join(smg, f)) for f, d in MEDIA.items()}
    if not have["cover.png"] and not after_es and own_shot(smg, os.path.join(smg, "captura.png")):
        os.replace(os.path.join(smg, "captura.png"), os.path.join(smg, "cover.png"))
        open(os.path.join(smg, "cover.png.captura"), "w").close()
        have["cover.png"] = True

    info = game_info(smg)
    log("ficha en %s (idioma del juego), firmware %s" % (info["lang"], cfw or "?"))
    write_gameinfo(smg, info)
    if is_muos(cfw):
        return give_muos(smg, info, have)

    # The entry, with absolute paths (EmulationStation stores them relative to Ports).
    entry = ET.Element("game")
    set_text(entry, "path", os.path.join(ports, GAME_SCRIPT[2:]))
    for tag, value in info.items():
        set_text(entry, tag, value)
    if have["cover.png"]:
        for tag in ("image", "thumbnail", "boxart"):
            set_text(entry, tag, os.path.join(smg, "cover.png"))
    if have["title.png"]:
        set_text(entry, "titleshot", os.path.join(smg, "title.png"))
    if have["screenshot.png"]:
        set_text(entry, "fanart", os.path.join(smg, "screenshot.png"))
    return result | give(entry, GAME_SCRIPT, smg, ports, after_es)


def is_muos(cfw):
    return "muos" in cfw.lower() or os.path.isdir("/opt/muos")


def muos_catalogue():
    """MUOS/info/catalogue/<the catalogue muOS gives the Ports folder> ("External - Ports" unless reassigned)."""
    for base in ("/run/muos/storage/info/catalogue", "/mnt/mmc/MUOS/info/catalogue", "/mnt/sdcard/MUOS/info/catalogue"):
        if os.path.isdir(base):
            break
    else:
        return None
    name = "External - Ports"
    content = "/opt/muos/share/info/content"
    try:
        for folder in os.listdir(content):
            if folder.lower() == "ports":
                with open(os.path.join(content, folder, "core.cfg"), encoding="utf-8", errors="replace") as f:
                    lines = f.read().splitlines()
                if len(lines) >= 3 and lines[2].strip():  # line 3: the catalogue
                    name = lines[2].strip()
                break
    except OSError:
        pass
    return os.path.join(base, name)


def resized_copy(source, target):
    """source as a PNG of at most 320x240 (what PortMaster gives muOS), resized if the firmware has a tool for it."""
    partial = target + ".part.png"
    shutil.copyfile(source, partial)
    size = "320x240>"
    for tool in (["mogrify", "-resize", size, partial],
                 ["/opt/graphicsmagick/bin/gm", "mogrify", "-resize", size, partial],
                 ["gm", "mogrify", "-resize", size, partial],
                 ["magick", "mogrify", "-resize", size, partial]):
        try:
            if subprocess.run(tool, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                break
        except OSError:
            continue
    os.replace(partial, target)  # resized, or as it came if there is no tool (newer muOS scales it itself)


def give_muos(smg, info, have):
    """The entry where muOS reads it: text/<script>.txt (one line), box/ and preview/<script>.png."""
    catalogue = muos_catalogue()
    if catalogue is None:
        log("muOS sin carpeta info/catalogue; no se escribe la ficha")
        return 1
    program = GAME_SCRIPT[2:-3]  # the script's name without .sh: "Super Mario Galaxy"
    changed = []
    try:
        for kind in ("text", "box", "preview"):
            os.makedirs(os.path.join(catalogue, kind), exist_ok=True)
        text = os.path.join(catalogue, "text", program + ".txt")
        desc = info["desc"].strip().split("\n", 1)[0].strip() + "\n"
        try:
            with open(text, encoding="utf-8") as f:
                current = f.read()
        except OSError:
            current = None
        if current != desc:
            with open(text + ".part", "w", encoding="utf-8") as f:
                f.write(desc)
            os.replace(text + ".part", text)
            changed.append("texto")
        for kind, source in (("box", "cover.png"), ("preview", "screenshot.png")):
            source = os.path.join(smg, source)
            target = os.path.join(catalogue, kind, program + ".png")
            if not os.path.isfile(source):
                continue
            if os.path.isfile(target) and os.path.getmtime(target) >= os.path.getmtime(source):
                continue
            resized_copy(source, target)
            changed.append(kind)
    except OSError as failure:
        log("muOS: no se pudo escribir la ficha en %s (%s)" % (catalogue, failure))
        return 1
    if changed:
        log("muOS: ficha en %s (%s)" % (catalogue, ", ".join(changed)))
    else:
        log("muOS: la ficha ya esta al dia")
    return 0


def give(entry, script, smg, ports, after_es=False):
    """The entry to EmulationStation (its API), or into ../gamelist.xml if it does not answer."""
    folder = os.path.basename(smg)
    pending = os.path.join(smg, ".ficha-pendiente")

    # EmulationStation keeps the game list in memory and, after a game, saves its own copy of the game's entry over
    # the file: an entry only written to gamelist.xml is lost. Its local API (127.0.0.1:1234, what Batocera's
    # scrapers use) takes the entry into memory and saves the file itself.
    body = b"<?xml version=\"1.0\"?>\n<gameList>" + ET.tostring(entry, encoding="utf-8") + b"</gameList>"
    try:
        if after_es:
            raise RuntimeError("parado para escribir la ficha")
        request = urllib.request.Request("http://127.0.0.1:1234/addgames/ports", data=body, method="POST",
                                         headers={"Content-Type": "application/xml"})
        with urllib.request.urlopen(request, timeout=10) as reply:
            status = reply.status
        if 200 <= status < 300:
            log("%s: ficha y portada dadas a EmulationStation (%d); aparecen en Ports" % (script[2:-3], status))
            if os.path.exists(pending):
                os.remove(pending)
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
    if after_es and os.path.exists(pending):
        os.remove(pending)
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
    if hasattr(os, "geteuid") and os.geteuid() == 0:  # written as root (--tras-cerrar-es): the owner of Ports keeps it
        try:
            owner = os.stat(ports)
            os.chown(gamelist, owner.st_uid, owner.st_gid)
        except OSError:
            pass
    if after_es:
        log("ficha escrita en %s con EmulationStation parado" % gamelist)
    else:
        # EmulationStation would save its own copy of the entry over this one when it closes: the .sh restarts it once
        # after the game, writing the entry while it is stopped.
        open(pending, "w").close()
        log("ficha escrita en %s; EmulationStation se reinicia al salir del juego para cargarla" % gamelist)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as failure:  # nunca debe impedir que arranque el juego
        log("fallo: %s" % failure)
        sys.exit(1)
