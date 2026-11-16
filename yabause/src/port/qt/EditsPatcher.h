/*  EditsPatcher -- enregistre les edits du debogueur VDP dans un pack .kmod ou
    les reporte dans l'image disque (image patchee, patch BPS ou IPS).

    Un edit est un couple (octets d'origine, octets modifies) pris dans la VRAM /
    la CRAM de l'emulateur. Pour le reporter sur le disque, on retrouve les octets
    d'origine dans les donnees utilisateur de l'image (secteurs de 2048 octets),
    on les remplace, puis on regenere EDC/ECC des secteurs bruts touches.
    Directement, ca ne marche que si les donnees sont stockees telles quelles sur
    le disque ; UIEditsExport cherche ensuite les edits non trouves dans les blocs
    PRS (TextTools) et les passe a buildPatches en 'extra'. Le pack .kmod marche
    toujours. Aucune dependance a Qt ni au coeur de l'emulateur (C++11). */

#ifndef EDITSPATCHER_H
#define EDITSPATCHER_H

#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace EditsPatcher {

struct Record {
    int type;                               // 0 = VRAM VDP1, 1 = VRAM VDP2, 2 = CRAM
    uint32_t addr;                          // adresse octet VRAM / CRAM (informatif)
    std::vector<uint8_t> orig, repl;        // octets avant / apres (meme taille)
    std::vector<uint8_t> origAlt, replAlt;  // variante plus courte, cherchee si orig est introuvable
    std::string label;                      // texte du journal
    // pack v3
    bool enabled;                           // false : enregistre mais desactive
    std::string name;                       // nom donne par l'utilisateur
    std::string meta;                       // descripteur (voir vdpedit.h)
    Record() : type(0), addr(0), enabled(true) {}
};

// En-tete d'un pack : lignes "cle=valeur". Cles utilisees : product (numero de
// produit du disque, IP.BIN), version, title, saved (date), tool.
typedef std::map<std::string, std::string> PackInfo;
std::string packInfoToString(const PackInfo &info);
PackInfo packInfoFromString(const std::string &text);

// Piste de donnees d'une image : secteurs de sectorSize octets dont les 2048 octets
// utiles commencent a dataOffset (mode 1 brut : 16, mode 2 brut : 24, ISO : 0).
struct Track {
    std::string file;
    uint64_t fileOffset;
    uint32_t sectorSize;    // 2048, 2352 ou 2336
    uint32_t dataOffset;
    int mode;               // 0 = cooked 2048, 1 = Mode 1 brut, 2 = Mode 2 brut
    uint64_t sectors;
    Track() : fileOffset(0), sectorSize(2048), dataOffset(0), mode(0), sectors(0) {}
};

struct Image {
    std::string path;                       // fichier ouvert (.cue / .iso / .bin / .img)
    std::vector<std::string> files;         // tous les fichiers de l'image (a copier)
    std::vector<Track> tracks;              // pistes de donnees uniquement
};

struct Change { uint64_t offset; std::vector<uint8_t> bytes; };

struct Status {
    int matches;            // occurrences trouvees (retenues)
    bool usedAlt;
    std::string note;
    Status() : matches(0), usedAlt(false) {}
};

struct Build {
    std::vector<Status> status;                          // un par Record
    std::map<std::string, std::vector<Change> > changes; // par fichier, tries, sans chevauchement
    std::map<std::string, uint64_t> fileSize;
    std::vector<std::string> log;
    int applied;                                         // records reportes sur le disque
    Build() : applied(0) {}
};

// Rappel de progression ; retourner false pour annuler.
typedef std::function<bool(const char *what, double fraction)> Progress;

bool openImage(const std::string &path, Image &img, std::string &err);
struct DiskEdit;
// 'extra': ecritures a des offsets connus ajoutees aux edits trouves (blocs
// compresses recompresses, voir UIEditsExport) ; leur resultat va dans le journal.
bool buildPatches(const Image &img, const std::vector<Record> &recs, Build &out,
                  std::string &err, Progress prog, const std::vector<DiskEdit> *extra = 0);
// Ecritures a une position connue des donnees utilisateur d'une piste (textes
// traduits, voir TextTools.h) : 'offset' = LBA relatif a la piste * 2048 + octet.
// Les octets d'origine sont verifies sur le disque avant d'etre remplaces.
struct DiskEdit {
    int track;
    uint64_t offset;
    std::vector<uint8_t> orig, repl;        // meme taille
    std::string label;
    DiskEdit() : track(0), offset(0) {}
};
bool buildDiskPatches(const Image &img, const std::vector<DiskEdit> &edits, Build &out, std::string &err);

// Lecture des donnees utilisateur (2048 octets par secteur) d'une piste de donnees.
uint64_t userDataSize(const Image &img, int track);
bool readUserData(const Image &img, int track, uint64_t offset, size_t n, std::vector<uint8_t> &out,
                  std::string &err);

// Copie l'image dans destDir (qui doit etre un autre dossier) et y applique les changements.
bool writePatchedImage(const Image &img, const Build &b, const std::string &destDir,
                       std::string &err, Progress prog);
bool writeBps(const std::string &sourceFile, const std::vector<Change> &changes,
              const std::string &destPath, std::string &err, Progress prog);
// IPS classique : offsets sur 24 bits, donc limite a 16 Mio.
bool writeIps(const std::vector<Change> &changes, const std::string &destPath, std::string &err);

// Pack .kmod : edits rejouables sur n'importe quelle image.
bool savePack(const std::string &path, const std::vector<Record> &recs, const std::string &meta,
              std::string &err);
bool loadPack(const std::string &path, std::vector<Record> &recs, std::string &meta,
              std::string &err);

// Utilitaires exposes pour les tests.
uint32_t crc32(uint32_t crc, const uint8_t *data, size_t len);
void regenerateSector(uint8_t *sector, int sectorSize, int mode);

} // namespace EditsPatcher

#endif
