Nouveautés

Support des exécutables CPE de SN Systems (format des kits de développement Saturn) :
le fichier est reconnu à sa signature, pas à son extension ;
chaque bloc est chargé à son adresse et le programme démarre à son propre point d’entrée.
Outils › Transfert mémoire :
accepte les fichiers .cpe ;
l’option « Run program » réinitialise la machine dans l’état laissé par le BIOS, charge le programme puis le lance.
Ligne de commande : --binary FICHIER[:ADRESSE] charge et lance un CPE, ou un binaire brut à l’adresse donnée (06004000 par défaut).
Le programme est relancé à chaque reset.
Il est abandonné dès qu’une autre image est ouverte.
Pour démarrer sans disque, l’associer à --no-disc.

Corrections

Transfert mémoire :
la case « Set PC to Start Address » n’avait aucun effet ;
les fichiers COFF/ELF étaient chargés comme des binaires bruts, en-têtes compris (filtres retirés) ;
plantage si l’émulation n’était pas démarrée.
Un binaire mal lu n’est plus écrit en mémoire avec des données non initialisées.
--binary ignorait le fichier et l’adresse, et ne lançait pas l’émulation.
Changelog (EN)

New

Support for SN Systems CPE executables (the Saturn development kit format):
files are recognized by their signature, not by their extension;
each block is loaded at its own address and the program starts at its own entry point.
Tools › Memory Transfer:
accepts .cpe files;
the “Run program” option resets the machine to the state left by the BIOS, loads the program and starts it.
Command line: --binary FILE[:ADDRESS] loads and runs a CPE, or a raw binary at ADDRESS (default 06004000).
The program is restarted on every reset.
It is dropped as soon as another image is opened.
Use it with --no-disc to start without a disc.

Fixes

Memory Transfer:
the “Set PC to Start Address” checkbox had no effect;
COFF/ELF files were loaded as raw binaries, headers included (filters removed);
crash when the emulation was not started.
A binary that was not fully read is no longer written to memory with uninitialized data.
--binary ignored the file and the address, and did not start the emulation.
