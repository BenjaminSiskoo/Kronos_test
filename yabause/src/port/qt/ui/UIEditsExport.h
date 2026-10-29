/*  UIEditsExport -- enregistrer les edits du debogueur VDP : pack .kmod, image
    patchee, patch BPS / IPS. Voir EditsPatcher.h pour le fonctionnement. */

#ifndef UIEDITSEXPORT_H
#define UIEDITSEXPORT_H

#include <QDialog>
#include <vector>
#include "../EditsPatcher.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;

class UIEditsExport : public QDialog
{
    Q_OBJECT

public:
    explicit UIEditsExport(QWidget *parent = 0);

private slots:
    void onChooseImage();
    void onSavePack();
    void onLoadPack();
    void onAnalyze();
    void onExportImage();
    void onExportBps();
    void onExportIps();

private:
    // edits du coeur, sous forme "pack" (une entree par sprite / tuile / couleur de CRAM)
    std::vector<EditsPatcher::Record> collectPack() const;
    // edits prets a etre cherches sur le disque (couleurs regroupees en blocs de palette)
    std::vector<EditsPatcher::Record> collectForDisc() const;
    // ouvre l'image et construit les changements ; false si annule ou en erreur
    bool prepare(EditsPatcher::Image &img, EditsPatcher::Build &build);
    void refreshSummary();
    void addLog(const QString &line);
    QString imageBaseName() const;

    QLabel *mSummary;
    QLineEdit *mImage;
    QPlainTextEdit *mLog;
};

#endif
