// Cactus Galaxy Wrapper's texts in the languages of KNULLI's menu it follows (system.language): Spanish, English,
// French, Portuguese, Italian and German. Each line fits the panel (22 characters at the body size) or is wrapped.
#pragma once

enum Str {
  S_CHOOSE, S_NO_DISC, S_NO_DISC_HELP, S_CHECKING, S_OTHER_GAME, S_NOT_DISC, S_LANGUAGE, S_FREE, S_HAVE_PAK,
  S_HAVE_PAK_ONLY, S_LOW, S_CONVERT, S_EXIT, S_DELETE_DISC, S_YES, S_NO, S_GAME_LANGUAGE, S_GAME_LANGUAGE_SUB,
  S_CHANGE_LATER, S_INSTALL, S_BACK, S_CONVERTING, S_CANCELLING, S_READING, S_LEFT, S_DONT_TURN_OFF, S_CANCEL,
  S_DONE, S_PAK_CREATED, S_IN, S_DISC_DELETED, S_BLOOMED, S_STARTS_IN, S_PLAY_NOW, S_OUCH, S_COULD_NOT,
  S_ERR_RUN, S_ERR_EXIT, S_ERR_SPACE, S_ERR_SPACE_SHORT, S_ERR_FAT32, S_ERR_INCOMPLETE, S_ERR_DOL, S_ERR_CANCELLED,
  S_COUNT
};

enum UiLanguage { UI_ES, UI_EN, UI_FR, UI_PT, UI_IT, UI_DE, UI_COUNT };

// "es", "en"... as system.language starts.
inline const char* const kUiCodes[UI_COUNT] = {"es", "en", "fr", "pt", "it", "de"};

inline const char* const kText[UI_COUNT][S_COUNT] = {
    {  // Español
        "Elige tu disco:", "No encontré ningún disco",
        "Pon tu ISO o RVZ de Super Mario Galaxy (USA) en roms/ports/smg/gamedata o en roms/wii y vuelve a abrir.",
        "comprobando…", "otro juego", "no es un disco de Wii", "Idioma: ", "Libre: ", "Hay game.pak · ",
        "Ya hay game.pak", " (poco)", "Convertir", "Salir", "Borrar disco: ", "Sí", "No", "Idioma del juego",
        "Language · Langue", "Se cambia luego con ◄►", "Instalar", "Volver", "Convirtiendo", "Cancelando…",
        "Leyendo el disco…", "Quedan %s", "No apagues la consola", "Cancelar", "¡LISTO!", "game.pak creado", "en %s",
        "Disco borrado", "Tu cactus floreció", "Empieza en %d s", "Jugar ya", "¡AY!", "No se pudo crear",
        "No se pudo ejecutar nebula-pak.", "nebula-pak terminó con el código %d.",
        "No hay espacio: hacen falta %s libres y hay %s.", "No queda espacio en la tarjeta.",
        "La tarjeta está en FAT32, que no admite archivos de más de 4 GB: usa exFAT o ext4.",
        "La imagen del disco está incompleta o dañada: vuelve a copiarla.",
        "main.dol no es el original: la imagen puede estar dañada.", "Conversión cancelada.",
    },
    {  // English
        "Choose your disc:", "No disc found",
        "Put your Super Mario Galaxy (USA) ISO or RVZ in roms/ports/smg/gamedata or in roms/wii and open this again.",
        "checking…", "another game", "not a Wii disc", "Language: ", "Free: ", "Has game.pak · ",
        "game.pak already made", " (low)", "Convert", "Exit", "Delete disc: ", "Yes", "No", "Game language",
        "Idioma · Langue", "Change it later: ◄►", "Install", "Back", "Converting", "Cancelling…",
        "Reading the disc…", "%s left", "Keep the console on", "Cancel", "DONE!", "game.pak created", "in %s",
        "Disc deleted", "Your cactus bloomed", "Starting in %d s", "Play now", "OOPS!", "Could not create",
        "Could not run nebula-pak.", "nebula-pak ended with code %d.",
        "Not enough space: %s free needed, %s available.", "The card is out of space.",
        "The card is FAT32, which cannot hold files over 4 GB: use exFAT or ext4.",
        "The disc image is incomplete or damaged: copy it again.",
        "main.dol is not the original one: the image may be damaged.", "Conversion cancelled.",
    },
    {  // Français
        "Choisis ton disque :", "Aucun disque trouvé",
        "Mets ton ISO ou RVZ de Super Mario Galaxy (USA) dans roms/ports/smg/gamedata ou dans roms/wii, puis rouvre.",
        "vérification…", "autre jeu", "pas un disque Wii", "Langue : ", "Libre : ", "game.pak là · ",
        "game.pak déjà fait", " (peu)", "Convertir", "Quitter", "Effacer disque : ", "Oui", "Non", "Langue du jeu",
        "Idioma · Language", "Modifiable avec ◄►", "Installer", "Retour", "Conversion", "Annulation…",
        "Lecture du disque…", "Reste %s", "Ne pas éteindre", "Annuler", "PRÊT !", "game.pak créé", "en %s",
        "Disque effacé", "Ton cactus a fleuri", "Début dans %d s", "Jouer", "OUPS !", "Impossible de créer",
        "Impossible de lancer nebula-pak.", "nebula-pak s'est arrêté avec le code %d.",
        "Pas assez d'espace : il faut %s libres, il y a %s.", "La carte est pleine.",
        "La carte est en FAT32, qui n'accepte pas les fichiers de plus de 4 Go : utilise exFAT ou ext4.",
        "L'image du disque est incomplète ou abîmée : recopie-la.",
        "main.dol n'est pas l'original : l'image est peut-être abîmée.", "Conversion annulée.",
    },
    {  // Português
        "Escolha seu disco:", "Nenhum disco encontrado",
        "Coloque seu ISO ou RVZ de Super Mario Galaxy (USA) em roms/ports/smg/gamedata ou em roms/wii e abra de novo.",
        "verificando…", "outro jogo", "não é disco de Wii", "Idioma: ", "Livre: ", "Tem game.pak · ",
        "game.pak já feito", " (pouco)", "Converter", "Sair", "Apagar disco: ", "Sim", "Não", "Idioma do jogo",
        "Language · Langue", "Muda depois com ◄►", "Instalar", "Voltar", "Convertendo", "Cancelando…",
        "Lendo o disco…", "Faltam %s", "Não desligue o console", "Cancelar", "PRONTO!", "game.pak criado", "em %s",
        "Disco apagado", "Seu cacto floresceu", "Começa em %d s", "Jogar já", "OPA!", "Não foi possível criar",
        "Não foi possível executar nebula-pak.", "nebula-pak terminou com o código %d.",
        "Sem espaço: precisa de %s livres e há %s.", "O cartão está sem espaço.",
        "O cartão está em FAT32, que não aceita arquivos de mais de 4 GB: use exFAT ou ext4.",
        "A imagem do disco está incompleta ou danificada: copie de novo.",
        "main.dol não é o original: a imagem pode estar danificada.", "Conversão cancelada.",
    },
    {  // Italiano
        "Scegli il disco:", "Nessun disco trovato",
        "Metti la ISO o RVZ di Super Mario Galaxy (USA) in roms/ports/smg/gamedata o in roms/wii e riapri.",
        "verifica…", "altro gioco", "non è un disco Wii", "Lingua: ", "Liberi: ", "game.pak c'è · ",
        "game.pak già fatto", " (poco)", "Converti", "Esci", "Elimina disco: ", "Sì", "No", "Lingua del gioco",
        "Language · Idioma", "Si cambia poi con ◄►", "Installa", "Indietro", "Conversione", "Annullamento…",
        "Lettura del disco…", "Mancano %s", "Non spegnere", "Annulla", "FATTO!", "game.pak creato", "in %s",
        "Disco eliminato", "Il cactus è fiorito", "Si parte tra %d s", "Gioca ora", "OPS!", "Impossibile creare",
        "Impossibile avviare nebula-pak.", "nebula-pak è terminato con il codice %d.",
        "Spazio insufficiente: servono %s liberi, ci sono %s.", "La scheda è piena.",
        "La scheda è in FAT32, che non accetta file oltre 4 GB: usa exFAT o ext4.",
        "L'immagine del disco è incompleta o danneggiata: ricopiala.",
        "main.dol non è l'originale: l'immagine potrebbe essere danneggiata.", "Conversione annullata.",
    },
    {  // Deutsch
        "Wähle deine Disc:", "Keine Disc gefunden",
        "Lege deine ISO oder RVZ von Super Mario Galaxy (USA) in roms/ports/smg/gamedata oder roms/wii und öffne erneut.",
        "prüfe…", "anderes Spiel", "keine Wii-Disc", "Sprache: ", "Frei: ", "game.pak da · ",
        "game.pak schon da", " (wenig)", "Umwandeln", "Beenden", "Disc löschen: ", "Ja", "Nein", "Spielsprache",
        "Language · Idioma", "Später ändern: ◄►", "Installieren", "Zurück", "Wird umgewandelt", "Abbruch…",
        "Disc wird gelesen…", "Noch %s", "Nicht ausschalten", "Abbrechen", "FERTIG!", "game.pak erstellt", "in %s",
        "Disc gelöscht", "Dein Kaktus blüht", "Start in %d s", "Jetzt spielen", "HUCH!", "Konnte nicht erstellen",
        "nebula-pak startet nicht.", "nebula-pak endete mit Code %d.",
        "Zu wenig Platz: %s frei nötig, %s vorhanden.", "Die Karte ist voll.",
        "Die Karte ist FAT32 und kann keine Dateien über 4 GB speichern: nimm exFAT oder ext4.",
        "Das Disc-Abbild ist unvollständig oder beschädigt: kopiere es neu.",
        "main.dol ist nicht das Original: das Abbild ist vielleicht beschädigt.", "Umwandlung abgebrochen.",
    },
};
