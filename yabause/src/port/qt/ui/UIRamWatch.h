/*  UIRamWatch -- surveillance d'adresses memoire (outil de TAS).

    Une liste d'adresses de la machine emulee, avec un type (8, 16 ou 32
    bits, signe ou non, virgule fixe 16.16) et un libelle, relue plusieurs
    dizaines de fois par seconde pendant que le jeu tourne, et apres chaque
    trame de l'avance image par image. Une valeur qui vient de changer est
    mise en evidence.

    La liste est gardee par jeu (numero de produit de l'IP.BIN) dans les
    reglages, et peut etre enregistree / chargee dans un fichier texte
    (.wch) : une ligne "adresse type libelle" par entree.

    Les lectures passent par DMAMappedMemoryReadByte() (memory.c), comme
    l'editeur hexadecimal : toute adresse du bus du SH-2 maitre, sans effet
    sur la machine.

    This file is part of Yabause (GPL v2 or later). */

#ifndef UIRAMWATCH_H
#define UIRAMWATCH_H

#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class UIYabause;

class UIRamWatch : public QDialog
{
	Q_OBJECT

public:
	enum Type { U8 = 0, S8, U16, S16, U32, S32, Fixed16_16, TypeCount };

	struct Entry
	{
		quint32 address;
		int type;
		QString label;
		// derniere valeur lue (brute), et nombre de rafraichissements pendant
		// lesquels la mise en evidence reste affichee
		quint32 last;
		bool valid;
		int highlight;
	};

	UIRamWatch( UIYabause* parent );

	static QString typeName( int type );
	static int typeSize( int type );

protected:
	void closeEvent( QCloseEvent* e ) override;

private slots:
	void refresh();
	void addEntry();
	void editEntry();
	void removeEntries();
	void moveUp();
	void moveDown();
	void loadFile();
	void saveFile();
	void selectionChanged();

private:
	bool editDialog( Entry& entry, const QString& title );
	void rebuildTable();
	void updateRow( int row, bool force );
	bool readValue( const Entry& entry, quint32* raw ) const;
	QString formatValue( const Entry& entry, quint32 raw ) const;
	QString formatHex( const Entry& entry, quint32 raw ) const;
	QString currentGame() const;
	void loadSettings();
	void saveSettings();
	static QString entryToLine( const Entry& entry );
	static bool lineToEntry( const QString& line, Entry& entry );
	void moveSelected( int delta );

	UIYabause* mMain;
	QTableWidget* mTable;
	QLabel* mStatus;
	QPushButton* mEdit;
	QPushButton* mRemove;
	QPushButton* mUp;
	QPushButton* mDown;
	QTimer* mTimer;
	QList<Entry> mEntries;
	QString mGame;
};

#endif // UIRAMWATCH_H
