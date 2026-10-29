/*  Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#ifndef UIDEBUGVDP2VIEWER_H
#define UIDEBUGVDP2VIEWER_H

#include "ui_UIDebugVDP2Viewer.h"
#include "../QtYabause.h"
#include <QImage>
#include <QString>

class QTreeWidget;
class QTreeWidgetItem;

// Remplit 'tree' (sous 'parent', ou a la racine si parent == NULL) avec le
// texte d'une fonction Vdp2DebugStatsXXX(), regroupe par theme (Alertes,
// Format, Carte, Scroll & zoom, Rotation, Fenetres, Couleur, Priorite,
// Sprite). general = 1 pour Vdp2DebugStatsGeneral, dont les sections sont
// les titres soulignes de "-----". Partage par l'onglet Debug du viewer et
// par la fenetre UIDebugVDP2.
void Vdp2FillStatsTree(QTreeWidget *tree, QTreeWidgetItem *parent, const QString &text, bool general);

class UIDebugVDP2Viewer : public QDialog, public Ui::UIDebugVDP2Viewer
{
    Q_OBJECT
public:
    UIDebugVDP2Viewer(QWidget *parent = 0);
    ~UIDebugVDP2Viewer();
    void addItem(int i);
    void clearItems();
    int  exec() override;
    void showEvent(QShowEvent *) override;
    void refresh();   // called by UIDebugVDP2 on Next Frame

private:
    void displayCurrentScreen();
    void updateVdp2Registers();
    void updateStats();
    void updateColorRam();
    void updateVramHex();
    // Panneau texte a cote de l'image (pteScreenInfo) : reprend les memes
    // fonctions Vdp2DebugStatsXXX() que la fenetre VDP2 Debug principale
    // (voir UIDebugVDP2.cpp), pour la couche actuellement selectionnee dans
    // cbScreen. Meme source de verite cote noyau, pas de logique dupliquee.
    void updateLayerInfo(int screenId);
    // Base et taille de la banque VRAM selectionnee dans cbVramBank.
    // Factorise entre updateVramHex() et on_pbVramExport_clicked() pour
    // que l'affichage hexa et le fichier exporte ne puissent pas diverger.
    void vramBankRange(u32 *base, u32 *size) const;
    // Met à jour les données de l'onglet actuellement affiché dans
    // tabWidget (Registers/Debug/Color RAM/VRAM Hex). L'onglet Screen
    // Viewer (index 0) n'est pas géré ici : son rendu est piloté par les
    // signaux de cbScreen et par displayCurrentScreen() dans refresh().
    // Utilisé par refresh(), showEvent() et on_tabWidget_currentChanged()
    // pour éviter la duplication et les incohérences entre ces trois points
    // d'entrée.
    void refreshActiveTab();

protected:
	void wheelEvent(QWheelEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    pixel_t *vdp2texture = NULL;
    int width = 0, height = 0;
    // Copie de l'image actuellement affichée (après le mirroring déjà
    // appliqué dans displayCurrentScreen()), pour que l'inspecteur de
    // pixel (lPixelInfo, sur survol de gvScreen) lise exactement ce que
    // l'utilisateur voit à l'écran sans avoir à refaire cette conversion.
    QImage currentImage;
    // Couche SPRITE : mots bruts du framebuffer VDP1 (R = octet bas, G =
    // octet haut), avant la mise en couleurs de displayCurrentScreen().
    // L'inspecteur de pixel y relit le mot ; currentImage, elle, contient
    // les couleurs reelles.
    QImage mSpriteRaw;

private:
    // Mémorisation de la sélection courante de cbScreen entre un
    // clearItems() et les addItem() qui suivent, pour la restaurer si la
    // couche précédemment affichée est toujours active (cf. clearItems() /
    // addItem() dans le .cpp). Sans ça, updateScreenInfos() (appelé à
    // chaque "Next Frame") faisait revenir la sélection sur la première
    // couche active à chaque frame.
    bool mRestoreScreenId = false;
    int  mScreenIdToRestore = 0;

    // Inspecteur de couleur.
    //  - describeScreenPixel() : texte de lPixelInfo pour un pixel de
    //    l'image (index CRAM redecode a la source par
    //    Vdp2DebugPixelSource(), numero de palette, priorite/CC decodes,
    //    controle de coherence avec la couleur affichee).
    //  - cramIndexAt() : entree de la CRAM sous un point de la scene de
    //    gvColorRam, -1 si entre deux cases.
    //  - cramEntryText() : texte de survol d'une entree CRAM (numero,
    //    palettes 16/256, RGB, mot brut, doublons, couleur 0).
    //  - mSelCram : entree mise en evidence dans l'onglet Color RAM (clic
    //    sur un pixel de l'image ou sur une case), -1 si aucune.
    QString describeScreenPixel(int px, int py, int *cramIndexOut) const;
    int     cramIndexAt(const QPointF &scenePos) const;
    QString cramEntryText(int idx) const;
    QString mCramHeader;
    int     mSelCram = -1;

    // Onglet Color RAM : panneau lateral (pastille, champs, doublons
    // cliquables) pour l'entree survolee, ou a defaut la selection.
    void showCramEntry(int idx);
    void fitColorRam();

    // Onglet Registers : textes complets (le filtre n'agit que sur
    // l'affichage ; l'export ecrit toujours tout).
    void applyRegFilter();
    QString mRawRegsText, mDecRegsText;

    // Onglet VRAM Hex : adresses des tables des couches actives, reprises
    // des lignes "... Address = ..." des Vdp2DebugStatsXXX().
    void updateVramJumps();

protected slots:
    void on_cbScreen_currentIndexChanged(int index);
    void on_pbSaveAsBitmap_clicked();
    void on_pbDumpLayers_clicked();
    void on_pbImportLayers_clicked();
    void on_pbOpenEditor_clicked();
    void on_pbRevertEdits_clicked();
    void on_pbSaveEdits_clicked();
    void on_cbOpaque_toggled(bool enable);
    void on_pbResetZoom_clicked();
    void on_tabWidget_currentChanged(int index);
    void on_cbVramBank_currentIndexChanged(int index);
    void on_pbVramGo_clicked();
    void on_pbVramExport_clicked();
    void on_cbCramHex_toggled(bool checked);
    void on_pbExportDebugInfo_clicked();
    void on_leRegFilter_textChanged(const QString &text);
    void on_cbStatsText_toggled(bool checked);
    void on_cbVramJump_activated(int index);
};

#endif // UIDEBUGVDP2VIEWER_H
